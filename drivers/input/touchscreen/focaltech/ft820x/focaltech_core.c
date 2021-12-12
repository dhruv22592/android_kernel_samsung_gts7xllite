/*
 *
 * FocalTech TouchScreen driver.
 *
 * Copyright (c) 2012-2020, FocalTech Systems, Ltd., all rights reserved.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
/*****************************************************************************
 *
 * File Name: focaltech_core.c
 *
 * Author: Focaltech Driver Team
 *
 * Created: 2016-08-08
 *
 * Abstract: entrance for focaltech ts driver
 *
 * Version: V1.0
 *
 *****************************************************************************/

/*****************************************************************************
 * Included header files
 *****************************************************************************/
#include <linux/module.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>
#if defined(CONFIG_FB)
#include <linux/notifier.h>
#include <linux/fb.h>
#elif defined(CONFIG_DRM)
#if defined(CONFIG_DRM_PANEL)
#include <drm/drm_panel.h>
#else
#include <linux/msm_drm_notify.h>
#endif
#elif defined(CONFIG_HAS_EARLYSUSPEND)
#include <linux/earlysuspend.h>
#define FTS_SUSPEND_LEVEL 1     /* Early-suspend level */
#endif
#include "focaltech_core.h"

/*****************************************************************************
 * Private constant and macro definitions using #define
 *****************************************************************************/
#define FTS_DRIVER_NAME                     "fts_ts"
#define FTS_DRIVER_PEN_NAME                 "fts_ts,pen"
#define INTERVAL_READ_REG                   200  /* unit:ms */
#define TIMEOUT_READ_REG                    1000 /* unit:ms */
#if FTS_POWER_SOURCE_CUST_EN
#define FTS_VTG_MIN_UV                      2800000
#define FTS_VTG_MAX_UV                      3300000
#define FTS_I2C_VTG_MIN_UV                  1800000
#define FTS_I2C_VTG_MAX_UV                  1800000
#endif

/*****************************************************************************
 * Global variable or extern global variabls/functions
 *****************************************************************************/
struct fts_ts_data *fts_data;

/*****************************************************************************
 * Static function prototypes
 *****************************************************************************/
static int fts_ts_resume(struct device *dev);

int fts_check_cid(struct fts_ts_data *ts_data, u8 id_h)
{
	int i = 0;
	struct ft_chip_id_t *cid = &ts_data->ic_info.cid;
	u8 cid_h = 0x0;

	if (cid->type == 0)
		return -ENODATA;

	for (i = 0; i < FTS_MAX_CHIP_IDS; i++) {
		cid_h = ((cid->chip_ids[i] >> 8) & 0x00FF);
		if (cid_h && (id_h == cid_h)) {
			return 0;
		}
	}

	return -ENODATA;
}

/*****************************************************************************
 *  Name: fts_wait_tp_to_valid
 *  Brief: Read chip id until TP FW become valid(Timeout: TIMEOUT_READ_REG),
 *         need call when reset/power on/resume...
 *  Input:
 *  Output:
 *  Return: return 0 if tp valid, otherwise return error code
 *****************************************************************************/
int fts_wait_tp_to_valid(void)
{
	int ret = 0;
	int cnt = 0;
	u8 idh = 0;
	struct fts_ts_data *ts_data = fts_data;
	u8 chip_idh = ts_data->ic_info.ids.chip_idh;

	FTS_FUNC_ENTER();
	do {
		ret = fts_read_reg(FTS_REG_CHIP_ID, &idh);
		if ((idh == chip_idh) || (fts_check_cid(ts_data, idh) == 0)) {
			FTS_INFO("TP Ready,Device ID:0x%02x", idh);
			return 0;
		} else
			FTS_DEBUG("TP Not Ready,ReadData:0x%02x,ret:%d", idh, ret);

		cnt++;
		sec_delay(INTERVAL_READ_REG);
	} while ((cnt * INTERVAL_READ_REG) < TIMEOUT_READ_REG);
	FTS_FUNC_EXIT();

	return -EIO;
}

/*****************************************************************************
 *  Name: fts_tp_state_recovery
 *  Brief: Need execute this function when reset
 *  Input:
 *  Output:
 *  Return:
 *****************************************************************************/
void fts_tp_state_recovery(struct fts_ts_data *ts_data)
{
	FTS_FUNC_ENTER();
	/* wait tp stable */
	fts_wait_tp_to_valid();
	/* recover TP charger state 0x8B */
	/* recover TP glove state 0xC0 */
	/* recover TP cover state 0xC1 */
	fts_ex_mode_recovery(ts_data);
	/* recover TP gesture state 0xD0 */
	fts_gesture_recovery(ts_data);
	FTS_FUNC_EXIT();
}

int fts_reset_proc(int hdelayms)
{
	FTS_DEBUG("tp reset");
	gpio_direction_output(fts_data->pdata->reset_gpio, 0);
	sec_delay(1);
	gpio_direction_output(fts_data->pdata->reset_gpio, 1);
	sec_delay(hdelayms);

	return 0;
}

void fts_irq_disable(void)
{
	mutex_lock(&fts_data->irq_lock);
	if (!fts_data->irq_disabled) {
		FTS_FUNC_ENTER();
		disable_irq_nosync(fts_data->irq);
		fts_data->irq_disabled = true;
	}
	mutex_unlock(&fts_data->irq_lock);
}

void fts_irq_enable(void)
{
	mutex_lock(&fts_data->irq_lock);
	if (fts_data->irq_disabled) {
		FTS_FUNC_ENTER();
		enable_irq(fts_data->irq);
		fts_data->irq_disabled = false;
	}
	mutex_unlock(&fts_data->irq_lock);
}

void fts_hid2std(void)
{
	int ret = 0;
	u8 buf[3] = {0xEB, 0xAA, 0x09};

	if (fts_data->bus_type != BUS_TYPE_I2C)
		return;

	ret = fts_write(buf, 3);
	if (ret < 0) {
		FTS_ERROR("hid2std cmd write fail");
	} else {
		sec_delay(10);
		buf[0] = buf[1] = buf[2] = 0;
		ret = fts_read(NULL, 0, buf, 3);
		if (ret < 0) {
			FTS_ERROR("hid2std cmd read fail");
		} else if ((0xEB == buf[0]) && (0xAA == buf[1]) && (0x08 == buf[2])) {
			FTS_DEBUG("hidi2c change to stdi2c successful");
		} else {
			FTS_DEBUG("hidi2c change to stdi2c not support or fail");
		}
	}
}

static int fts_match_cid(struct fts_ts_data *ts_data,
		u16 type, u8 id_h, u8 id_l, bool force)
{
#ifdef FTS_CHIP_ID_MAPPING
	u32 i = 0;
	u32 j = 0;
	struct ft_chip_id_t chip_id_list[] = FTS_CHIP_ID_MAPPING;
	u32 cid_entries = sizeof(chip_id_list) / sizeof(struct ft_chip_id_t);
	u16 id = (id_h << 8) + id_l;

	memset(&ts_data->ic_info.cid, 0, sizeof(struct ft_chip_id_t));
	for (i = 0; i < cid_entries; i++) {
		if (!force && (type == chip_id_list[i].type)) {
			break;
		} else if (force && (type == chip_id_list[i].type)) {
			FTS_INFO("match cid,type:0x%x", (int)chip_id_list[i].type);
			ts_data->ic_info.cid = chip_id_list[i];
			return 0;
		}
	}

	if (i >= cid_entries) {
		return -ENODATA;
	}

	for (j = 0; j < FTS_MAX_CHIP_IDS; j++) {
		if (id == chip_id_list[i].chip_ids[j]) {
			FTS_DEBUG("cid:%x==%x", id, chip_id_list[i].chip_ids[j]);
			FTS_INFO("match cid,type:0x%x", (int)chip_id_list[i].type);
			ts_data->ic_info.cid = chip_id_list[i];
			return 0;
		}
	}

	return -ENODATA;
#else
	return -EINVAL;
#endif
}


static int fts_get_chip_types(
		struct fts_ts_data *ts_data,
		u8 id_h, u8 id_l, bool fw_valid)
{
	u32 i = 0;
	struct ft_chip_t ctype[] = FTS_CHIP_TYPE_MAPPING;
	u32 ctype_entries = sizeof(ctype) / sizeof(struct ft_chip_t);

	if ((0x0 == id_h) || (0x0 == id_l)) {
		FTS_ERROR("id_h/id_l is 0");
		return -EINVAL;
	}

	FTS_DEBUG("verify id:0x%02x%02x", id_h, id_l);
	for (i = 0; i < ctype_entries; i++) {
		if (VALID == fw_valid) {
			if (((id_h == ctype[i].chip_idh) && (id_l == ctype[i].chip_idl))
					|| (!fts_match_cid(ts_data, ctype[i].type, id_h, id_l, 0)))
				break;
		} else {
			if (((id_h == ctype[i].rom_idh) && (id_l == ctype[i].rom_idl))
					|| ((id_h == ctype[i].pb_idh) && (id_l == ctype[i].pb_idl))
					|| ((id_h == ctype[i].bl_idh) && (id_l == ctype[i].bl_idl))) {
				break;
			}
		}
	}

	if (i >= ctype_entries) {
		return -ENODATA;
	}

	fts_match_cid(ts_data, ctype[i].type, id_h, id_l, 1);
	ts_data->ic_info.ids = ctype[i];
	return 0;
}

static int fts_read_bootid(struct fts_ts_data *ts_data, u8 *id)
{
	int ret = 0;
	u8 chip_id[2] = { 0 };
	u8 id_cmd[4] = { 0 };
	u32 id_cmd_len = 0;

	id_cmd[0] = FTS_CMD_START1;
	id_cmd[1] = FTS_CMD_START2;
	ret = fts_write(id_cmd, 2);
	if (ret < 0) {
		FTS_ERROR("start cmd write fail");
		return ret;
	}

	sec_delay(FTS_CMD_START_DELAY);
	id_cmd[0] = FTS_CMD_READ_ID;
	id_cmd[1] = id_cmd[2] = id_cmd[3] = 0x00;
	if (ts_data->ic_info.is_incell)
		id_cmd_len = FTS_CMD_READ_ID_LEN_INCELL;
	else
		id_cmd_len = FTS_CMD_READ_ID_LEN;
	ret = fts_read(id_cmd, id_cmd_len, chip_id, 2);
	if ((ret < 0) || (0x0 == chip_id[0]) || (0x0 == chip_id[1])) {
		FTS_ERROR("read boot id fail,read:0x%02x%02x", chip_id[0], chip_id[1]);
		return -EIO;
	}

	id[0] = chip_id[0];
	id[1] = chip_id[1];
	return 0;
}

/*****************************************************************************
 * Name: fts_get_ic_information
 * Brief: read chip id to get ic information, after run the function, driver w-
 *        ill know which IC is it.
 *        If cant get the ic information, maybe not focaltech's touch IC, need
 *        unregister the driver
 * Input:
 * Output:
 * Return: return 0 if get correct ic information, otherwise return error code
 *****************************************************************************/
static int fts_get_ic_information(struct fts_ts_data *ts_data)
{
	int ret = 0;
	int cnt = 0;
	u8 chip_id[2] = { 0 };

	ts_data->ic_info.is_incell = FTS_CHIP_IDC;
	ts_data->ic_info.hid_supported = FTS_HID_SUPPORTTED;

	FTS_FUNC_ENTER();
	for (cnt = 0; cnt < 3; cnt++) {
		fts_reset_proc(0);
		sec_delay(FTS_CMD_START_DELAY);

		ret = fts_read_bootid(ts_data, &chip_id[0]);
		if (ret <  0) {
			FTS_DEBUG("read boot id fail,retry:%d", cnt);
			continue;
		}

		ret = fts_get_chip_types(ts_data, chip_id[0], chip_id[1], INVALID);
		if (ret < 0) {
			FTS_DEBUG("can't get ic informaton,retry:%d", cnt);
			continue;
		}

		break;
	}

	if (cnt >= 3) {
		FTS_ERROR("get ic informaton fail");
		return -EIO;
	}


	FTS_INFO("get ic information, chip id = 0x%02x%02x(cid type=0x%x)",
			ts_data->ic_info.ids.chip_idh, ts_data->ic_info.ids.chip_idl,
			ts_data->ic_info.cid.type);

	FTS_FUNC_EXIT();
	return 0;
}

/*****************************************************************************
 *  Reprot related
 *****************************************************************************/
static void fts_show_touch_buffer(u8 *data, int datalen)
{
	int i = 0;
	int count = 0;
	char *tmpbuf = NULL;

	tmpbuf = kzalloc(1024, GFP_KERNEL);
	if (!tmpbuf) {
		FTS_ERROR("tmpbuf zalloc fail");
		return;
	}

	for (i = 0; i < datalen; i++) {
		count += snprintf(tmpbuf + count, 1024 - count, "%02X,", data[i]);
		if (count >= 1024)
			break;
	}
	FTS_DEBUG("point buffer:%s", tmpbuf);

	if (tmpbuf) {
		kfree(tmpbuf);
		tmpbuf = NULL;
	}
}

void fts_release_all_finger(void)
{
	struct fts_ts_data *ts_data = fts_data;
	struct input_dev *input_dev = ts_data->input_dev;
#if FTS_MT_PROTOCOL_B_EN
	u32 finger_count = 0;
	u32 max_touches = ts_data->pdata->max_touch_number;
#endif

	mutex_lock(&ts_data->report_mutex);

	if (ts_data->prox_power_off == 1) {
		input_report_key(input_dev, KEY_INT_CANCEL, 1);
		input_sync(input_dev);
		input_report_key(input_dev, KEY_INT_CANCEL, 0);
		input_sync(input_dev);
	}

#if FTS_MT_PROTOCOL_B_EN
	for (finger_count = 0; finger_count < max_touches; finger_count++) {
		input_mt_slot(input_dev, finger_count);
		input_mt_report_slot_state(input_dev, MT_TOOL_FINGER, false);

		if (ts_data->touchs & BIT(finger_count))
			FTS_INFO("[RA] tID:%d", finger_count);
	}
#else
	input_mt_sync(input_dev);
#endif
	input_report_key(input_dev, BTN_TOUCH, 0);
	input_report_key(input_dev, BTN_PALM, 0);
	input_sync(input_dev);

#if FTS_PEN_EN
	input_report_key(ts_data->pen_dev, BTN_TOOL_PEN, 0);
	input_report_key(ts_data->pen_dev, BTN_TOUCH, 0);
	input_sync(ts_data->pen_dev);
#endif

	ts_data->touchs = 0;
	ts_data->key_state = 0;
	ts_data->palm_flag = 0;
	mutex_unlock(&ts_data->report_mutex);
}

/*****************************************************************************
 * Name: fts_input_report_key
 * Brief: process key events,need report key-event if key enable.
 *        if point's coordinate is in (x_dim-50,y_dim-50) ~ (x_dim+50,y_dim+50),
 *        need report it to key event.
 *        x_dim: parse from dts, means key x_coordinate, dimension:+-50
 *        y_dim: parse from dts, means key y_coordinate, dimension:+-50
 * Input:
 * Output:
 * Return: return 0 if it's key event, otherwise return error code
 *****************************************************************************/
static int fts_input_report_key(struct fts_ts_data *data, int index)
{
	int i = 0;
	int x = data->events[index].x;
	int y = data->events[index].y;
	int *x_dim = &data->pdata->key_x_coords[0];
	int *y_dim = &data->pdata->key_y_coords[0];

	if (!data->pdata->have_key) {
		return -EINVAL;
	}
	for (i = 0; i < data->pdata->key_number; i++) {
		if ((x >= x_dim[i] - FTS_KEY_DIM) && (x <= x_dim[i] + FTS_KEY_DIM) &&
				(y >= y_dim[i] - FTS_KEY_DIM) && (y <= y_dim[i] + FTS_KEY_DIM)) {
			if (EVENT_DOWN(data->events[index].flag)
					&& !(data->key_state & (1 << i))) {
				input_report_key(data->input_dev, data->pdata->keys[i], 1);
				data->key_state |= (1 << i);
				FTS_DEBUG("Key%d(%d,%d) DOWN!", i, x, y);
			} else if (EVENT_UP(data->events[index].flag)
					&& (data->key_state & (1 << i))) {
				input_report_key(data->input_dev, data->pdata->keys[i], 0);
				data->key_state &= ~(1 << i);
				FTS_DEBUG("Key%d(%d,%d) Up!", i, x, y);
			}
			return 0;
		}
	}
	return -EINVAL;
}

static void location_detect(struct fts_ts_data *ts, char *loc, int x, int y)
{
	memset(loc, 0, SEC_TS_LOCATION_DETECT_SIZE);

	if (ts->pdata) {
		if (x < ts->pdata->area_edge)
			strncat(loc, "E.", 2);
		else if (x < (ts->pdata->x_max - ts->pdata->area_edge))
			strncat(loc, "C.", 2);
		else
			strncat(loc, "e.", 2);

		if (y < ts->pdata->area_indicator)
			strncat(loc, "S", 1);
		else if (y < (ts->pdata->y_max - ts->pdata->area_navigation))
			strncat(loc, "C", 1);
		else
			strncat(loc, "N", 1);
	}
}

#if FTS_MT_PROTOCOL_B_EN
static int fts_input_report_b(struct fts_ts_data *data)
{
	int i = 0;
	unsigned long touchs = 0;
	bool va_reported = false;
	u32 max_touch_num = data->pdata->max_touch_number;
	struct ts_event *events = data->events;
	char loc[SEC_TS_LOCATION_DETECT_SIZE] = { 0 };

	for (i = 0; i < data->touch_point; i++) {
		if (fts_input_report_key(data, i) == 0) {
			continue;
		}

		va_reported = true;
		input_mt_slot(data->input_dev, events[i].id);

		if (EVENT_DOWN(events[i].flag)) {
			input_mt_report_slot_state(data->input_dev, MT_TOOL_FINGER, true);

#if FTS_REPORT_PRESSURE_EN
			if (events[i].p <= 0) {
				events[i].p = 0x3f;
			}
			input_report_abs(data->input_dev, ABS_MT_PRESSURE, events[i].p);
#endif
//			if (events[i].area <= 0) {
//				events[i].area = 0x09;
//			}
			input_report_abs(data->input_dev, ABS_MT_TOUCH_MAJOR, events[i].area);
			input_report_abs(data->input_dev, ABS_MT_TOUCH_MINOR, events[i].area_minor);
			input_report_abs(data->input_dev, ABS_MT_POSITION_X, events[i].x);
			input_report_abs(data->input_dev, ABS_MT_POSITION_Y, events[i].y);
			input_report_key(data->input_dev, BTN_PALM, data->palm_flag);

			touchs |= BIT(events[i].id);
			data->touchs |= BIT(events[i].id);

			if (events[i].flag == FTS_TOUCH_DOWN) {
				location_detect(data, loc, events[i].x, events[i].y);
				events[i].p_x = events[i].x;
				events[i].p_y = events[i].y;
				events[i].mcount = 0;
#if !IS_ENABLED(CONFIG_SAMSUNG_PRODUCT_SHIP)
				FTS_INFO("[P] tID:%d.%d x:%d y:%d z:%d p:%d major:%d minor:%d loc:%s tc:%d",
						events[i].id, (data->input_dev->mt->trkid - 1) & TRKID_MAX,
						events[i].x, events[i].y,
						events[i].p, events[i].palm,
						events[i].area, events[i].area_minor, loc,
						FTS_TOUCH_COUNT(&touchs));
#else
				FTS_INFO("[P] tID:%d.%d z:%d p:%d major:%d minor:%d loc:%s tc:%d",
						events[i].id, (data->input_dev->mt->trkid - 1) & TRKID_MAX,
						events[i].p, events[i].palm,
						events[i].area, events[i].area_minor, loc,
						FTS_TOUCH_COUNT(&touchs));
#endif
			} else if (events[i].flag == FTS_TOUCH_CONTACT) {
				events[i].mcount++;
			}
		} else {
			input_mt_report_slot_state(data->input_dev, MT_TOOL_FINGER, false);
			data->touchs &= ~BIT(events[i].id);
			location_detect(data, loc, events[i].x, events[i].y);

			data->palm_flag &= ~BIT(events[i].id);
			input_report_key(data->input_dev, BTN_PALM, data->palm_flag);

#if !IS_ENABLED(CONFIG_SAMSUNG_PRODUCT_SHIP)
			FTS_INFO("[R] tID:%d loc:%s dd:%d,%d pc:%d mc:%d tc:%d lx:%d ly:%d",
					events[i].id, loc, events[i].x - events[i].p_x, events[i].y - events[i].p_y,
					FTS_TOUCH_COUNT(&data->palm_flag),
					events[i].mcount, FTS_TOUCH_COUNT(&touchs),
					events[i].x, events[i].y);
#else
			FTS_INFO("[R] tID:%d loc:%s dd:%d,%d pc:%d mc:%d tc:%d",
					events[i].id, loc, events[i].x - events[i].p_x, events[i].y - events[i].p_y,
					FTS_TOUCH_COUNT(&data->palm_flag),
					events[i].mcount, FTS_TOUCH_COUNT(&touchs));
#endif
		}
	}

	if (unlikely(data->touchs ^ touchs)) {
		for (i = 0; i < max_touch_num; i++)  {
			if (BIT(i) & (data->touchs ^ touchs)) {
				FTS_INFO("[R] tID:%d", i);
				va_reported = true;
				input_mt_slot(data->input_dev, i);
				input_mt_report_slot_state(data->input_dev, MT_TOOL_FINGER, false);
				data->palm_flag &= ~BIT(i);
				input_report_key(data->input_dev, BTN_PALM, data->palm_flag);
			}
		}
	}
	data->touchs = touchs;

	if (va_reported) {
		/* touchs==0, there's no point but key */
		if (EVENT_NO_DOWN(data) || (!touchs)) {
			data->print_info_cnt_release = 0;
			if (data->log_level >= 2)
				FTS_DEBUG("[B]Points All Up!");

			input_report_key(data->input_dev, BTN_TOUCH, 0);
		} else {
			input_report_key(data->input_dev, BTN_TOUCH, 1);
		}
	}

	input_sync(data->input_dev);
	return 0;
}

#else
static int fts_input_report_a(struct fts_ts_data *data)
{
	int i = 0;
	int touchs = 0;
	bool va_reported = false;
	struct ts_event *events = data->events;

	for (i = 0; i < data->touch_point; i++) {
		if (fts_input_report_key(data, i) == 0) {
			continue;
		}

		va_reported = true;
		if (EVENT_DOWN(events[i].flag)) {
			input_report_abs(data->input_dev, ABS_MT_TRACKING_ID, events[i].id);
#if FTS_REPORT_PRESSURE_EN
			if (events[i].p <= 0) {
				events[i].p = 0x3f;
			}
			input_report_abs(data->input_dev, ABS_MT_PRESSURE, events[i].p);
#endif
			if (events[i].area <= 0) {
				events[i].area = 0x09;
			}
			input_report_abs(data->input_dev, ABS_MT_TOUCH_MAJOR, events[i].area);

			input_report_abs(data->input_dev, ABS_MT_POSITION_X, events[i].x);
			input_report_abs(data->input_dev, ABS_MT_POSITION_Y, events[i].y);

			input_mt_sync(data->input_dev);

			if ((data->log_level >= 2) ||
					((1 == data->log_level) && (FTS_TOUCH_DOWN == events[i].flag))) {
				FTS_DEBUG("[A]P%d(%d, %d)[p:%d,tm:%d] DOWN!",
						events[i].id,
						events[i].x, events[i].y,
						events[i].p, events[i].area);
			}
			touchs++;
		}
	}

	/* last point down, current no point but key */
	if (data->touchs && !touchs) {
		va_reported = true;
	}
	data->touchs = touchs;

	if (va_reported) {
		if (EVENT_NO_DOWN(data)) {
			if (data->log_level >= 1) {
				FTS_DEBUG("[A]Points All Up!");
			}
			input_report_key(data->input_dev, BTN_TOUCH, 0);
			input_mt_sync(data->input_dev);
		} else {
			input_report_key(data->input_dev, BTN_TOUCH, 1);
		}
	}

	input_sync(data->input_dev);
	return 0;
}
#endif

#if FTS_PEN_EN
static int fts_input_pen_report(struct fts_ts_data *data)
{
	struct input_dev *pen_dev = data->pen_dev;
	struct pen_event *pevt = &data->pevent;
	u8 *buf = data->point_buf;


	if (buf[3] & 0x08)
		input_report_key(pen_dev, BTN_STYLUS, 1);
	else
		input_report_key(pen_dev, BTN_STYLUS, 0);

	if (buf[3] & 0x02)
		input_report_key(pen_dev, BTN_STYLUS2, 1);
	else
		input_report_key(pen_dev, BTN_STYLUS2, 0);

	pevt->inrange = (buf[3] & 0x20) ? 1 : 0;
	pevt->tip = (buf[3] & 0x01) ? 1 : 0;
	pevt->x = ((buf[4] & 0x0F) << 8) + buf[5];
	pevt->y = ((buf[6] & 0x0F) << 8) + buf[7];
	pevt->p = ((buf[8] & 0x0F) << 8) + buf[9];
	pevt->id = buf[6] >> 4;
	pevt->flag = buf[4] >> 6;
	pevt->tilt_x = (buf[10] << 8) + buf[11];
	pevt->tilt_y = (buf[12] << 8) + buf[13];
	pevt->tool_type = BTN_TOOL_PEN;

	if (data->log_level >= 2  ||
			((1 == data->log_level) && (FTS_TOUCH_DOWN == pevt->flag))) {
		FTS_DEBUG("[PEN]x:%d,y:%d,p:%d,inrange:%d,tip:%d,flag:%d DOWN",
				pevt->x, pevt->y, pevt->p, pevt->inrange,
				pevt->tip, pevt->flag);
	}

	if ((data->log_level >= 1) && (!pevt->inrange)) {
		FTS_DEBUG("[PEN]UP");
	}

	input_report_abs(pen_dev, ABS_X, pevt->x);
	input_report_abs(pen_dev, ABS_Y, pevt->y);
	input_report_abs(pen_dev, ABS_PRESSURE, pevt->p);

	/* check if the pen support tilt event */
	if ((pevt->tilt_x != 0) || (pevt->tilt_y != 0)) {
		input_report_abs(pen_dev, ABS_TILT_X, pevt->tilt_x);
		input_report_abs(pen_dev, ABS_TILT_Y, pevt->tilt_y);
	}

	input_report_key(pen_dev, BTN_TOUCH, pevt->tip);
	input_report_key(pen_dev, BTN_TOOL_PEN, pevt->inrange);
	input_sync(pen_dev);

	return 0;
}
#endif

static int fts_read_touchdata(struct fts_ts_data *data)
{
	int ret = 0;
	u8 *buf = data->point_buf;

	memset(buf, 0xFF, data->pnt_buf_size);
	buf[0] = 0x01;

	ret = fts_read(buf, 1, buf + 1, data->pnt_buf_size - 1);

	if (((0xEF == buf[2]) && (0xEF == buf[3]) && (0xEF == buf[4]))
			|| ((ret < 0) && (0xEF == buf[1]))) {
		fts_release_all_finger();
		/* check if need recovery fw */
		fts_fw_recovery();
		data->fw_is_running = true;
		return 1;
	} else if (ret < 0) {
		FTS_ERROR("touch data(%x) abnormal,ret:%d", buf[1], ret);
		return -EIO;
	}

	if (data->gesture_mode) {
		ret = fts_gesture_readdata(data, buf + FTS_TOUCH_DATA_LEN);
		if (0 == ret) {
			FTS_INFO("succuss to get gesture data in irq handler");
			return 1;
		}
	}

	if (data->log_level >= 3) {
		fts_show_touch_buffer(buf, data->pnt_buf_size);
	}

	return 0;
}


static int fts_read_parse_touchdata(struct fts_ts_data *data)
{
	int ret = 0;
	int i = 0;
	u8 pointid = 0;
	int base = 0;
	struct ts_event *events = data->events;
	int max_touch_num = data->pdata->max_touch_number;
	u8 *buf = data->point_buf;
	unsigned long palm_temp = data->palm_flag;

	ret = fts_read_touchdata(data);
	if (ret) {
		return ret;
	}

#if FTS_PEN_EN
	if ((buf[2] & 0xF0) == 0xB0) {
		fts_input_pen_report(data);
		return 2;
	}
#endif

	data->point_num = buf[FTS_TOUCH_POINT_NUM] & 0x0F;
	data->touch_point = 0;

	if (data->ic_info.is_incell) {
		if ((data->point_num == 0x0F) && (buf[2] == 0xFF) && (buf[3] == 0xFF)
				&& (buf[4] == 0xFF) && (buf[5] == 0xFF) && (buf[6] == 0xFF)) {
			FTS_DEBUG("touch buff is 0xff, need recovery state");
			fts_release_all_finger();
			fts_tp_state_recovery(data);
			data->point_num = 0;
			return -EIO;
		}
	}

	if (data->point_num > max_touch_num) {
		FTS_INFO("invalid point_num(%d)", data->point_num);
		data->point_num = 0;
		return -EIO;
	}

	for (i = 0; i < max_touch_num; i++) {
		base = FTS_ONE_TCH_LEN * i;
		pointid = (buf[FTS_TOUCH_ID_POS + base]) >> 4;
		if (pointid >= FTS_MAX_ID)
			break;
		else if (pointid >= max_touch_num) {
			FTS_ERROR("ID(%d) beyond max_touch_number", pointid);
			return -EINVAL;
		}

		data->touch_point++;
		events[i].x = ((buf[FTS_TOUCH_X_H_POS + base] & 0x0F) << 8) +
			(buf[FTS_TOUCH_X_L_POS + base] & 0xFF);
		events[i].y = ((buf[FTS_TOUCH_Y_H_POS + base] & 0x0F) << 8) +
			(buf[FTS_TOUCH_Y_L_POS + base] & 0xFF);
		events[i].flag = buf[FTS_TOUCH_EVENT_POS + base] >> 6;
		events[i].id = buf[FTS_TOUCH_ID_POS + base] >> 4;
		events[i].area = buf[FTS_TOUCH_AREA_POS + base];
		events[i].palm = (buf[FTS_TOUCH_EVENT_POS + base] >> 4) & 0x01;
		if (events[i].palm)
			data->palm_flag |= BIT(events[i].id);
		else
			data->palm_flag &= ~BIT(events[i].id);
		events[i].area_minor = buf[FTS_TOUCH_PRE_POS + base];
		//events[i].p =  buf[FTS_TOUCH_PRE_POS + base];

		if (palm_temp != data->palm_flag) {
			FTS_INFO("tID:%d palm changed to %d", events[i].id, events[i].palm);
			palm_temp = data->palm_flag;
		}

		if (EVENT_DOWN(events[i].flag) && (data->point_num == 0)) {
			FTS_INFO("abnormal touch data from fw");
			return -EIO;
		}
	}

	if (data->touch_point == 0) {
		FTS_INFO("no touch point information(%02x)", buf[2]);
		return -EIO;
	}

	return 0;
}

static void fts_irq_read_report(void)
{
	int ret = 0;
	struct fts_ts_data *ts_data = fts_data;

#if FTS_ESDCHECK_EN
	fts_esdcheck_set_intr(1);
#endif

#if FTS_POINT_REPORT_CHECK_EN
	fts_prc_queue_work(ts_data);
#endif

	ret = fts_read_parse_touchdata(ts_data);
	if (ret == 0) {
		mutex_lock(&ts_data->report_mutex);
#if FTS_MT_PROTOCOL_B_EN
		fts_input_report_b(ts_data);
#else
		fts_input_report_a(ts_data);
#endif
		mutex_unlock(&ts_data->report_mutex);
	}

#if FTS_ESDCHECK_EN
	fts_esdcheck_set_intr(0);
#endif
}

#if defined(CONFIG_PM) && FTS_PATCH_COMERR_PM
static void fts_wakeup_source_init(struct fts_ts_data *ts_data)
{
	FTS_FUNC_ENTER();
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 19, 110))
	ts_data->wake_lock = wakeup_source_register(ts_data->dev, "tsp");
#else
	wakeup_source_init(ts_data->wake_lock, "tsp");
#endif
	FTS_FUNC_EXIT();
}

static void fts_wakeup_source_unregister(struct fts_ts_data *ts_data)
{
	if (!ts_data->wake_lock)
		return;

	FTS_FUNC_ENTER();
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 19, 110))
	wakeup_source_unregister(ts_data->wake_lock);
#else
	wakeup_source_trash(ts_data->wake_lock);
#endif
	FTS_FUNC_EXIT();
}
#endif

static irqreturn_t fts_irq_handler(int irq, void *data)
{
#if defined(CONFIG_PM) && FTS_PATCH_COMERR_PM
	int ret = 0;
	struct fts_ts_data *ts_data = fts_data;

	if ((ts_data->suspended) && (ts_data->pm_suspend)) {
		__pm_wakeup_event(ts_data->wake_lock, FTS_WAKELOCK_TIME);
		ret = wait_for_completion_timeout(
				&ts_data->pm_completion,
				msecs_to_jiffies(FTS_TIMEOUT_COMERR_PM));
		if (!ret) {
			FTS_ERROR("Bus don't resume from pm(deep),timeout,skip irq");
			return IRQ_HANDLED;
		}
	}
#endif

	fts_irq_read_report();
	return IRQ_HANDLED;
}

static int fts_irq_registration(struct fts_ts_data *ts_data)
{
	int ret = 0;
	struct fts_ts_platform_data *pdata = ts_data->pdata;

	ts_data->irq = gpio_to_irq(pdata->irq_gpio);
	pdata->irq_gpio_flags = IRQF_TRIGGER_FALLING | IRQF_ONESHOT;
	FTS_INFO("irq:%d, flag:%x", ts_data->irq, pdata->irq_gpio_flags);
	ret = request_threaded_irq(ts_data->irq, NULL, fts_irq_handler,
			pdata->irq_gpio_flags,
			FTS_DRIVER_NAME, ts_data);

	return ret;
}

#if FTS_PEN_EN
static int fts_input_pen_init(struct fts_ts_data *ts_data)
{
	int ret = 0;
	struct input_dev *pen_dev;
	struct fts_ts_platform_data *pdata = ts_data->pdata;

	FTS_FUNC_ENTER();
	pen_dev = devm_input_allocate_device(ts_data->dev);
	if (!pen_dev) {
		FTS_ERROR("Failed to allocate memory for input_pen device");
		return -ENOMEM;
	}

	pen_dev->dev.parent = ts_data->dev;
	pen_dev->name = FTS_DRIVER_PEN_NAME;
	pen_dev->evbit[0] |= BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS);
	__set_bit(ABS_X, pen_dev->absbit);
	__set_bit(ABS_Y, pen_dev->absbit);
	__set_bit(BTN_STYLUS, pen_dev->keybit);
	__set_bit(BTN_STYLUS2, pen_dev->keybit);
	__set_bit(BTN_TOUCH, pen_dev->keybit);
	__set_bit(BTN_TOOL_PEN, pen_dev->keybit);
	__set_bit(INPUT_PROP_DIRECT, pen_dev->propbit);
	input_set_abs_params(pen_dev, ABS_X, pdata->x_min, pdata->x_max, 0, 0);
	input_set_abs_params(pen_dev, ABS_Y, pdata->y_min, pdata->y_max, 0, 0);
	input_set_abs_params(pen_dev, ABS_PRESSURE, 0, 4096, 0, 0);

	ret = input_register_device(pen_dev);
	if (ret) {
		FTS_ERROR("Input device registration failed");
		return ret;
	}

	ts_data->pen_dev = pen_dev;
	FTS_FUNC_EXIT();
	return 0;
}
#endif

static int fts_input_set_prop(struct fts_ts_data *ts_data, struct input_dev *input_dev, int propbit)
{
	int key_num = 0;
	int ret;

	if (ts_data->bus_type == BUS_TYPE_I2C)
		input_dev->id.bustype = BUS_I2C;
	else
		input_dev->id.bustype = BUS_SPI;
	input_dev->dev.parent = ts_data->dev;

	set_bit(EV_SYN, input_dev->evbit);
	set_bit(EV_ABS, input_dev->evbit);
	set_bit(EV_KEY, input_dev->evbit);
	set_bit(BTN_TOUCH, input_dev->keybit);
	set_bit(propbit, input_dev->propbit);
	set_bit(KEY_WAKEUP, input_dev->keybit);
	set_bit(KEY_INT_CANCEL, input_dev->keybit);
	set_bit(BTN_PALM, input_dev->keybit);

	if (ts_data->pdata->have_key) {
		FTS_INFO("set key capabilities");
		for (key_num = 0; key_num < ts_data->pdata->key_number; key_num++)
			input_set_capability(input_dev, EV_KEY, ts_data->pdata->keys[key_num]);
	}

#if FTS_MT_PROTOCOL_B_EN
	if (propbit == INPUT_PROP_DIRECT)
		input_mt_init_slots(input_dev, ts_data->pdata->max_touch_number, INPUT_MT_DIRECT);
	else
		input_mt_init_slots(input_dev, ts_data->pdata->max_touch_number, INPUT_MT_POINTER);
#else
	input_set_abs_params(input_dev, ABS_MT_TRACKING_ID, 0, 0x0F, 0, 0);
#endif
	input_set_abs_params(input_dev, ABS_MT_POSITION_X, ts_data->pdata->x_min, ts_data->pdata->x_max, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_POSITION_Y, ts_data->pdata->y_min, ts_data->pdata->y_max, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_TOUCH_MAJOR, 0, 0xFF, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_TOUCH_MINOR, 0, 0xFF, 0, 0);
#if FTS_REPORT_PRESSURE_EN
	if (propbit == INPUT_PROP_DIRECT)
		input_set_abs_params(input_dev, ABS_MT_PRESSURE, 0, 0xFF, 0, 0);
#endif

	ret = input_register_device(input_dev);
	if (ret) {
		FTS_ERROR("Input device registration failed");
		input_set_drvdata(input_dev, NULL);
		return ret;
	}

	return 0;
}

static int fts_input_init(struct fts_ts_data *ts_data)
{
	int ret = 0;

	FTS_FUNC_ENTER();
	ts_data->input_dev = devm_input_allocate_device(ts_data->dev);
	if (!ts_data->input_dev) {
		FTS_ERROR("Failed to allocate memory for input device");
		return -ENOMEM;
	}

	/* Init and register Input device */
	ts_data->input_dev->name = "sec_touchscreen";
	input_set_drvdata(ts_data->input_dev, ts_data);
	ret = fts_input_set_prop(ts_data, ts_data->input_dev, INPUT_PROP_DIRECT);
	if (ret)
		return ret;

#if FTS_PEN_EN
	ret = fts_input_pen_init(ts_data);
	if (ret) {
		FTS_ERROR("Input-pen device registration failed");
		input_set_drvdata(ts_data->input_dev, NULL);
		return ret;
	}
#endif

	if (ts_data->pdata->support_dex) {
		ts_data->input_dev_pad = devm_input_allocate_device(ts_data->dev);
		if (!ts_data->input_dev_pad) {
			FTS_ERROR("Failed to allocate memory for input pad device");
			return 0;
		}
		ts_data->input_dev_pad->name = "sec_touchpad";
		fts_input_set_prop(ts_data, ts_data->input_dev_pad, INPUT_PROP_POINTER);
	}

	FTS_FUNC_EXIT();
	return 0;
}

static int fts_report_buffer_init(struct fts_ts_data *ts_data)
{
	int point_num = 0;
	int events_num = 0;

	point_num = FTS_MAX_POINTS_SUPPORT;
	ts_data->pnt_buf_size = FTS_TOUCH_DATA_LEN + FTS_GESTURE_DATA_LEN;

	ts_data->point_buf = (u8 *)devm_kzalloc(ts_data->dev, ts_data->pnt_buf_size + 1, GFP_KERNEL);
	if (!ts_data->point_buf) {
		FTS_ERROR("failed to alloc memory for point buf");
		return -ENOMEM;
	}

	events_num = point_num * sizeof(struct ts_event);
	ts_data->events = (struct ts_event *)devm_kzalloc(ts_data->dev, events_num, GFP_KERNEL);
	if (!ts_data->events) {
		FTS_ERROR("failed to alloc memory for point events");
		return -ENOMEM;
	}

	return 0;
}

int fts_ctrl_lcd_reset_regulator(struct fts_ts_data *ts, bool on)
{
	static struct regulator *panel_reset;
	static bool enabled;
	int ret = 0;

	if (!ts->pdata->panel_reset)
		return 0;

	FTS_INFO("called! %s, enabled(%d)", on ? "on" : "off", enabled);

	if (enabled == on)
		return ret;

	if (IS_ERR_OR_NULL(panel_reset)) {
		panel_reset = devm_regulator_get(ts->dev, ts->pdata->panel_reset);
		if (IS_ERR_OR_NULL(panel_reset)) {
			FTS_ERROR("Failed to get panel_reset(%s) regulator", ts->pdata->panel_reset);
			ret = PTR_ERR(panel_reset);
			goto error;
		}
	}

	FTS_INFO("%s: [BEFORE] reset:%d", on ? "on" : "off", regulator_is_enabled(panel_reset));

	if (on) {
		ret = regulator_enable(panel_reset);
		if (ret) {
			FTS_ERROR("Failed to enable panel_reset: %d", ret);
			goto out;
		}
	} else {
		regulator_disable(panel_reset);
	}

	enabled = on;

out:
	FTS_INFO("%s: [AFTER] reset:%d", on ? "on" : "off", regulator_is_enabled(panel_reset));

error:
	return ret;

}

int fts_ctrl_lcd_regulators(struct fts_ts_data *ts, bool on)
{
	static struct regulator *panel_buck_en;
	static struct regulator *panel_buck_en2;
	static struct regulator *panel_ldo_en;
	static bool enabled;
	int ret = 0;

	if (!ts->pdata->panel_buck_en || !ts->pdata->panel_buck_en2 || !ts->pdata->panel_ldo_en)
		return 0;

	FTS_INFO("called! %s, enabled(%d)", on ? "on" : "off", enabled);

	if (enabled == on)
		return ret;

	if (IS_ERR_OR_NULL(panel_buck_en)) {
		panel_buck_en = devm_regulator_get(ts->dev, ts->pdata->panel_buck_en);
		if (IS_ERR_OR_NULL(panel_buck_en)) {
			FTS_ERROR("Failed to get panel_buck_en(%s) regulator", ts->pdata->panel_buck_en);
			ret = PTR_ERR(panel_buck_en);
			goto error;
		}
	}

	if (IS_ERR_OR_NULL(panel_buck_en2)) {
		panel_buck_en2 = devm_regulator_get(ts->dev, ts->pdata->panel_buck_en2);
		if (IS_ERR_OR_NULL(panel_buck_en2)) {
			FTS_ERROR("Failed to get panel_buck_en2(%s) regulator", ts->pdata->panel_buck_en2);
			ret = PTR_ERR(panel_buck_en2);
			goto error;
		}
	}

	if (IS_ERR_OR_NULL(panel_ldo_en)) {
		panel_ldo_en = devm_regulator_get(ts->dev, ts->pdata->panel_ldo_en);
		if (IS_ERR_OR_NULL(panel_ldo_en)) {
			FTS_ERROR("Failed to get panel_ldo_en(%s) regulator", ts->pdata->panel_ldo_en);
			ret = PTR_ERR(panel_ldo_en);
			goto error;
		}
	}

	FTS_INFO("%s: [BEFORE] lcd_pwr1:%d,lcd_pwr2:%d,lcd_ldo:%d", on ? "on" : "off",
			regulator_is_enabled(panel_buck_en),
			regulator_is_enabled(panel_buck_en2),
			regulator_is_enabled(panel_ldo_en));

	if (on) {
		ret = regulator_enable(panel_ldo_en);
		if (ret) {
			FTS_ERROR("Failed to enable panel_ldo_en: %d", ret);
			goto out;
		}
		ret = regulator_enable(panel_buck_en2);
		if (ret) {
			FTS_ERROR("Failed to enable panel_buck_en2: %d", ret);
			goto out;
		}
		ret = regulator_enable(panel_buck_en);
		if (ret) {
			FTS_ERROR("Failed to enable panel_buck_en: %d", ret);
			goto out;
		}
	} else {
		regulator_disable(panel_buck_en);
		regulator_disable(panel_buck_en2);
		regulator_disable(panel_ldo_en);
	}

	enabled = on;

out:
	FTS_INFO("%s: [AFTER] lcd_pwr1:%d,lcd_pwr2:%d,lcd_ldo:%d", on ? "on" : "off",
			regulator_is_enabled(panel_buck_en),
			regulator_is_enabled(panel_buck_en2),
			regulator_is_enabled(panel_ldo_en));

error:
	return ret;
}


#if FTS_PINCTRL_EN
static int fts_pinctrl_init(struct fts_ts_data *ts)
{
	int ret = 0;

	FTS_FUNC_ENTER();
	ts->pinctrl = devm_pinctrl_get(ts->dev);
	if (IS_ERR_OR_NULL(ts->pinctrl)) {
		FTS_ERROR("Failed to get pinctrl, please check dts");
		ret = PTR_ERR(ts->pinctrl);
		goto err_pinctrl_get;
	}

	ts->pins_active = pinctrl_lookup_state(ts->pinctrl, "pmx_ts_active");
	if (IS_ERR_OR_NULL(ts->pins_active)) {
		FTS_ERROR("Pin state[active] not found");
		ret = PTR_ERR(ts->pins_active);
		goto err_pinctrl_lookup;
	}

	ts->pins_suspend = pinctrl_lookup_state(ts->pinctrl, "pmx_ts_suspend");
	if (IS_ERR_OR_NULL(ts->pins_suspend)) {
		FTS_ERROR("Pin state[suspend] not found");
		ret = PTR_ERR(ts->pins_suspend);
		goto err_pinctrl_lookup;
	}

	ts->pins_release = pinctrl_lookup_state(ts->pinctrl, "pmx_ts_release");
	if (IS_ERR_OR_NULL(ts->pins_release)) {
		FTS_ERROR("Pin state[release] not found");
		ret = PTR_ERR(ts->pins_release);
	}

	FTS_FUNC_EXIT();
	return 0;
err_pinctrl_lookup:
	if (ts->pinctrl) {
		devm_pinctrl_put(ts->pinctrl);
	}
err_pinctrl_get:
	ts->pinctrl = NULL;
	ts->pins_release = NULL;
	ts->pins_suspend = NULL;
	ts->pins_active = NULL;
	return ret;
}

static int fts_pinctrl_select_normal(struct fts_ts_data *ts)
{
	int ret = 0;

	if (ts->pinctrl && ts->pins_active) {
		ret = pinctrl_select_state(ts->pinctrl, ts->pins_active);
		if (ret < 0) {
			FTS_ERROR("Set normal pin state error:%d", ret);
		} else {
			FTS_INFO("success");
		}
	}

	return ret;
}

static int fts_pinctrl_select_suspend(struct fts_ts_data *ts)
{
	int ret = 0;

	if (ts->pinctrl && ts->pins_suspend) {
		ret = pinctrl_select_state(ts->pinctrl, ts->pins_suspend);
		if (ret < 0) {
			FTS_ERROR("Set suspend pin state error:%d", ret);
		} else {
			FTS_INFO("success");
		}
	}

	return ret;
}

static int fts_pinctrl_select_release(struct fts_ts_data *ts)
{
	int ret = 0;

	if (ts->pinctrl) {
		if (IS_ERR_OR_NULL(ts->pins_release)) {
			devm_pinctrl_put(ts->pinctrl);
			ts->pinctrl = NULL;
		} else {
			ret = pinctrl_select_state(ts->pinctrl, ts->pins_release);
			if (ret < 0)
				FTS_ERROR("Set gesture pin state error:%d", ret);
			else
				FTS_INFO("success");
		}
	}

	return ret;
}
#endif /* FTS_PINCTRL_EN */

#if FTS_POWER_SOURCE_CUST_EN
/*****************************************************************************
 * Power Control
 *****************************************************************************/
static int fts_power_source_ctrl(struct fts_ts_data *ts_data, int enable)
{
	int ret = 0;

	if (IS_ERR_OR_NULL(ts_data->vdd)) {
		FTS_ERROR("vdd is invalid");
		return -EINVAL;
	}

	FTS_FUNC_ENTER();
	if (enable) {
		if (ts_data->power_disabled) {
			FTS_DEBUG("regulator enable !");
			gpio_direction_output(ts_data->pdata->reset_gpio, 0);
			sec_delay(1);
			ret = regulator_enable(ts_data->vdd);
			if (ret) {
				FTS_ERROR("enable vdd regulator failed,ret=%d", ret);
			}

			if (!IS_ERR_OR_NULL(ts_data->vcc_i2c)) {
				ret = regulator_enable(ts_data->vcc_i2c);
				if (ret) {
					FTS_ERROR("enable vcc_i2c regulator failed,ret=%d", ret);
				}
			}
			ts_data->power_disabled = false;
		}
	} else {
		if (!ts_data->power_disabled) {
			FTS_DEBUG("regulator disable !");
			gpio_direction_output(ts_data->pdata->reset_gpio, 0);
			sec_delay(1);
			ret = regulator_disable(ts_data->vdd);
			if (ret) {
				FTS_ERROR("disable vdd regulator failed,ret=%d", ret);
			}
			if (!IS_ERR_OR_NULL(ts_data->vcc_i2c)) {
				ret = regulator_disable(ts_data->vcc_i2c);
				if (ret) {
					FTS_ERROR("disable vcc_i2c regulator failed,ret=%d", ret);
				}
			}
			ts_data->power_disabled = true;
		}
	}

	FTS_FUNC_EXIT();
	return ret;
}

/*****************************************************************************
 * Name: fts_power_source_init
 * Brief: Init regulator power:vdd/vcc_io(if have), generally, no vcc_io
 *        vdd---->vdd-supply in dts, kernel will auto add "-supply" to parse
 *        Must be call after fts_gpio_configure() execute,because this function
 *        will operate reset-gpio which request gpio in fts_gpio_configure()
 * Input:
 * Output:
 * Return: return 0 if init power successfully, otherwise return error code
 *****************************************************************************/
static int fts_power_source_init(struct fts_ts_data *ts_data)
{
	int ret = 0;

	FTS_FUNC_ENTER();
	ts_data->vdd = regulator_get(ts_data->dev, "vdd");
	if (IS_ERR_OR_NULL(ts_data->vdd)) {
		ret = PTR_ERR(ts_data->vdd);
		FTS_ERROR("get vdd regulator failed,ret=%d", ret);
		return ret;
	}

	if (regulator_count_voltages(ts_data->vdd) > 0) {
		ret = regulator_set_voltage(ts_data->vdd, FTS_VTG_MIN_UV,
				FTS_VTG_MAX_UV);
		if (ret) {
			FTS_ERROR("vdd regulator set_vtg failed ret=%d", ret);
			regulator_put(ts_data->vdd);
			return ret;
		}
	}

	ts_data->vcc_i2c = regulator_get(ts_data->dev, "vcc_i2c");
	if (!IS_ERR_OR_NULL(ts_data->vcc_i2c)) {
		if (regulator_count_voltages(ts_data->vcc_i2c) > 0) {
			ret = regulator_set_voltage(ts_data->vcc_i2c,
					FTS_I2C_VTG_MIN_UV,
					FTS_I2C_VTG_MAX_UV);
			if (ret) {
				FTS_ERROR("vcc_i2c regulator set_vtg failed,ret=%d", ret);
				regulator_put(ts_data->vcc_i2c);
			}
		}
	}

	ts_data->power_disabled = true;
	ret = fts_power_source_ctrl(ts_data, ENABLE);
	if (ret) {
		FTS_ERROR("fail to enable power(regulator)");
	}

	FTS_FUNC_EXIT();
	return ret;
}

static int fts_power_source_exit(struct fts_ts_data *ts_data)
{
	fts_power_source_ctrl(ts_data, DISABLE);

	if (!IS_ERR_OR_NULL(ts_data->vdd)) {
		if (regulator_count_voltages(ts_data->vdd) > 0)
			regulator_set_voltage(ts_data->vdd, 0, FTS_VTG_MAX_UV);
		regulator_put(ts_data->vdd);
	}

	if (!IS_ERR_OR_NULL(ts_data->vcc_i2c)) {
		if (regulator_count_voltages(ts_data->vcc_i2c) > 0)
			regulator_set_voltage(ts_data->vcc_i2c, 0, FTS_I2C_VTG_MAX_UV);
		regulator_put(ts_data->vcc_i2c);
	}

	return 0;
}

static int fts_power_source_suspend(struct fts_ts_data *ts_data)
{
	int ret = 0;

	ret = fts_power_source_ctrl(ts_data, DISABLE);
	if (ret < 0) {
		FTS_ERROR("power off fail, ret=%d", ret);
	}

	return ret;
}

static int fts_power_source_resume(struct fts_ts_data *ts_data)
{
	int ret = 0;

	ret = fts_power_source_ctrl(ts_data, ENABLE);
	if (ret < 0) {
		FTS_ERROR("power on fail, ret=%d", ret);
	}

	return ret;
}
#endif /* FTS_POWER_SOURCE_CUST_EN */

static int fts_gpio_configure(struct fts_ts_data *data)
{
	int ret = 0;

	FTS_FUNC_ENTER();
	/* request irq gpio */
	if (gpio_is_valid(data->pdata->irq_gpio)) {
		ret = gpio_request(data->pdata->irq_gpio, "fts_irq_gpio");
		if (ret) {
			FTS_ERROR("[GPIO]irq gpio request failed");
			goto err_irq_gpio_req;
		}

		ret = gpio_direction_input(data->pdata->irq_gpio);
		if (ret) {
			FTS_ERROR("[GPIO]set_direction for irq gpio failed");
			goto err_irq_gpio_dir;
		}
	}

	/* request reset gpio */
	if (gpio_is_valid(data->pdata->reset_gpio)) {
		ret = gpio_request(data->pdata->reset_gpio, "fts_reset_gpio");
		if (ret) {
			FTS_ERROR("[GPIO]reset gpio request failed");
			goto err_irq_gpio_dir;
		}

		ret = gpio_direction_output(data->pdata->reset_gpio, 1);
		if (ret) {
			FTS_ERROR("[GPIO]set_direction for reset gpio failed");
			goto err_reset_gpio_dir;
		}
	}

	FTS_FUNC_EXIT();
	return 0;

err_reset_gpio_dir:
	if (gpio_is_valid(data->pdata->reset_gpio))
		gpio_free(data->pdata->reset_gpio);
err_irq_gpio_dir:
	if (gpio_is_valid(data->pdata->irq_gpio))
		gpio_free(data->pdata->irq_gpio);
err_irq_gpio_req:
	FTS_FUNC_EXIT();
	return ret;
}

static int fts_get_dt_coords(struct device *dev, char *name,
		struct fts_ts_platform_data *pdata)
{
	int ret = 0;
	u32 coords[FTS_COORDS_ARR_SIZE] = { 0 };
	struct property *prop;
	struct device_node *np = dev->of_node;
	int coords_size;
	u32 px_zone[3] = { 0 };

	prop = of_find_property(np, name, NULL);
	if (!prop)
		return -EINVAL;
	if (!prop->value)
		return -ENODATA;

	coords_size = prop->length / sizeof(u32);
	if (coords_size != FTS_COORDS_ARR_SIZE) {
		FTS_ERROR("invalid:%s, size:%d", name, coords_size);
		return -EINVAL;
	}

	ret = of_property_read_u32_array(np, name, coords, coords_size);
	if (ret < 0) {
		FTS_ERROR("Unable to read %s, please check dts", name);
		pdata->x_min = FTS_X_MIN_DISPLAY_DEFAULT;
		pdata->y_min = FTS_Y_MIN_DISPLAY_DEFAULT;
		pdata->x_max = FTS_X_MAX_DISPLAY_DEFAULT - 1;
		pdata->y_max = FTS_Y_MAX_DISPLAY_DEFAULT - 1;
		return -ENODATA;
	} else {
		pdata->x_min = coords[0];
		pdata->y_min = coords[1];
		pdata->x_max = coords[2] - 1;
		pdata->y_max = coords[3] - 1;
	}

	FTS_INFO("display x(%d %d) y(%d %d)", pdata->x_min, pdata->x_max,
			pdata->y_min, pdata->y_max);

	if (of_property_read_u32_array(np, "focaltech,area-size", px_zone, 3)) {
		pdata->area_indicator = 48;
		pdata->area_navigation = 96;
		pdata->area_edge = 60;
	} else {
		pdata->area_indicator = px_zone[0];
		pdata->area_navigation = px_zone[1];
		pdata->area_edge = px_zone[2];
	}
	FTS_INFO("zone's size - indicator:%d, navigation:%d, edge:%d",
		pdata->area_indicator, pdata->area_navigation, pdata->area_edge);

	return 0;
}

static int fts_parse_dt(struct device *dev, struct fts_ts_platform_data *pdata)
{
	int ret = 0;
	struct device_node *np = dev->of_node;
	u32 temp_val = 0;
	int lcdtype = 0;

	FTS_FUNC_ENTER();

#ifdef CONFIG_DISPLAY_SAMSUNG
	lcdtype = get_lcd_attached("GET");
	if (lcdtype == 0xFFFFFF) {
		FTS_ERROR("lcd is not attached");
		//return -ENODEV;
	}
#endif
	FTS_INFO("lcdtype=%X", lcdtype);

	ret = fts_get_dt_coords(dev, "focaltech,display-coords", pdata);
	if (ret < 0)
		FTS_ERROR("Unable to get display-coords");

	/* key */
	pdata->have_key = of_property_read_bool(np, "focaltech,have-key");
	if (pdata->have_key) {
		ret = of_property_read_u32(np, "focaltech,key-number", &pdata->key_number);
		if (ret < 0)
			FTS_ERROR("Key number undefined!");

		ret = of_property_read_u32_array(np, "focaltech,keys",
				pdata->keys, pdata->key_number);
		if (ret < 0)
			FTS_ERROR("Keys undefined!");
		else if (pdata->key_number > FTS_MAX_KEYS)
			pdata->key_number = FTS_MAX_KEYS;

		ret = of_property_read_u32_array(np, "focaltech,key-x-coords",
				pdata->key_x_coords,
				pdata->key_number);
		if (ret < 0)
			FTS_ERROR("Key Y Coords undefined!");

		ret = of_property_read_u32_array(np, "focaltech,key-y-coords",
				pdata->key_y_coords,
				pdata->key_number);
		if (ret < 0)
			FTS_ERROR("Key X Coords undefined!");

		FTS_INFO("VK Number:%d, key:(%d,%d,%d), "
				"coords:(%d,%d),(%d,%d),(%d,%d)",
				pdata->key_number,
				pdata->keys[0], pdata->keys[1], pdata->keys[2],
				pdata->key_x_coords[0], pdata->key_y_coords[0],
				pdata->key_x_coords[1], pdata->key_y_coords[1],
				pdata->key_x_coords[2], pdata->key_y_coords[2]);
	}

	/* reset, irq gpio info */
	pdata->reset_gpio = of_get_named_gpio_flags(np, "focaltech,reset-gpio",
			0, &pdata->reset_gpio_flags);
	if (pdata->reset_gpio < 0)
		FTS_ERROR("Unable to get reset_gpio");

	pdata->irq_gpio = of_get_named_gpio_flags(np, "focaltech,irq-gpio",
			0, &pdata->irq_gpio_flags);
	if (pdata->irq_gpio < 0)
		FTS_ERROR("Unable to get irq_gpio");

	pdata->gpio_vendor_check = of_get_named_gpio(np, "focaltech,vendor_check-gpio", 0);
	if (gpio_is_valid(pdata->gpio_vendor_check)) {
		ret = of_property_read_u32(np, "focaltech,vendor_check_enable_value", &temp_val);
		if (ret < 0) {
			FTS_ERROR("Unable to get vendor_check_enable_value, %d", ret);
		} else {
			int vendor_check_value = gpio_get_value(pdata->gpio_vendor_check);

			if (vendor_check_value != temp_val) {
				FTS_ERROR("gpio_vendor_check is %d, focaltech ic is not connected",
					vendor_check_value);
				return -ENODEV;
			}
		}
	}

	ret = of_property_read_u32(np, "focaltech,max-touch-number", &temp_val);
	if (ret < 0) {
		FTS_ERROR("Unable to get max-touch-number, please check dts");
		pdata->max_touch_number = FTS_MAX_POINTS_SUPPORT;
	} else {
		if (temp_val < 2)
			pdata->max_touch_number = 2; /* max_touch_number must >= 2 */
		else if (temp_val > FTS_MAX_POINTS_SUPPORT)
			pdata->max_touch_number = FTS_MAX_POINTS_SUPPORT;
		else
			pdata->max_touch_number = temp_val;
	}

	of_property_read_string(np, "sec,firmware_name", &pdata->firmware_name);

	FTS_INFO("max touch number:%d, irq gpio:%d, reset gpio:%d",
			pdata->max_touch_number, pdata->irq_gpio, pdata->reset_gpio);

	pdata->enable_settings_aot = of_property_read_bool(np, "enable_settings_aot");
	pdata->enable_sysinput_enabled = of_property_read_bool(np, "enable_sysinput_enabled");
	pdata->support_dex = of_property_read_bool(np, "support_dex");
	pdata->scan_off_when_cover_closed = of_property_read_bool(np, "scan_off_when_cover_closed");

	FTS_INFO("scan_off_when_cover_closed:%d", pdata->scan_off_when_cover_closed);

	if (of_property_read_string(np, "focaltech,panel_buck_en", &pdata->panel_buck_en))
		FTS_ERROR("Failed to get panel_buck_en name property");
	if (of_property_read_string(np, "focaltech,panel_buck_en2", &pdata->panel_buck_en2))
		FTS_ERROR("Failed to get panel_buck_en2 name property");
	if (of_property_read_string(np, "focaltech,panel_ldo_en", &pdata->panel_ldo_en))
		FTS_ERROR("Failed to get panel_ldo_en name property");
	if (of_property_read_string(np, "focaltech,panel_reset", &pdata->panel_reset))
		FTS_ERROR("Failed to get panel_reset name property");

	FTS_FUNC_EXIT();
	return 0;
}

static void fts_resume_work(struct work_struct *work)
{
	struct fts_ts_data *ts_data = container_of(work, struct fts_ts_data,
			resume_work);

	fts_ts_resume(ts_data->dev);
}

#if defined(CONFIG_FB)
static int fb_notifier_callback(struct notifier_block *self,
		unsigned long event, void *data)
{
	struct fb_event *evdata = data;
	int *blank = NULL;
	struct fts_ts_data *ts_data = container_of(self, struct fts_ts_data,
			fb_notif);

	if (!evdata) {
		FTS_ERROR("evdata is null");
		return 0;
	}

	if (!(event == FB_EARLY_EVENT_BLANK || event == FB_EVENT_BLANK)) {
		FTS_INFO("event(%lu) do not need process", event);
		return 0;
	}

	blank = evdata->data;
	FTS_INFO("FB event:%lu,blank:%d", event, *blank);
	switch (*blank) {
	case FB_BLANK_UNBLANK:
		if (FB_EARLY_EVENT_BLANK == event) {
			FTS_INFO("resume: event = %lu, not care", event);
		} else if (FB_EVENT_BLANK == event) {
			queue_work(ts_data->ts_workqueue, &ts_data->resume_work);
		}
		break;
	case FB_BLANK_POWERDOWN:
		if (FB_EARLY_EVENT_BLANK == event) {
			cancel_work_sync(&ts_data->resume_work);
			fts_ts_suspend(ts_data->dev);
		} else if (FB_EVENT_BLANK == event) {
			FTS_INFO("suspend: event = %lu, not care", event);
		}
		break;
	default:
		FTS_INFO("FB BLANK(%d) do not need process", *blank);
		break;
	}

	return 0;
}
#elif defined(CONFIG_DRM)
#if defined(CONFIG_DRM_PANEL)
static struct drm_panel *active_panel;

static int drm_check_dt(struct device_node *np)
{
	int i = 0;
	int count = 0;
	struct device_node *node = NULL;
	struct drm_panel *panel = NULL;

	count = of_count_phandle_with_args(np, "panel", NULL);
	if (count <= 0) {
		FTS_ERROR("find drm_panel count(%d) fail", count);
		return -ENODEV;
	}

	for (i = 0; i < count; i++) {
		node = of_parse_phandle(np, "panel", i);
		panel = of_drm_find_panel(node);
		of_node_put(node);
		if (!IS_ERR(panel)) {
			FTS_INFO("find drm_panel successfully");
			active_panel = panel;
			return 0;
		}
	}

	FTS_ERROR("no find drm_panel");
	return -ENODEV;
}

static int drm_notifier_callback(struct notifier_block *self,
		unsigned long event, void *data)
{
	struct msm_drm_notifier *evdata = data;
	int *blank = NULL;
	struct fts_ts_data *ts_data = container_of(self, struct fts_ts_data,
			fb_notif);

	if (!evdata) {
		FTS_ERROR("evdata is null");
		return 0;
	}

	if (!((event == DRM_PANEL_EARLY_EVENT_BLANK)
				|| (event == DRM_PANEL_EVENT_BLANK))) {
		FTS_INFO("event(%lu) do not need process", event);
		return 0;
	}

	blank = evdata->data;
	FTS_INFO("DRM event:%lu,blank:%d", event, *blank);
	switch (*blank) {
	case DRM_PANEL_BLANK_UNBLANK:
		if (DRM_PANEL_EARLY_EVENT_BLANK == event) {
			FTS_INFO("resume: event = %lu, not care", event);
		} else if (DRM_PANEL_EVENT_BLANK == event) {
			queue_work(ts_data->ts_workqueue, &ts_data->resume_work);
		}
		break;
	case DRM_PANEL_BLANK_POWERDOWN:
		if (DRM_PANEL_EARLY_EVENT_BLANK == event) {
			cancel_work_sync(&ts_data->resume_work);
			fts_ts_suspend(ts_data->dev);
		} else if (DRM_PANEL_EVENT_BLANK == event) {
			FTS_INFO("suspend: event = %lu, not care", event);
		}
		break;
	default:
		FTS_INFO("DRM BLANK(%d) do not need process", *blank);
		break;
	}

	return 0;
}
#else
static int drm_notifier_callback(struct notifier_block *self,
		unsigned long event, void *data)
{
	struct msm_drm_notifier *evdata = data;
	int *blank = NULL;
	struct fts_ts_data *ts_data = container_of(self, struct fts_ts_data,
			fb_notif);

	if (!evdata) {
		FTS_ERROR("evdata is null");
		return 0;
	}

	if (!((event == MSM_DRM_EARLY_EVENT_BLANK)
				|| (event == MSM_DRM_EVENT_BLANK))) {
		FTS_INFO("event(%lu) do not need process", event);
		return 0;
	}

	blank = evdata->data;
	FTS_INFO("DRM event:%lu,blank:%d", event, *blank);
	switch (*blank) {
	case MSM_DRM_BLANK_UNBLANK:
		if (MSM_DRM_EARLY_EVENT_BLANK == event) {
			FTS_INFO("resume: event = %lu, not care", event);
		} else if (MSM_DRM_EVENT_BLANK == event) {
			queue_work(ts_data->ts_workqueue, &ts_data->resume_work);
		}
		break;
	case MSM_DRM_BLANK_POWERDOWN:
		if (MSM_DRM_EARLY_EVENT_BLANK == event) {
			cancel_work_sync(&ts_data->resume_work);
			fts_ts_suspend(ts_data->dev);
		} else if (MSM_DRM_EVENT_BLANK == event) {
			FTS_INFO("suspend: event = %lu, not care", event);
		}
		break;
	default:
		FTS_INFO("DRM BLANK(%d) do not need process", *blank);
		break;
	}

	return 0;
}
#endif
#elif defined(CONFIG_HAS_EARLYSUSPEND)
static void fts_ts_early_suspend(struct early_suspend *handler)
{
	struct fts_ts_data *ts_data = container_of(handler, struct fts_ts_data,
			early_suspend);

	cancel_work_sync(&ts_data->resume_work);
	fts_ts_suspend(ts_data->dev);
}

static void fts_ts_late_resume(struct early_suspend *handler)
{
	struct fts_ts_data *ts_data = container_of(handler, struct fts_ts_data,
			early_suspend);

	queue_work(ts_data->ts_workqueue, &ts_data->resume_work);
}
#endif

static void fts_print_info(struct fts_ts_data *ts)
{
	ts->print_info_cnt_open++;

	if (ts->print_info_cnt_open > 0xfff0)
		ts->print_info_cnt_open = 0;

	if (ts->touchs == 0)
		ts->print_info_cnt_release++;

	FTS_INFO("tc:%d v:%02X%02X lp:%x // #%d %d",
		FTS_TOUCH_COUNT(&ts->touchs),
		ts->ic_fw_ver.module_id, ts->ic_fw_ver.fw_ver,
		ts->gesture_mode,
		ts->print_info_cnt_open, ts->print_info_cnt_release);
}

static void fts_print_info_work(struct work_struct *work)
{
	struct fts_ts_data *ts = container_of(work, struct fts_ts_data,
			print_info_work.work);

	fts_print_info(ts);

	schedule_delayed_work(&ts->print_info_work, msecs_to_jiffies(TOUCH_PRINT_INFO_DWORK_TIME));
}

static int fts_ts_probe_entry(struct fts_ts_data *ts_data)
{
	int ret = 0;
	int pdata_size = sizeof(struct fts_ts_platform_data);

	FTS_FUNC_ENTER();
	FTS_INFO("%s", FTS_DRIVER_VERSION);
	ts_data->pdata = devm_kzalloc(ts_data->dev, pdata_size, GFP_KERNEL);
	if (!ts_data->pdata) {
		FTS_ERROR("allocate memory for platform_data fail");
		return -ENOMEM;
	}

	if (ts_data->dev->of_node) {
		ret = fts_parse_dt(ts_data->dev, ts_data->pdata);
		if (ret) {
			FTS_ERROR("device-tree parse fail");
			goto err_parse_dt;
		}

#if !defined(CONFIG_FB) && defined(CONFIG_DRM)
#if defined(CONFIG_DRM_PANEL)
		ret = drm_check_dt(ts_data->dev->of_node);
		if (ret) {
			FTS_ERROR("parse drm-panel fail");
		}
#endif
#endif
	} else {
		if (ts_data->dev->platform_data) {
			memcpy(ts_data->pdata, ts_data->dev->platform_data, pdata_size);
		} else {
			FTS_ERROR("platform_data is null");
			goto err_parse_dt;
		}
	}

	ts_data->ts_workqueue = create_singlethread_workqueue("fts_wq");
	if (!ts_data->ts_workqueue) {
		FTS_ERROR("create fts workqueue fail");
	}

	mutex_init(&ts_data->report_mutex);
	mutex_init(&ts_data->bus_lock);
	mutex_init(&ts_data->device_lock);
	mutex_init(&ts_data->irq_lock);

	/* Init communication interface */
	ret = fts_bus_init(ts_data);
	if (ret) {
		FTS_ERROR("bus initialize fail");
		goto err_bus_init;
	}

	ret = fts_input_init(ts_data);
	if (ret) {
		FTS_ERROR("input initialize fail");
		goto err_input_init;
	}

	ret = fts_report_buffer_init(ts_data);
	if (ret) {
		FTS_ERROR("report buffer init fail");
		goto err_report_buffer;
	}

	ret = fts_gpio_configure(ts_data);
	if (ret) {
		FTS_ERROR("configure the gpios fail");
		goto err_gpio_config;
	}

#if FTS_PINCTRL_EN
	fts_pinctrl_init(ts_data);
	fts_pinctrl_select_normal(ts_data);
#endif

#if FTS_POWER_SOURCE_CUST_EN
	ret = fts_power_source_init(ts_data);
	if (ret) {
		FTS_ERROR("fail to get power(regulator)");
		goto err_power_init;
	}
#endif

#if (!FTS_CHIP_IDC)
	fts_reset_proc(200);
#endif

	ret = fts_get_ic_information(ts_data);
	if (ret) {
		FTS_ERROR("not focal IC, unregister driver");
		goto err_irq_req;
	}

#if !IS_ENABLED(CONFIG_SAMSUNG_PRODUCT_SHIP)
	ret = fts_create_apk_debug_channel(ts_data);
	if (ret) {
		FTS_ERROR("create apk debug node fail");
	}

	ret = fts_create_sysfs(ts_data);
	if (ret) {
		FTS_ERROR("create sysfs node fail");
	}
#endif

#if FTS_POINT_REPORT_CHECK_EN
	ret = fts_point_report_check_init(ts_data);
	if (ret) {
		FTS_ERROR("init point report check fail");
	}
#endif

	ret = fts_ex_mode_init(ts_data);
	if (ret) {
		FTS_ERROR("init glove/cover/charger fail");
	}

	ret = fts_gesture_init(ts_data);
	if (ret) {
		FTS_ERROR("init gesture fail");
	}

#if FTS_TEST_EN
	ret = fts_test_init(ts_data);
	if (ret) {
		FTS_ERROR("init production test fail");
	}
#endif

#if FTS_ESDCHECK_EN
	ret = fts_esdcheck_init(ts_data);
	if (ret) {
		FTS_ERROR("init esd check fail");
	}
#endif

	ret = fts_irq_registration(ts_data);
	if (ret) {
		FTS_ERROR("request irq failed");
		goto err_irq_req;
	}

	ret = fts_fwupg_init(ts_data);
	if (ret) {
		FTS_ERROR("init fw upgrade fail");
		goto err_fwupg_init;
	}

	ret = fts_sec_cmd_init(ts_data);
	if (ret) {
		FTS_ERROR("init sec_cmd fail");
		goto err_fwupg_init;
	}

	if (ts_data->ts_workqueue) {
		INIT_WORK(&ts_data->resume_work, fts_resume_work);
	}

#if defined(CONFIG_PM) && FTS_PATCH_COMERR_PM
	init_completion(&ts_data->pm_completion);
	complete_all(&ts_data->pm_completion);
	ts_data->pm_suspend = false;
	fts_wakeup_source_init(ts_data);
#endif

	if (!ts_data->pdata->enable_sysinput_enabled) {
#if defined(CONFIG_FB)
		ts_data->fb_notif.notifier_call = fb_notifier_callback;
		ret = fb_register_client(&ts_data->fb_notif);
		if (ret) {
			FTS_ERROR("[FB]Unable to register fb_notifier: %d", ret);
		}
#elif defined(CONFIG_DRM)
		ts_data->fb_notif.notifier_call = drm_notifier_callback;
#if defined(CONFIG_DRM_PANEL)
		if (active_panel) {
			ret = drm_panel_notifier_register(active_panel, &ts_data->fb_notif);
			if (ret)
				FTS_ERROR("[DRM]drm_panel_notifier_register fail: %d", ret);
		}
#else
		ret = msm_drm_register_client(&ts_data->fb_notif);
		if (ret) {
			FTS_ERROR("[DRM]Unable to register fb_notifier: %d", ret);
		}
#endif
#elif defined(CONFIG_HAS_EARLYSUSPEND)
		ts_data->early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + FTS_SUSPEND_LEVEL;
		ts_data->early_suspend.suspend = fts_ts_early_suspend;
		ts_data->early_suspend.resume = fts_ts_late_resume;
		register_early_suspend(&ts_data->early_suspend);
#endif
	}

	INIT_DELAYED_WORK(&ts_data->print_info_work, fts_print_info_work);
	schedule_delayed_work(&ts_data->print_info_work, msecs_to_jiffies(15000));

	FTS_FUNC_EXIT();
	return 0;

	//fts_wakeup_source_unregister(ts_data);
err_fwupg_init:
	fts_irq_disable();
	free_irq(ts_data->irq, ts_data);
err_irq_req:
#if FTS_POWER_SOURCE_CUST_EN
err_power_init:
	fts_power_source_exit(ts_data);
#endif
#if FTS_PINCTRL_EN
	fts_pinctrl_select_release(ts_data);
#endif
	if (gpio_is_valid(ts_data->pdata->reset_gpio))
		gpio_free(ts_data->pdata->reset_gpio);
	if (gpio_is_valid(ts_data->pdata->irq_gpio))
		gpio_free(ts_data->pdata->irq_gpio);
err_gpio_config:
err_report_buffer:
err_input_init:
	if (ts_data->ts_workqueue)
		destroy_workqueue(ts_data->ts_workqueue);
err_bus_init:
	mutex_destroy(&ts_data->report_mutex);
	mutex_destroy(&ts_data->bus_lock);
	mutex_destroy(&ts_data->device_lock);
	mutex_destroy(&ts_data->irq_lock);
err_parse_dt:
	FTS_FUNC_EXIT();
	return ret;
}

static int fts_ts_remove_entry(struct fts_ts_data *ts_data)
{
	FTS_FUNC_ENTER();

	disable_irq(ts_data->irq);
	cancel_delayed_work_sync(&ts_data->print_info_work);

#if FTS_POINT_REPORT_CHECK_EN
	fts_point_report_check_exit(ts_data);
#endif

	fts_sec_cmd_exit(ts_data);

#if !IS_ENABLED(CONFIG_SAMSUNG_PRODUCT_SHIP)
	fts_release_apk_debug_channel(ts_data);
	fts_remove_sysfs(ts_data);
#endif
	fts_ex_mode_exit(ts_data);

	fts_fwupg_exit(ts_data);

#if FTS_TEST_EN
	fts_test_exit(ts_data);
#endif

#if FTS_ESDCHECK_EN
	fts_esdcheck_exit(ts_data);
#endif

	fts_gesture_exit(ts_data);
	fts_bus_exit(ts_data);

	fts_wakeup_source_unregister(ts_data);

	free_irq(ts_data->irq, ts_data);

	if (ts_data->ts_workqueue)
		destroy_workqueue(ts_data->ts_workqueue);

	if (!ts_data->pdata->enable_sysinput_enabled) {
#if defined(CONFIG_FB)
		if (fb_unregister_client(&ts_data->fb_notif))
			FTS_ERROR("[FB]Error occurred while unregistering fb_notifier.");
#elif defined(CONFIG_DRM)
#if defined(CONFIG_DRM_PANEL)
		if (active_panel)
			drm_panel_notifier_unregister(active_panel, &ts_data->fb_notif);
#else
		if (msm_drm_unregister_client(&ts_data->fb_notif))
			FTS_ERROR("[DRM]Error occurred while unregistering fb_notifier.\n");
#endif
#elif defined(CONFIG_HAS_EARLYSUSPEND)
		unregister_early_suspend(&ts_data->early_suspend);
#endif
	}

	if (gpio_is_valid(ts_data->pdata->reset_gpio))
		gpio_free(ts_data->pdata->reset_gpio);

	if (gpio_is_valid(ts_data->pdata->irq_gpio))
		gpio_free(ts_data->pdata->irq_gpio);

#if FTS_POWER_SOURCE_CUST_EN
	fts_power_source_exit(ts_data);
#endif
#if FTS_PINCTRL_EN
	fts_pinctrl_select_release(ts_data);
#endif
	mutex_destroy(&ts_data->report_mutex);
	mutex_destroy(&ts_data->bus_lock);
	mutex_destroy(&ts_data->device_lock);
	mutex_destroy(&ts_data->irq_lock);

	FTS_FUNC_EXIT();
	fts_data = NULL;

	return 0;
}

int fts_ts_suspend(struct device *dev)
{
	int ret = 0;
	struct fts_ts_data *ts_data = fts_data;

	mutex_lock(&ts_data->device_lock);
	FTS_INFO("Enter. gesture_mode=0x%x, prox_power_off=%d", ts_data->gesture_mode, ts_data->prox_power_off);

	if (ts_data->suspended) {
		FTS_INFO("Already in suspend state");
		mutex_unlock(&ts_data->device_lock);
		return 0;
	}

	if (ts_data->fw_loading) {
		FTS_INFO("fw upgrade in process, can't suspend");
		mutex_unlock(&ts_data->device_lock);
		return 0;
	}

#if FTS_ESDCHECK_EN
	fts_esdcheck_suspend();
#endif

	if (ts_data->prox_power_off == 1 && ts_data->aot_enable) {
		ts_data->gesture_mode = false;
		FTS_INFO("disable aot when prox_power_off");
	}

	if (ts_data->gesture_mode) {
		fts_ctrl_lcd_reset_regulator(ts_data, true);
		fts_ctrl_lcd_regulators(ts_data, true);
		fts_gesture_suspend(ts_data);
	} else {
		FTS_INFO("make TP enter into sleep mode");
		ret = fts_write_reg(FTS_REG_POWER_MODE, FTS_REG_POWER_MODE_SLEEP);
		if (ret < 0)
			FTS_ERROR("set TP to sleep mode fail, ret=%d", ret);

		fts_irq_disable();
		gpio_direction_output(ts_data->pdata->reset_gpio, 0);

		if (!ts_data->ic_info.is_incell) {
#if FTS_POWER_SOURCE_CUST_EN
			ret = fts_power_source_suspend(ts_data);
			if (ret < 0) {
				FTS_ERROR("power enter suspend fail");
			}
#endif
		} else {
			ts_data->power_disabled = true;
		}
#if FTS_PINCTRL_EN
		fts_pinctrl_select_suspend(ts_data);
#endif
	}

	fts_release_all_finger();
	ts_data->suspended = true;

	cancel_delayed_work(&ts_data->print_info_work);
	fts_print_info(ts_data);

	FTS_FUNC_EXIT();
	mutex_unlock(&ts_data->device_lock);
	return 0;
}

static int fts_ts_resume(struct device *dev)
{
	struct fts_ts_data *ts_data = fts_data;
	ktime_t start_time = ktime_get();
	s64 time_diff_ms;

	mutex_lock(&ts_data->device_lock);
	FTS_FUNC_ENTER();
	if (!ts_data->suspended) {
		FTS_DEBUG("Already in awake state");
		mutex_unlock(&ts_data->device_lock);
		return 0;
	}

	fts_release_all_finger();

	if (!ts_data->gesture_mode) {
#if FTS_PINCTRL_EN
		fts_pinctrl_select_normal(ts_data);
#endif
		if (!ts_data->ic_info.is_incell) {
#if FTS_POWER_SOURCE_CUST_EN
			fts_power_source_resume(ts_data);
#endif
		} else {
			ts_data->power_disabled = false;
		}
		fts_fwupg_func(TSP_BUILT_IN, false);
		//fts_reset_proc(200);
	} else {
		fts_ctrl_lcd_regulators(ts_data, false);
	}

	fts_wait_tp_to_valid();
	fts_ex_mode_recovery(ts_data);

#if FTS_ESDCHECK_EN
	fts_esdcheck_resume();
#endif

	if (ts_data->gesture_mode) {
		fts_gesture_resume(ts_data);
	}

	if (ts_data->aot_enable)
		ts_data->gesture_mode = true;
	ts_data->prox_power_off = 0;

	ts_data->suspended = false;

	cancel_delayed_work(&ts_data->print_info_work);
	ts_data->print_info_cnt_open = 0;
	ts_data->print_info_cnt_release = 0;
	schedule_work(&ts_data->print_info_work.work);

	time_diff_ms = ktime_ms_delta(ktime_get(), start_time);
	FTS_INFO("Exit(%d). spent %lldms", __LINE__, time_diff_ms);
	mutex_unlock(&ts_data->device_lock);
	return 0;
}

#if defined(CONFIG_PM) && FTS_PATCH_COMERR_PM
static int fts_pm_suspend(struct device *dev)
{
	struct fts_ts_data *ts_data = dev_get_drvdata(dev);

	//FTS_INFO("system enters into pm_suspend");
	ts_data->pm_suspend = true;
	reinit_completion(&ts_data->pm_completion);
	return 0;
}

static int fts_pm_resume(struct device *dev)
{
	struct fts_ts_data *ts_data = dev_get_drvdata(dev);

	//FTS_INFO("system resumes from pm_suspend");
	ts_data->pm_suspend = false;
	complete_all(&ts_data->pm_completion);
	return 0;
}

static const struct dev_pm_ops fts_dev_pm_ops = {
	.suspend = fts_pm_suspend,
	.resume = fts_pm_resume,
};
#endif

/*****************************************************************************
 * TP Driver
 *****************************************************************************/
static int fts_ts_probe(struct spi_device *spi)
{
	int ret = 0;
	struct fts_ts_data *ts_data = NULL;

	FTS_INFO("Touch Screen(SPI BUS) driver probe...");
	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret) {
		FTS_ERROR("spi setup fail");
		return ret;
	}

	/* malloc memory for global struct variable */
	ts_data = devm_kzalloc(&spi->dev, sizeof(struct fts_ts_data), GFP_KERNEL);
	if (!ts_data) {
		FTS_ERROR("allocate memory for fts_data fail");
		return -ENOMEM;
	}

	fts_data = ts_data;
	ts_data->spi = spi;
	ts_data->dev = &spi->dev;
	ts_data->log_level = 1;

	ts_data->bus_type = BUS_TYPE_SPI_V2;
	spi_set_drvdata(spi, ts_data);

	ret = fts_ts_probe_entry(ts_data);
	if (ret) {
		FTS_ERROR("Touch Screen(SPI BUS) driver probe fail");
		fts_data = NULL;
		return ret;
	}

	FTS_INFO("Touch Screen(SPI BUS) driver probe successfully");
	return 0;
}

static int fts_ts_remove(struct spi_device *spi)
{
	return fts_ts_remove_entry(spi_get_drvdata(spi));
}

static void fts_ts_shutdown(struct spi_device *spi)
{
	fts_ts_remove_entry(spi_get_drvdata(spi));
}

static const struct spi_device_id fts_ts_id[] = {
	{FTS_DRIVER_NAME, 0},
	{},
};
static const struct of_device_id fts_dt_match[] = {
	{.compatible = "focaltech,fts", },
	{},
};
MODULE_DEVICE_TABLE(of, fts_dt_match);

static struct spi_driver fts_ts_driver = {
	.probe = fts_ts_probe,
	.remove = fts_ts_remove,
	.shutdown = fts_ts_shutdown,
	.driver = {
		.name = FTS_DRIVER_NAME,
		.owner = THIS_MODULE,
#if defined(CONFIG_PM) && FTS_PATCH_COMERR_PM
		.pm = &fts_dev_pm_ops,
#endif
		.of_match_table = of_match_ptr(fts_dt_match),
	},
	.id_table = fts_ts_id,
};

static int __init fts_ts_init(void)
{
	int ret = 0;

	FTS_FUNC_ENTER();
	ret = spi_register_driver(&fts_ts_driver);
	if (ret != 0)
		FTS_ERROR("Focaltech touch screen driver init failed!");

	FTS_FUNC_EXIT();
	input_log_fix();
	return ret;
}

static void __exit fts_ts_exit(void)
{
	spi_unregister_driver(&fts_ts_driver);
}

module_init(fts_ts_init);
module_exit(fts_ts_exit);

MODULE_AUTHOR("FocalTech Driver Team");
MODULE_DESCRIPTION("FocalTech Touchscreen Driver");
MODULE_LICENSE("GPL v2");

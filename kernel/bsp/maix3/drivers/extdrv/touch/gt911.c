/* Copyright (c) 2025, Canaan Bright Sight Co., Ltd
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "drv_touch.h"
#include "rtthread.h"
#include <stdint.h>

#define DBG_TAG "gt911"
#define DBG_LVL DBG_WARNING
#define DBG_COLOR
#include <rtdbg.h>

struct __attribute__((packed)) GTInfo {
    char     productId[4]; // 0x8140 - 0x8143
    uint16_t fwId; // 0x8144 - 0x8145
    uint16_t xResolution; // 0x8146 - 0x8147
    uint16_t yResolution; // 0x8148 - 0x8149
    uint8_t  vendorId; // 0x814A
};

struct __attribute__((packed)) GTPoint {
    // 0x814F-0x8156, ... 0x8176 (5 points)
    uint8_t trackId;
    uint8_t xl;
    uint8_t xh;
    uint8_t yl;
    uint8_t yh;
    uint8_t szl;
    uint8_t szh;
    uint8_t reserved;
};

struct gt911_reg {
    uint8_t        status;
    struct GTPoint pos[TOUCH_MAX_POINT_NUMBER];
};

_Static_assert(sizeof(struct gt911_reg) < TOUCH_READ_REG_MAX_SIZE, "gt911 reg data size > TOUCH_READ_REG_MAX_SIZE");

// APIs ///////////////////////////////////////////////////////////////////////
static int read_register(struct drv_touch_dev* dev, struct touch_register* reg)
{
    int      ret       = 0;
    uint8_t  point_num = 0;
    uint8_t* _reg      = (uint8_t*)&reg->reg[0];

    uint8_t clear[] = { 0x81, 0x4e, 0x00 };

    reg->time = rt_tick_get();

    if (0x00 != touch_dev_read_reg(dev, 0x814E, &_reg[0], 1)) {
        return -1;
    }
    ret += touch_dev_write_reg(dev, clear, sizeof(clear));

    point_num = reg->reg[0] & 0x0F;
    if (point_num) {
        ret += touch_dev_read_reg(dev, 0x814F, &_reg[1], point_num * sizeof(struct GTPoint));
    }

    return ret;
}

static int parse_register(struct drv_touch_dev* dev, struct touch_register* reg, struct touch_point* result)
{
    int      finger_num;
    uint8_t  xh, xl, yh, yl, szh, szl;
    uint16_t point_x, point_y, point_size;
    int      result_index = 0, point_index = 0;

    rt_tick_t time = reg->time;

    struct rt_touch_data* point     = NULL;
    struct gt911_reg*     gt911_reg = (struct gt911_reg*)reg->reg;

    finger_num = gt911_reg->status & 0x0F;
    if (finger_num > TOUCH_MAX_POINT_NUMBER) {
        LOG_W("gt911 touch point %d > max %d", finger_num, TOUCH_MAX_POINT_NUMBER);

        finger_num = TOUCH_MAX_POINT_NUMBER;
    }
    result->point_num = finger_num;

    if (finger_num) {
        for (result_index = 0, point_index = 0; result_index < finger_num; result_index++, point_index++) {
            point = &result->point[point_index];

            xh      = gt911_reg->pos[result_index].xh;
            xl      = gt911_reg->pos[result_index].xl;
            point_x = (xh << 8) | xl;
            if (point_x > dev->touch.range_x) {
                point_index--;
                continue;
            }

            yh      = gt911_reg->pos[result_index].yh;
            yl      = gt911_reg->pos[result_index].yl;
            point_y = (yh << 8) | yl;
            if (point_y > dev->touch.range_y) {
                point_index--;
                continue;
            }

            szh        = gt911_reg->pos[result_index].szh;
            szl        = gt911_reg->pos[result_index].szl;
            point_size = (szh << 8) | szl;

            point->event        = RT_TOUCH_EVENT_NONE;
            point->track_id     = gt911_reg->pos[result_index].trackId;
            point->width        = point_size;
            point->x_coordinate = point_x;
            point->y_coordinate = point_y;
            point->timestamp    = time;
        }
    }

    touch_dev_update_event(finger_num, point);

    return 0;
}

static int reset(struct drv_touch_dev* dev)
{
    int     ret          = 0;
    uint8_t gt911_mode[] = { 0x80, 0x40, 0x00 };

    static int gt911_rest_flag = 0;

    if ((0x00 == gt911_rest_flag) && (0 <= dev->pin.rst) && (63 >= dev->pin.rst)) {
        gt911_rest_flag = 1;

        if ((0 <= dev->pin.intr) && (63 >= dev->pin.intr)) {
            kd_pin_mode(dev->pin.intr, GPIO_DM_OUTPUT);
            kd_pin_write(dev->pin.intr, 0);

            dev->i2c.addr = 0x5d;
        }

        kd_pin_write(dev->pin.rst, 1 - dev->pin.rst_valid);
        rt_thread_mdelay(20);
        kd_pin_write(dev->pin.rst, dev->pin.rst_valid);
        rt_thread_mdelay(10);
        kd_pin_write(dev->pin.rst, 1 - dev->pin.rst_valid);
        rt_thread_mdelay(100);

        if ((0 <= dev->pin.intr) && (63 >= dev->pin.intr)) {
            kd_pin_mode(dev->pin.intr, GPIO_DM_INPUT);
        }
    } else {
        // reset
        gt911_mode[2] = 0x02;
        if (0x00 != touch_dev_write_reg(dev, gt911_mode, sizeof(gt911_mode))) {
            return -1;
        }
        rt_thread_mdelay(100);

        gt911_mode[2] = 0x00;
    }

    // set to read coordinate
    return touch_dev_write_reg(dev, gt911_mode, sizeof(gt911_mode));
}

static int get_default_rotate(struct drv_touch_dev* dev)
{
#if defined(CONFIG_BOARD_K230_CANMV_LCKFB)
    return RT_TOUCH_ROTATE_DEGREE_0;
#elif defined(CONFIG_BOARD_K230_CANMV_01STUDIO)
    return RT_TOUCH_ROTATE_DEGREE_270;
#else
    return RT_TOUCH_ROTATE_DEGREE_0;
#endif
}

int drv_touch_probe_gt911(struct drv_touch_dev* dev)
{
    struct GTInfo info;

    dev->i2c.addr      = 0x14;
    dev->i2c.reg_width = 2;

    if (0x00 != reset(dev)) {
        return -1;
    }

    if (0x00 != touch_dev_read_reg(dev, 0x8140, (uint8_t*)&info, sizeof(info))) {
        return -2;
    }
    rt_strncpy(dev->dev.drv_name, "gt911", sizeof(dev->dev.drv_name));

    dev->dev.read_register      = read_register;
    dev->dev.parse_register     = parse_register;
    dev->dev.reset              = reset;
    dev->dev.get_default_rotate = get_default_rotate;

    dev->touch.point_num = 10;
    dev->touch.range_x = info.xResolution;
    dev->touch.range_y = info.yResolution;

    LOG_I("gt9xx-> product id: %s, firmware version: 0x%x, resolution %dx%d\n", info.productId, info.fwId,
               dev->touch.range_x, dev->touch.range_y);

    return 0;
}

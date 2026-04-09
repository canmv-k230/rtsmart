/* Copyright (c) 2026, Canaan Bright Sight Co., Ltd
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

#define DBG_TAG "cf1124"
#define DBG_LVL DBG_WARNING
#define DBG_COLOR
#include <rtdbg.h>

/* Sitronix CF1124 register definitions */
#define CF1124_I2C_ADDR         0x55

#define CF1124_REG_FW_VERSION   0x00
#define CF1124_REG_STATUS       0x01
#define CF1124_REG_DEV_CTRL     0x02
#define CF1124_REG_XY_RES_HIGH  0x04
#define CF1124_REG_X_RES_LOW    0x05
#define CF1124_REG_Y_RES_LOW    0x06
#define CF1124_REG_TOUCH_DATA   0x11
#define CF1124_REG_MAX_TOUCHES  0x3F
#define CF1124_REG_CHIP_ID      0xF4

#define CF1124_MAX_TOUCHES      10

/* Touch data: 1 byte key status + CF1124_MAX_TOUCHES * 4 bytes per point */
#define CF1124_TOUCH_DATA_LEN   (1 + CF1124_MAX_TOUCHES * 4)

struct cf1124_point {
    uint8_t status;  /* bit 7: valid, bits 6-4: X[10:8], bits 3-0: Y[10:8] */
    uint8_t xl;      /* X[7:0] */
    uint8_t yl;      /* Y[7:0] */
};

struct cf1124_reg {
    uint8_t key_status;
    struct cf1124_point points[CF1124_MAX_TOUCHES];
};

_Static_assert(sizeof(struct cf1124_reg) <= TOUCH_READ_REG_MAX_SIZE, "CF1124 reg data size > TOUCH_READ_REG_MAX_SIZE");

// APIs ///////////////////////////////////////////////////////////////////////
static int read_register(struct drv_touch_dev* dev, struct touch_register* reg)
{
    reg->time = rt_tick_get();

    return touch_dev_read_reg(dev, CF1124_REG_TOUCH_DATA, (uint8_t*)&reg->reg[0], CF1124_TOUCH_DATA_LEN);
}

static int parse_register(struct drv_touch_dev* dev, struct touch_register* reg, struct touch_point* result)
{
    int       finger_num = 0;
    uint16_t  point_x, point_y;
    rt_tick_t time = reg->time;

    uint8_t* buf = (uint8_t*)reg->reg;
    struct rt_touch_data* point = NULL;

    result->point_num = 0;

    for (int i = 0; i < dev->touch.point_num; i++) {
        /* Each point: 4 bytes starting at buf[1 + 4*i] */
        uint8_t st = buf[1 + 4 * i];

        if (!(st & 0x80))
            continue;

        /* X coordinate: bits [6:4] of status as high 3 bits, next byte as low 8 bits */
        point_x = (uint16_t)(st & 0x70) << 4 | buf[1 + 4 * i + 1];
        if (point_x > dev->touch.range_x)
            continue;

        /* Y coordinate: bits [3:0] of status as high 4 bits, next next byte as low 8 bits */
        point_y = (uint16_t)(st & 0x0F) << 8 | buf[1 + 4 * i + 2];
        if (point_y > dev->touch.range_y)
            continue;

        point = &result->point[finger_num];

        point->event        = RT_TOUCH_EVENT_NONE;
        point->track_id     = i;
        point->width        = 1;
        point->x_coordinate = point_x;
        point->y_coordinate = point_y;
        point->timestamp    = time;

        finger_num++;
    }

    result->point_num = finger_num;
    touch_dev_update_event(finger_num, result->point);

    return 0;
}

static int reset(struct drv_touch_dev* dev)
{
    if ((0 <= dev->pin.rst) && (63 >= dev->pin.rst)) {
        kd_pin_write(dev->pin.rst, 1 - dev->pin.rst_valid);
        rt_thread_mdelay(5);
        kd_pin_write(dev->pin.rst, dev->pin.rst_valid);
        rt_thread_mdelay(3);
        kd_pin_write(dev->pin.rst, 1 - dev->pin.rst_valid);
        rt_thread_mdelay(10);
    }

    return 0;
}

static int get_default_rotate(struct drv_touch_dev* dev) { return RT_TOUCH_ROTATE_SWAP_XY; }

int drv_touch_probe_cf1124(struct drv_touch_dev* dev)
{
    uint8_t  buf[3];
    uint8_t  max_touches;
    uint16_t x_res, y_res;

    dev->i2c.addr      = CF1124_I2C_ADDR;
    dev->i2c.reg_width = 1;

    rt_thread_mdelay(150); /* wait for chip boot after reset */

    /* Read chip ID register (3 bytes: chip_id, Num_X, Num_Y) */
    if (0x00 != touch_dev_read_reg(dev, CF1124_REG_CHIP_ID, buf, 3)) {
        return -1;
    }

    LOG_I("cf1124 chip_id=%d, Num_X=%d, Num_Y=%d", buf[0], buf[1], buf[2]);

    /* Verify communication by reading status register */
    if (0x00 != touch_dev_read_reg(dev, CF1124_REG_STATUS, buf, 1)) {
        return -2;
    }

    /* Status should not indicate bootcode mode (0x06) */
    if ((buf[0] & 0x0F) == 0x06) {
        LOG_W("cf1124 in bootcode mode, status=0x%02x", buf[0]);
        return -3;
    }

    /* Read resolution from chip */
    if (0x00 == touch_dev_read_reg(dev, CF1124_REG_XY_RES_HIGH, buf, 3)) {
        x_res = ((uint16_t)(buf[0] & 0xF0) << 4) | buf[1];
        y_res = ((uint16_t)(buf[0] & 0x0F) << 8) | buf[2];

        if (x_res > 0 && y_res > 0) {
            dev->touch.range_x = x_res;
            dev->touch.range_y = y_res;
            LOG_I("cf1124 resolution: %d x %d", x_res, y_res);
        }
    }

    /* Read max touches from chip */
    max_touches = CF1124_MAX_TOUCHES;
    if (0x00 == touch_dev_read_reg(dev, CF1124_REG_MAX_TOUCHES, buf, 1)) {
        if (buf[0] > 0 && buf[0] <= TOUCH_MAX_POINT_NUMBER) {
            max_touches = buf[0];
        }
    }

    rt_strncpy(dev->dev.drv_name, "cf1124", sizeof(dev->dev.drv_name));

    dev->dev.read_register      = read_register;
    dev->dev.parse_register     = parse_register;
    dev->dev.reset              = reset;
    dev->dev.get_default_rotate = get_default_rotate;

    dev->touch.point_num = max_touches;

    return 0;
}

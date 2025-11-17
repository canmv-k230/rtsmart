/* Copyright (c) 2023, Canaan Bright Sight Co., Ltd
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

#define DBG_TAG "cst3xx"
#define DBG_LVL DBG_WARNING
#define DBG_COLOR
#include <rtdbg.h>

struct cst3xx_point {
    uint8_t id_stat;
    uint8_t xh;
    uint8_t yh;
    uint8_t xl_yl;
    uint8_t z;
};

struct cst3xx_reg {
    struct cst3xx_point point1;
    uint8_t             point_num;
    uint8_t             const_0xab;
    struct cst3xx_point point2_5[4];
};

_Static_assert(sizeof(struct cst3xx_reg) < TOUCH_READ_REG_MAX_SIZE, "CST3xx reg data size > TOUCH_READ_REG_MAX_SIZE");

struct cst3xx_info {
    // 0xD1F4
    uint8_t tp_ntx;
    uint8_t resv0;
    uint8_t tp_nrx;
    uint8_t key_num;
    // 0xD1F8
    uint8_t tp_resx[2];
    uint8_t tp_resy[2];
    // 0xD1FC
    uint8_t boot_timer[2];
    uint8_t CACA[2];
    // 0xD200
    uint8_t project_id[2];
    uint8_t ic_type[2];
    // 0xD204
    uint8_t fw_build[2];
    uint8_t fw_minor;
    uint8_t fw_major;
    // 0xD208
    uint32_t checksum;
};

// APIs ///////////////////////////////////////////////////////////////////////
static int read_register(struct drv_touch_dev* dev, struct touch_register* reg)
{
    int     ret;
    uint8_t temp;
    uint8_t cmd[4];

    reg->time = rt_tick_get();

    cmd[0] = 0xD0;
    cmd[1] = 0x05;
    if (0x00 != (ret = touch_dev_write_read_reg(dev, cmd, 2, &temp, 1))) {
        LOG_E("CST3xx read reg failed");
        return -1;
    }

    int pt_num = temp & 0x0F;
    if ((pt_num == 0) || (pt_num > 5)) {
        rt_memset(reg, 0x00, sizeof(*reg));

        goto _clear;
    }

    cmd[0] = 0xD0;
    cmd[1] = 0x00;
    if (0x00 != touch_dev_write_read_reg(dev, cmd, 2, (uint8_t*)&reg->reg[0], pt_num * sizeof(struct cst3xx_point) + 2)) {
        LOG_E("CST3xx read reg failed");
        return -1;
    }

_clear:
    cmd[0] = 0xD0;
    cmd[1] = 0x05;
    cmd[2] = 0x00;
    if (0x00 != touch_dev_write_read_reg(dev, cmd, 3, NULL, 0)) {
        LOG_E("CST3xx read reg failed");
        return -1;
    }

    return 0;
}

static int parse_register(struct drv_touch_dev* dev, struct touch_register* reg, struct touch_point* result)
{
    int                   finger_num;
    rt_tick_t             time       = reg->time;
    struct rt_touch_data* point      = NULL;
    struct cst3xx_reg*    cst3xx_reg = (struct cst3xx_reg*)reg->reg;

    // Check constant value and key report
    if ((cst3xx_reg->const_0xab != 0xAB) || ((cst3xx_reg->point_num & 0x80) == 0x80)) {
        result->point_num = 0;
        return 0;
    }

    finger_num = cst3xx_reg->point_num & 0x7F;
    if (finger_num > 5) {
        result->point_num = 0;
        return 0;
    }

    if (finger_num > TOUCH_MAX_POINT_NUMBER) {
        LOG_W("CST3xx touch point %d > max %d", finger_num, TOUCH_MAX_POINT_NUMBER);
        finger_num = TOUCH_MAX_POINT_NUMBER;
    }
    result->point_num = finger_num;

    if (finger_num > 0) {
        // Parse first point
        point = &result->point[0];

        uint8_t temp        = cst3xx_reg->point1.id_stat;
        point->event        = ((temp & 0x0F) == 0x06) ? RT_TOUCH_EVENT_DOWN : RT_TOUCH_EVENT_NONE;
        point->track_id     = (temp & 0xF0) >> 4;
        point->width        = cst3xx_reg->point1.z;
        point->x_coordinate = (cst3xx_reg->point1.xh << 4) | ((cst3xx_reg->point1.xl_yl & 0xF0) >> 4);
        point->y_coordinate = (cst3xx_reg->point1.yh << 4) | (cst3xx_reg->point1.xl_yl & 0x0F);
        point->timestamp    = time;

        // Parse additional points (2-5)
        for (int i = 1; i < finger_num; i++) {
            point                          = &result->point[i];
            struct cst3xx_point* cst_point = &cst3xx_reg->point2_5[i - 1];

            temp                = cst_point->id_stat;
            point->event        = ((temp & 0x0F) == 0x06) ? RT_TOUCH_EVENT_DOWN : RT_TOUCH_EVENT_NONE;
            point->track_id     = (temp & 0xF0) >> 4;
            point->width        = cst_point->z;
            point->x_coordinate = (cst_point->xh << 4) | ((cst_point->xl_yl & 0xF0) >> 4);
            point->y_coordinate = (cst_point->yh << 4) | (cst_point->xl_yl & 0x0F);
            point->timestamp    = time;
        }
    }

    touch_dev_update_event(finger_num, result->point);

    return 0;
}

static int reset(struct drv_touch_dev* dev)
{
    if ((0 <= dev->pin.rst) && (63 >= dev->pin.rst)) {
        kd_pin_write(dev->pin.rst, 1 - dev->pin.rst_valid);
        rt_thread_mdelay(20);
        kd_pin_write(dev->pin.rst, dev->pin.rst_valid);
        rt_thread_mdelay(10);
        kd_pin_write(dev->pin.rst, 1 - dev->pin.rst_valid);
        rt_thread_mdelay(50);
    }

    return 0;
}

static int init_chip(struct drv_touch_dev* dev)
{
    int     ret;
    uint8_t cmd[4];
    uint8_t data[4];

    struct cst3xx_info info;

    // HYN_REG_MUT_DEBUG_INFO_MODE
    cmd[0] = 0xd1;
    cmd[1] = 0x01;
    if (0x00 != touch_dev_write_read_reg(dev, cmd, 2, NULL, 0)) {
        LOG_E("CST3xx init step 1 failed");
        return -1;
    }
    rt_thread_mdelay(1);

    cmd[0] = 0xd1;
    cmd[1] = 0xF4;
    if (0x00 != (ret = touch_dev_write_read_reg(dev, cmd, 2, (uint8_t*)&info, sizeof(info)))) {
        LOG_E("CST3xx init step 2 failed");
        return -1;
    }

    if ((0xCA != info.CACA[0]) || (0xCA != info.CACA[1])) {
        LOG_E("CST3xx read error flag\n");

        return -1;
    }

    dev->touch.point_num = 5;
    dev->touch.range_x   = (info.tp_resx[1] << 8) | info.tp_resx[0];
    dev->touch.range_y   = (info.tp_resy[1] << 8) | info.tp_resy[0];

    LOG_I("CST3xx Resoltion %dx%d", dev->touch.range_x, dev->touch.range_y);

    // HYN_REG_MUT_NORMAL_MODE
    cmd[0] = 0xd1;
    cmd[1] = 0x09;
    ret    = touch_dev_write_read_reg(dev, cmd, 2, NULL, 0);
    if (ret != 0) {
        LOG_E("CST3xx init step 3 failed");
        return -1;
    }

    return 0;
}

static int get_default_rotate(struct drv_touch_dev* dev) { return RT_TOUCH_ROTATE_SWAP_XY; }

int drv_touch_probe_cst328(struct drv_touch_dev* dev)
{
    uint8_t chip_id[4];
    int     ret;

    // probe CST328
    dev->i2c.addr      = 0x1A;
    dev->i2c.reg_width = 1;
    if (0x00 != (ret = init_chip(dev))) {
        // probe CST226SE
        dev->i2c.addr      = 0x5A;
        dev->i2c.reg_width = 1;
        if (0x00 != (ret = init_chip(dev))) {
            LOG_E("CST3xx init failed");
            return -2;
        }
    }

    if (0x1A == dev->i2c.addr) {
        rt_strncpy(dev->dev.drv_name, "cst328", sizeof(dev->dev.drv_name));
    } else if (0x5A == dev->i2c.addr) {
        rt_strncpy(dev->dev.drv_name, "cst226se", sizeof(dev->dev.drv_name));
    } else {
        rt_strncpy(dev->dev.drv_name, "cst3xx", sizeof(dev->dev.drv_name));
    }

    dev->dev.read_register      = read_register;
    dev->dev.parse_register     = parse_register;
    dev->dev.reset              = reset;
    dev->dev.get_default_rotate = get_default_rotate;

    LOG_I("CST3xx touch driver initialized successfully");

    return 0;
}

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

#define DBG_TAG "cst_mutcap"
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

_Static_assert(sizeof(struct cst3xx_reg) < TOUCH_READ_REG_MAX_SIZE, "CST3XX reg data size > TOUCH_READ_REG_MAX_SIZE");

// APIs ///////////////////////////////////////////////////////////////////////
static int read_register(struct drv_touch_dev* dev, struct touch_register* reg)
{
    uint8_t  temp;
    uint8_t* reg_data = (uint8_t*)&reg->reg[0];
    int      pt_num;

    reg->time = rt_tick_get();

    // Read point number from register 0x05
    if (0x00 != touch_dev_read_reg(dev, 0x05, &temp, 1)) {
        return -1;
    }

    pt_num = (temp & 0x0F);

    if ((0x00 == pt_num) || (5 < pt_num)) {
        // Clear the flag
        uint8_t clear_cmd[] = { 0x05, 0x00 };
        touch_dev_write_reg(dev, clear_cmd, sizeof(clear_cmd));
        reg->reg[0] = 0;
        return 0;
    }

    // Read touch data starting from register 0x00
    int read_size = pt_num * sizeof(struct cst3xx_point) + 2;
    if (0x00 != touch_dev_read_reg(dev, 0x00, reg_data, read_size)) {
        return -1;
    }

    // Clear the flag
    uint8_t clear_cmd[] = { 0x05, 0x00 };
    touch_dev_write_reg(dev, clear_cmd, sizeof(clear_cmd));

    return 0;
}

static int parse_register(struct drv_touch_dev* dev, struct touch_register* reg, struct touch_point* result)
{
    int                   finger_num;
    rt_tick_t             time       = reg->time;
    struct rt_touch_data* point      = NULL;
    struct cst3xx_reg*    cst3xx_reg = (struct cst3xx_reg*)reg->reg;
    uint8_t               temp;
    int                   result_index = 0, point_index = 0;

    if ((0xAB != cst3xx_reg->const_0xab) || (0x80 == (cst3xx_reg->point_num & 0x80))) {
        result->point_num = 0;
        return 0;
    }

    finger_num = cst3xx_reg->point_num & 0x7F;
    if (finger_num > TOUCH_MAX_POINT_NUMBER) {
        LOG_W("CST3XX touch point %d > max %d", finger_num, TOUCH_MAX_POINT_NUMBER);
        finger_num = TOUCH_MAX_POINT_NUMBER;
    }
    result->point_num = finger_num;

    if (finger_num) {
        // Parse first point
        temp                = cst3xx_reg->point1.id_stat;
        point               = &result->point[point_index];
        point->event        = (temp & 0x0F) == 0x06 ? RT_TOUCH_EVENT_DOWN : RT_TOUCH_EVENT_NONE;
        point->track_id     = (temp & 0xF0) >> 4;
        point->width        = cst3xx_reg->point1.z;
        point->x_coordinate = (cst3xx_reg->point1.xh << 4) | (cst3xx_reg->point1.xl_yl & 0xF0) >> 4;
        point->y_coordinate = (cst3xx_reg->point1.yh << 4) | (cst3xx_reg->point1.xl_yl & 0x0F);
        point->timestamp    = time;

        if (point->x_coordinate > dev->touch.range_x || point->y_coordinate > dev->touch.range_y) {
            point_index--;
        } else {
            point_index++;
        }

        // Parse remaining points if any
        if (finger_num > 1) {
            for (result_index = 1; result_index < finger_num; result_index++) {
                if (point_index >= TOUCH_MAX_POINT_NUMBER) {
                    break;
                }

                struct cst3xx_point* cst_point = &cst3xx_reg->point2_5[result_index - 1];

                temp                = cst_point->id_stat;
                point               = &result->point[point_index];
                point->event        = (temp & 0x0F) == 0x06 ? RT_TOUCH_EVENT_DOWN : RT_TOUCH_EVENT_NONE;
                point->track_id     = (temp & 0xF0) >> 4;
                point->width        = cst_point->z;
                point->x_coordinate = (cst_point->xh << 4) | (cst_point->xl_yl & 0xF0) >> 4;
                point->y_coordinate = (cst_point->yh << 4) | (cst_point->xl_yl & 0x0F);
                point->timestamp    = time;

                if (point->x_coordinate > dev->touch.range_x || point->y_coordinate > dev->touch.range_y) {
                    point_index--;
                } else {
                    point_index++;
                }
            }
        }

        result->point_num = point_index;
    }

    touch_dev_update_event(result->point_num, result->point);

    return 0;
}

static int reset(struct drv_touch_dev* dev)
{
    uint8_t cmd[3];

    if ((0 <= dev->pin.rst) && (63 >= dev->pin.rst)) {
        kd_pin_write(dev->pin.rst, 1 - dev->pin.rst_valid);
        rt_thread_mdelay(20);
        kd_pin_write(dev->pin.rst, dev->pin.rst_valid);
        rt_thread_mdelay(10);
        kd_pin_write(dev->pin.rst, 1 - dev->pin.rst_valid);
        rt_thread_mdelay(50);
    }

    // HYN_REG_MUT_DEBUG_INFO_MODE
    cmd[0] = 0xd1;
    cmd[1] = 0x01;
    cmd[2] = 0x01;
    if (0x00 != touch_dev_write_reg(dev, cmd, 3)) {
        return -1;
    }
    rt_thread_mdelay(1);

    // HYN_REG_MUT_NORMAL_MODE
    cmd[0] = 0xd1;
    cmd[1] = 0x09;
    if (0x00 != touch_dev_write_reg(dev, cmd, 2)) {
        return -1;
    }

    return 0;
}

static int get_default_rotate(struct drv_touch_dev* dev)
{
    // Default rotate based on chip type
    if (dev->i2c.addr == 0x1A) {
        // CST328
        return RT_TOUCH_ROTATE_SWAP_XY;
    } else if (dev->i2c.addr == 0x5A) {
        // CST226SE
        return RT_TOUCH_ROTATE_DEGREE_0;
    }
    return RT_TOUCH_ROTATE_DEGREE_0;
}

int drv_touch_probe_cst328(struct drv_touch_dev* dev)
{
    uint8_t  data[4];
    uint16_t chip, proj;
    int      ret;

    // Try CST328 first (address 0x1A)
    dev->i2c.addr      = 0x1A;
    dev->i2c.reg_width = 1;

    rt_thread_mdelay(50); // wait touch startup.

    // Check if CST328 is present
    uint8_t cmd_d1[] = { 0xd1, 0x01, 0x01 };
    if (0x00 != touch_dev_write_reg(dev, cmd_d1, sizeof(cmd_d1))) {
        // Try CST226SE (address 0x5A)
        dev->i2c.addr = 0x5A;
        if (0x00 != touch_dev_write_reg(dev, cmd_d1, sizeof(cmd_d1))) {
            return -2;
        }
    }

    // HYN_REG_MUT_DEBUG_INFO_FW_VERSION
    uint8_t cmd_d2[] = { 0xd2, 0x04 };
    if (0x00 != touch_dev_read_reg(dev, 0xd2, data, 4)) {
        return -3;
    }
    proj = (data[0] << 8) | data[1];
    chip = (data[2] << 8) | data[3];

    rt_kprintf("CST3XX, ChipID: 0x%04X, ProjectID: 0x%04X, I2C addr: 0x%02X\n", chip, proj, dev->i2c.addr);

    if (dev->i2c.addr == 0x1A) {
        rt_strncpy(dev->dev.drv_name, "cst328", sizeof(dev->dev.drv_name));
    } else if (dev->i2c.addr == 0x5A) {
        rt_strncpy(dev->dev.drv_name, "cst226se", sizeof(dev->dev.drv_name));
    } else {
        rt_strncpy(dev->dev.drv_name, "cst3xx", sizeof(dev->dev.drv_name));
    }

    dev->dev.read_register      = read_register;
    dev->dev.parse_register     = parse_register;
    dev->dev.reset              = reset;
    dev->dev.get_default_rotate = get_default_rotate;

    dev->touch.range_x   = TOUCH_CST328_DFT_RANGE_X;
    dev->touch.range_y   = TOUCH_CST328_DFT_RANGE_Y;
    dev->touch.point_num = 5;

    return 0;
}

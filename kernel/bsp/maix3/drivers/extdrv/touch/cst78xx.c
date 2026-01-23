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

#define DBG_TAG "cst816t"
#define DBG_LVL DBG_WARNING
#include <rtdbg.h>

/* CST816T register definitions */
#define CST816T_ADDRESS 0x15

#define REG_GESTURE_ID      0x01
#define REG_FINGER_NUM      0x02
#define REG_XPOS_H          0x03
#define REG_XPOS_L          0x04
#define REG_YPOS_H          0x05
#define REG_YPOS_L          0x06
#define REG_CHIP_ID         0xA7
#define REG_PROJ_ID         0xA8
#define REG_FW_VERSION      0xA9
#define REG_FACTORY_ID      0xAA
#define REG_SLEEP_MODE      0xE5
#define REG_IRQ_CTL         0xFA
#define REG_LONG_PRESS_TICK 0xEB
#define REG_MOTION_MASK     0xEC
#define REG_DIS_AUTOSLEEP   0xFE

#define CHIPID_CST716  0x20
#define CHIPID_CST816S 0xB4
#define CHIPID_CST816T 0xB5
#define CHIPID_CST816D 0xB6

#define GESTURE_NONE         0x00
#define GESTURE_SWIPE_DOWN   0x02
#define GESTURE_SWIPE_UP     0x01
#define GESTURE_SWIPE_LEFT   0x03
#define GESTURE_SWIPE_RIGHT  0x04
#define GESTURE_SINGLE_CLICK 0x05
#define GESTURE_DOUBLE_CLICK 0x0B
#define GESTURE_LONG_PRESS   0x0C

/* CST816T register structure */
struct cst816t_reg {
    uint8_t gesture_id; // 0x01
    uint8_t finger_num; // 0x02
    uint8_t x_h; // 0x03
    uint8_t x_l; // 0x04
    uint8_t y_h; // 0x05
    uint8_t y_l; // 0x06
};

_Static_assert(sizeof(struct cst816t_reg) <= TOUCH_READ_REG_MAX_SIZE, "CST816T reg data size > TOUCH_READ_REG_MAX_SIZE");

// APIs ///////////////////////////////////////////////////////////////////////
static int read_register(struct drv_touch_dev* dev, struct touch_register* reg)
{
    reg->time = rt_tick_get();

    return touch_dev_read_reg(dev, REG_GESTURE_ID, (uint8_t*)&reg->reg[0], sizeof(struct cst816t_reg));
}

static int parse_register(struct drv_touch_dev* dev, struct touch_register* reg, struct touch_point* result)
{
    const uint8_t event_map[8] = {
        RT_TOUCH_EVENT_NONE, // GESTURE_NONE
        RT_TOUCH_EVENT_UP, // GESTURE_SWIPE_UP (treated as up event)
        RT_TOUCH_EVENT_DOWN, // GESTURE_SWIPE_DOWN (treated as down event)
        RT_TOUCH_EVENT_MOVE, // GESTURE_SWIPE_LEFT (treated as move)
        RT_TOUCH_EVENT_MOVE, // GESTURE_SWIPE_RIGHT (treated as move)
        RT_TOUCH_EVENT_DOWN, // GESTURE_SINGLE_CLICK (treated as down)
        RT_TOUCH_EVENT_NONE, // Reserved
        RT_TOUCH_EVENT_NONE // Reserved
    };

    uint8_t  xh, xl, yh, yl;
    uint16_t point_x, point_y;
    int      finger_num;
    int      result_index = 0;

    rt_tick_t time = reg->time;

    struct rt_touch_data* point       = NULL;
    struct cst816t_reg*   cst816t_reg = (struct cst816t_reg*)reg->reg;

    result->point_num = 0;

    finger_num = cst816t_reg->finger_num & 0x0F;

    // CST816T supports only single touch
    if (finger_num > 1) {
        finger_num = 1;
    }

    if (finger_num > TOUCH_MAX_POINT_NUMBER) {
        LOG_W("CST816T touch point %d > max %d", finger_num, TOUCH_MAX_POINT_NUMBER);
        finger_num = TOUCH_MAX_POINT_NUMBER;
    }

    if (finger_num) {
        for (int i = 0; i < finger_num; i++) {
            point = &result->point[i];

            xh      = cst816t_reg->x_h & 0x0F;
            xl      = cst816t_reg->x_l;
            point_x = (xh << 8) | xl;

            if (point_x > dev->touch.range_x) {
                continue;
            }

            yh      = cst816t_reg->y_h & 0x0F;
            yl      = cst816t_reg->y_l;
            point_y = (yh << 8) | yl;

            if (point_y > dev->touch.range_y) {
                continue;
            }

            // Map gesture to touch event
            uint8_t gesture = cst816t_reg->gesture_id;
            uint8_t event   = RT_TOUCH_EVENT_NONE;

            if (gesture <= GESTURE_SINGLE_CLICK) {
                event = event_map[gesture];
            } else if (gesture == GESTURE_DOUBLE_CLICK) {
                event = RT_TOUCH_EVENT_DOWN; // Treat double click as down
            } else if (gesture == GESTURE_LONG_PRESS) {
                event = RT_TOUCH_EVENT_MOVE; // Treat long press as move
            }

            point->event        = event;
            point->track_id     = 0; // Single touch only
            point->width        = 1; // Default width for single touch
            point->x_coordinate = point_x;
            point->y_coordinate = point_y;
            point->timestamp    = time;

            result_index++;
        }
    }

    result->point_num = result_index;

    // CST816T doesn't have hardware event generation, so we need to update events
    if (result_index > 0) {
        touch_dev_update_event(result_index, result->point);
    }

    return 0;
}

static int reset(struct drv_touch_dev* dev)
{
    uint8_t dis_auto_sleep = 0xFF;
    uint8_t chip_id        = 0;

    // Perform hardware reset if reset pin is available
    if ((0 <= dev->pin.rst) && (63 >= dev->pin.rst)) {
        kd_pin_write(dev->pin.rst, 1 - dev->pin.rst_valid);
        rt_thread_mdelay(100);
        kd_pin_write(dev->pin.rst, dev->pin.rst_valid);
        rt_thread_mdelay(10);
        kd_pin_write(dev->pin.rst, 1 - dev->pin.rst_valid);
        rt_thread_mdelay(100);
    }

    // Read chip ID
    if (0x00 != touch_dev_read_reg(dev, REG_CHIP_ID, &chip_id, 1)) {
        LOG_E("Failed to read CST816T chip ID");
        return -1;
    }

    LOG_I("CST816T chip ID: 0x%02X", chip_id);

    // Disable auto sleep (except for CST716)
    if (chip_id != CHIPID_CST716) {
        if (0x00 != touch_dev_write_reg(dev, (uint8_t[]) { REG_DIS_AUTOSLEEP, dis_auto_sleep }, 2)) {
            LOG_W("Failed to disable auto sleep");
        }
    }

    // Configure interrupt mode for continuous reporting
    uint8_t irq_config = 0x60; // Default to mode_change (continuous with coordinate)
    if (0x00 != touch_dev_write_reg(dev, (uint8_t[]) { REG_IRQ_CTL, irq_config }, 2)) {
        LOG_W("Failed to configure interrupt");
    }

    rt_thread_mdelay(50); // Wait for initialization to complete

    return 0;
}

static int get_default_rotate(struct drv_touch_dev* dev) { return RT_TOUCH_ROTATE_DEGREE_0; }

static int get_info(struct drv_touch_dev* dev)
{
    uint8_t chip_id    = 0;
    uint8_t fw_version = 0;

    // Read chip information
    if (0x00 != touch_dev_read_reg(dev, REG_CHIP_ID, &chip_id, 1)) {
        return -1;
    }

    if (0x00 != touch_dev_read_reg(dev, REG_FW_VERSION, &fw_version, 1)) {
        return -2;
    }

    // Set default ranges based on common CST816T resolutions
    dev->touch.range_x   = TOUCH_CST78XX_DFT_RANGE_X; // Default X resolution
    dev->touch.range_y   = TOUCH_CST78XX_DFT_RANGE_Y; // Default Y resolution
    dev->touch.point_num = 1; // Single touch only

    LOG_I("CST816T-> Chip ID: 0x%02X, FW Version: 0x%02X", chip_id, fw_version);

    return 0;
}

int drv_touch_probe_cst_78xx(struct drv_touch_dev* dev)
{
    uint8_t chip_id = 0;

    // CST816T uses 7-bit I2C address 0x15
    dev->i2c.addr      = CST816T_ADDRESS;
    dev->i2c.reg_width = 1; // 1-byte register addresses

    // Read chip ID to verify the device
    if (0x00 != touch_dev_read_reg(dev, REG_CHIP_ID, &chip_id, 1)) {
        return -1;
    }

    // Check if it's a CST816 series chip
    if (!(chip_id == CHIPID_CST716 || chip_id == CHIPID_CST816S || chip_id == CHIPID_CST816T || chip_id == CHIPID_CST816D)) {
        LOG_W("CST816T probe failed: unknown chip ID 0x%02X", chip_id);
        return -2;
    }

    // Perform reset and initialization
    if (reset(dev) != 0) {
        return -3;
    }

    // Get device information
    if (get_info(dev) != 0) {
        return -4;
    }

    // Set driver name
    rt_strncpy(dev->dev.drv_name, "cst78xx", sizeof(dev->dev.drv_name));

    // Register callbacks
    dev->dev.read_register      = read_register;
    dev->dev.parse_register     = parse_register;
    dev->dev.reset              = reset;
    dev->dev.get_default_rotate = get_default_rotate;

    LOG_I("cst816t-> resolution %dx%d, max touches: %d", dev->touch.range_x, dev->touch.range_y, dev->touch.point_num);

    return 0;
}

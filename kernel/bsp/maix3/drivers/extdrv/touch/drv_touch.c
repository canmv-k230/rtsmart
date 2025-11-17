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
#include "drv_gpio.h"
#include "rtconfig.h"
#include "rtdef.h"
#include "rthw.h"
#include "rtservice.h"
#include "rtthread.h"

#include <ioremap.h>
#include <lwp_user_mm.h>

#define DBG_TAG "drv_touch"
#define DBG_LVL DBG_WARNING
#define DBG_COLOR
#include <rtdbg.h>

#define TOUCH_TIMEOUT_MS 1000

#define TOUCH_THREAD_MQ_INT_FLAG 0x01
#define TOUCH_THREAD_MQ_RST_FLAG 0x02

int touch_dev_write_reg(struct drv_touch_dev* dev, rt_uint8_t* buffer, rt_size_t length)
{
    struct rt_i2c_msg msg = {
        .addr  = dev->i2c.addr,
        .flags = RT_I2C_WR,
        .buf   = buffer,
        .len   = length,
    };

    if (0x01 == rt_i2c_transfer(dev->i2c.bus, &msg, 1)) {
        return 0;
    } else {
        return -1;
    }
}

int touch_dev_read_reg(struct drv_touch_dev* dev, rt_uint16_t addr, rt_uint8_t* buffer, rt_size_t length)
{
    rt_uint8_t _addr_buf[2];

    if (0x01 == dev->i2c.reg_width) {
        _addr_buf[0] = addr & 0xFF;
    } else if (0x02 == dev->i2c.reg_width) {
        _addr_buf[0] = (addr >> 8) & 0xFF;
        _addr_buf[1] = addr & 0xFF;
    }

    struct rt_i2c_msg msgs[2] = {
        {
            .addr  = dev->i2c.addr,
            .flags = RT_I2C_WR,
            .buf   = &_addr_buf[0],
            .len   = dev->i2c.reg_width,
        },
        {
            .addr  = dev->i2c.addr,
            .flags = RT_I2C_RD,
            .buf   = buffer,
            .len   = length,
        },
    };

    if (0x02 == rt_i2c_transfer(dev->i2c.bus, msgs, 2)) {
        return 0;
    } else {
        return -1;
    }
}

int touch_dev_write_read_reg(struct drv_touch_dev* dev, uint8_t* send_buffer, uint32_t send_len, uint8_t* read_buffer,
                             uint32_t read_len)
{
    struct rt_i2c_msg msgs[2];
    int               msg_count = 0;

    if (send_buffer && send_len) {
        msgs[msg_count].addr  = dev->i2c.addr;
        msgs[msg_count].flags = RT_I2C_WR;
        msgs[msg_count].buf   = send_buffer;
        msgs[msg_count].len   = send_len;

        msg_count++;
    }

    if (read_buffer && read_len) {
        msgs[msg_count].addr  = dev->i2c.addr;
        msgs[msg_count].flags = RT_I2C_RD;
        msgs[msg_count].buf   = read_buffer;
        msgs[msg_count].len   = read_len;

        msg_count++;
    }

    if (msg_count == rt_i2c_transfer(dev->i2c.bus, msgs, msg_count)) {
        return 0;
    } else {
        return -1;
    }
}

void touch_dev_update_event(int finger_num, struct rt_touch_data* point)
{
    static int                  last_finger_num = 0;
    static struct rt_touch_data last_point[TOUCH_MAX_POINT_NUMBER];
    static rt_tick_t            last_timestamp = 0; //, tmp_timestamp;      // Track the timestamp of the last touch event
    // static rt_tick_t last_click_timestamp[TOUCH_MAX_POINT_NUMBER] = {0}; // Track double-click timing

    // rt_uint16_t tmpx, tmpy;
    rt_bool_t new_session = RT_FALSE;

    // Check if too much time has passed since the last touch event
    if (finger_num > 0 && (point[0].timestamp - last_timestamp > TOUCH_TIMEOUT_MS)) {
        // A new touch event is considered after a timeout
        new_session = RT_TRUE;
        LOG_D("New touch session detected due to timeout.\n");

        // If needed, mark all previous touches as lifted
        for (int i = 0; i < last_finger_num; i++) {
            last_point[i].event = RT_TOUCH_EVENT_UP;
            LOG_D("Finger %d: Timeout - Finger lifted\n", last_point[i].track_id);
        }

        // Reset the last finger number since it's a new session
        last_finger_num = 0;
    }

    // Update the timestamp of the last touch event
    last_timestamp = point[0].timestamp;

    // Process current touch data
    for (int i = 0; i < finger_num; i++) {
        struct rt_touch_data* current_point = &point[i];
        struct rt_touch_data* last_touch    = &last_point[i];

        // tmp_timestamp = last_click_timestamp[i];
        // // Update last click timestamp if it's a valid touch
        // last_click_timestamp[i] = current_point->timestamp;

        // tmpx = abs(current_point->x_coordinate - last_touch->x_coordinate);
        // tmpy = abs(current_point->y_coordinate - last_touch->y_coordinate);
        // // Double-click detection: Check if this touch is at the same position as the last one within the threshold
        // if (i < last_finger_num &&
        //     !new_session && (tmpx < 5) && (tmpy < 5) &&
        //     (current_point->timestamp - tmp_timestamp) < 100) {

        //     // Double-click detected, ignore this event
        //     LOG_D("Double-click detected for finger %d, filtering out.\n", current_point->track_id);
        //     continue;
        // }

        if (i < last_finger_num && !new_session) {
            // Check if the touch point has moved
            if (current_point->x_coordinate != last_touch->x_coordinate
                || current_point->y_coordinate != last_touch->y_coordinate) {
                // Update event to move
                current_point->event = RT_TOUCH_EVENT_MOVE;
            }
        } else {
            // New finger detected or new session
            current_point->event = RT_TOUCH_EVENT_DOWN;
        }

        // Print touch info for debugging (optional)
        LOG_D("Finger %d: X = %d, Y = %d, Event = %d, Timestamp = %ld\n", current_point->track_id, current_point->x_coordinate,
              current_point->y_coordinate, current_point->event, current_point->timestamp);

        // Update the last point data
        rt_memcpy(last_touch, current_point, sizeof(struct rt_touch_data));
    }

    // Check if fingers have been lifted
    if (finger_num < last_finger_num && !new_session) {
        for (int i = finger_num; i < last_finger_num; i++) {
            struct rt_touch_data* last_touch = &last_point[i];
            // Finger lifted
            last_touch->event = RT_TOUCH_EVENT_UP;
            LOG_D("Finger %d: Lifted, Event = %d, Timestamp = %ld\n", last_touch->track_id, last_touch->event,
                  last_touch->timestamp);
        }
    }

    // Update last finger number
    last_finger_num = finger_num;
}

static rt_size_t drv_touch_parse(struct drv_touch_dev* dev, struct touch_register* reg, struct rt_touch_data* point,
                                 rt_size_t* point_cnt)
{
    // This is the single source of truth for the touch state
    static int                  last_finger_num = 0;
    static struct rt_touch_data last_point[TOUCH_MAX_POINT_NUMBER];

    struct touch_point parsed_point;

    if (!dev->dev.parse_register) {
        return 0;
    }

    if (0x00 != dev->dev.parse_register(dev, reg, &parsed_point)) {
        LOG_E("Touch parse register failed.");
        return 0;
    }

    int current_finger_num = parsed_point.point_num;
    // Temporary buffer to hold all generated events (down, move, up)
    struct rt_touch_data all_events[TOUCH_MAX_POINT_NUMBER];
    int                  total_events = 0;
    rt_tick_t            timestamp    = rt_tick_get();

    // PART 1: Determine DOWN and MOVE events
    for (int i = 0; i < current_finger_num; i++) {
        struct rt_touch_data* current_touch = &parsed_point.point[i];
        rt_bool_t             is_new        = RT_TRUE;

        for (int j = 0; j < last_finger_num; j++) {
            if (current_touch->track_id == last_point[j].track_id) {
                current_touch->event = RT_TOUCH_EVENT_MOVE;
                is_new               = RT_FALSE;
                break;
            }
        }
        if (is_new) {
            current_touch->event = RT_TOUCH_EVENT_DOWN;
        }
        current_touch->timestamp = timestamp;
        rt_memcpy(&all_events[total_events++], current_touch, sizeof(struct rt_touch_data));
    }

    // PART 2: Determine UP events
    for (int i = 0; i < last_finger_num; i++) {
        rt_bool_t is_lifted = RT_TRUE;
        for (int j = 0; j < current_finger_num; j++) {
            if (last_point[i].track_id == parsed_point.point[j].track_id) {
                is_lifted = RT_FALSE;
                break;
            }
        }
        if (is_lifted) {
            if (total_events < TOUCH_MAX_POINT_NUMBER) {
                rt_memcpy(&all_events[total_events], &last_point[i], sizeof(struct rt_touch_data));
                all_events[total_events].event     = RT_TOUCH_EVENT_UP;
                all_events[total_events].timestamp = timestamp;
                total_events++;
            }
        }
    }

    // PART 3: Update state for the next cycle
    last_finger_num = current_finger_num;
    rt_memcpy(last_point, &parsed_point.point[0], current_finger_num * sizeof(struct rt_touch_data));

    // PART 4: Copy final event list to user buffer
    if (total_events > 0) {
        rt_size_t req_buffer_size = 0, req_finger_num = *point_cnt;

        req_finger_num  = (req_finger_num > total_events) ? total_events : req_finger_num;
        req_buffer_size = req_finger_num * sizeof(struct rt_touch_data);

        *point_cnt = req_finger_num;
        rt_memcpy(point, all_events, req_buffer_size);

        return req_buffer_size;
    }

    return 0;
}

#ifdef TOUCH_DRV_MODEL_INT_WITH_THREAD
static void touch_int_irq(void* args)
{
    struct drv_touch_dev* dev = args;

    uint32_t flag = TOUCH_THREAD_MQ_INT_FLAG;

    rt_mq_send(&dev->thr.ctrl_mq, &flag, sizeof(uint32_t));
}

static void touch_read_thread(void* args)
{
    struct drv_touch_dev* dev = args;

    uint32_t              recv;
    struct touch_register reg, dump[2];

    while (1) {
        if (RT_EOK == rt_mq_recv(&dev->thr.ctrl_mq, &recv, sizeof(uint32_t), RT_WAITING_FOREVER)) {
            if (TOUCH_THREAD_MQ_INT_FLAG == recv) {
                if (dev->dev.read_register) {
                    if (0x00 != dev->dev.read_register(dev, &reg)) {
                        LOG_E("Touch device read register faild.");

                        continue;
                    }

                    if (RT_EOK != rt_mq_send(&dev->thr.read_mq, &reg, sizeof(reg))) {
                        // drop oldest message
                        rt_mq_recv(&dev->thr.read_mq, &dump[0], sizeof(dump), RT_WAITING_NO);

                        rt_mq_send(&dev->thr.read_mq, &reg, sizeof(reg));
                    }
                } else {
                    LOG_W("Touch device not impl read_register");
                }
            } else if (TOUCH_THREAD_MQ_RST_FLAG == recv) {
                if (dev->dev.reset) {
                    if (0x00 != dev->dev.reset(dev)) {
                        LOG_E("reset touch failed");
                    }

                    rt_thread_delay(rt_tick_from_millisecond(30));

                    rt_sem_release(&dev->thr.ctrl_sem);
                }
            } else {
                LOG_W("Unknown message %d\n", recv);
            }
        }
    }
}

static rt_size_t drv_touch_read(struct rt_touch_device* touch, void* buf, rt_size_t len)
{
    static rt_size_t last_finger_num = 0;
    const rt_tick_t  one_sec_tick    = rt_tick_from_millisecond(1000);

    struct drv_touch_dev* dev = touch->config.user_data;

    rt_tick_t             time;
    struct touch_register reg;

    // make a fake interrupt
    if (dev->pin.fake_intr) {
        uint32_t flag = TOUCH_THREAD_MQ_INT_FLAG;
        rt_mq_send(&dev->thr.ctrl_mq, &flag, sizeof(uint32_t));
        rt_thread_mdelay(1);
    }

    while (1) {
        if (RT_EOK != rt_mq_recv(&dev->thr.read_mq, &reg, sizeof(reg), rt_tick_from_millisecond(3))) {
            // No touch data, but we might need to generate an UP event
            if (last_finger_num > 0) {
                break;
            }
            return 0;
        }
        time = rt_tick_get() - reg.time;
        if (one_sec_tick > time) {
            break;
        }
    }

    rt_size_t             result, req_finger_num = len / sizeof(struct rt_touch_data);
    struct rt_touch_data* req_point = (struct rt_touch_data*)buf;

    result = drv_touch_parse(dev, &reg, req_point, &req_finger_num);

    last_finger_num = result / sizeof(struct rt_touch_data);

    return result;
}
#else
static volatile int int_flag = 0x00;

static void touch_int_irq(void* args) { int_flag = 1; }

static rt_size_t drv_touch_read(struct rt_touch_device* touch, void* buf, rt_size_t len)
{
    struct touch_register reg;

    struct drv_touch_dev* dev = touch->config.user_data;

    rt_size_t             req_finger_num = len / sizeof(struct rt_touch_data);
    struct rt_touch_data* req_point      = (struct rt_touch_data*)buf;

    if ((0x00 == dev->pin.fake_intr) && (0x00 == int_flag)) {
        return 0;
    }
    int_flag = 0x00;

    if (dev->dev.read_register) {
        if (RT_EOK != dev->dev.read_register(dev, &reg)) {
            LOG_E("Touch device read register faild.");
            return 0;
        }

        return drv_touch_parse(dev, &reg, req_point, &req_finger_num);
    } else {
        LOG_W("Touch device not impl read_register");
    }

    return 0;
}
#endif

static rt_err_t drv_touch_control(struct rt_touch_device* touch, int cmd, void* arg)
{
    struct drv_touch_dev* dev = touch->config.user_data;

    switch (cmd) {
    case RT_TOUCH_CTRL_GET_INFO: {
        lwp_put_to_user(arg, &touch->info, sizeof(struct rt_touch_info));

        return RT_EOK;
    } break;
    case RT_TOUCH_CTRL_RESET: {
        if (!dev) {
            return RT_EINVAL;
        }

        if (dev->dev.reset) {
#ifdef TOUCH_DRV_MODEL_INT_WITH_THREAD
            // push command to thread
            uint32_t flag = TOUCH_THREAD_MQ_RST_FLAG;
            rt_mq_send_wait(&dev->thr.ctrl_mq, &flag, sizeof(uint32_t), RT_WAITING_FOREVER);

            if (RT_EOK != rt_sem_take(&dev->thr.ctrl_sem, rt_tick_from_millisecond(200))) {
                LOG_W("Wait Thread reset touch timeout.");
                return -RT_ETIMEOUT;
            }

            return RT_EOK;
#else
            return dev->dev.reset(dev);
#endif
        } else {
            LOG_W("Touch device not impl get_default_rotate");
            return RT_EOK;
        }
    } break;
    case RT_TOUCH_CTRL_GET_DFT_ROTATE: {
        if (!dev) {
            return RT_EINVAL;
        }

        int rotate = RT_TOUCH_ROTATE_DEGREE_0;

        if (dev->dev.get_default_rotate) {
            rotate = dev->dev.get_default_rotate(dev);
        } else {
            LOG_W("Touch device not impl get_default_rotate");
        }

        lwp_put_to_user(arg, &rotate, sizeof(rotate));

        return RT_EOK;
    } break;
    case RT_TOUCH_CTRL_GET_DEVICE_CFG: {
        if (!dev) {
            return RT_EINVAL;
        }

        lwp_put_to_user(arg, &dev->config, sizeof(struct drv_touch_config));

        return RT_EOK;
    } break;
    case RT_TOUCH_CTRL_DISABLE_INT: {
        if (!dev) {
            return RT_EINVAL;
        }

        if (0x00 == dev->pin.fake_intr) {
            dev->pin.fake_intr = 1;

            kd_pin_detach_irq(dev->pin.intr);
        }

        return RT_EOK;
    } break;
    default: {
        LOG_E("touch not support cmd 0x%08X", cmd);
    } break;
    }

    return -RT_ENOSYS;
}

static struct rt_touch_ops drv_touch_ops = {
    .touch_readpoint = drv_touch_read,
    .touch_control   = drv_touch_control,
};

static const drv_touch_probe touch_probes[] = {

#if defined TOUCH_TYPE_FT5316
    drv_touch_probe_ft5x16,
#endif // TOUCH_TYPE_FT5316
#if defined TOUCH_TYPE_FT5406
    drv_touch_probe_ft5x06,
#endif // TOUCH_TYPE_FT5406
#if defined TOUCH_TYPE_CST128
    drv_touch_probe_cst128,
#endif // TOUCH_TYPE_CST128
#if defined TOUCH_TYPE_CST328
    drv_touch_probe_cst328,
#endif // TOUCH_TYPE_CST328
#if defined TOUCH_TYPE_CHSC5XXX
    drv_touch_probe_chsc5xxx,
#endif // TOUCH_TYPE_CHSC5XXX
#if defined TOUCH_TYPE_GT911
    drv_touch_probe_gt911,
#endif // TOUCH_TYPE_GT911
    NULL
};

static struct drv_touch_dev* drv_touch_create_device(struct drv_touch_config* cfg)
{
    int ret;

    struct drv_touch_dev* dev = NULL;
    char                  dev_name[RT_NAME_MAX];

    struct rt_i2c_bus_device* i2c_bus = NULL;
    char                      i2c_name[RT_NAME_MAX];

    if (!cfg) {
        LOG_E("Invalid config");

        goto _err_invalid_cfg;
    }

    dev = rt_malloc_align(sizeof(struct drv_touch_dev), RT_CPU_CACHE_LINE_SZ);
    if (!dev) {
        LOG_E("alloc failed");

        goto _err_alloc_dev;
    }
    rt_memset(dev, 0x00, sizeof(struct drv_touch_dev));

    snprintf(i2c_name, sizeof(i2c_name) - 1, "i2c%d", cfg->i2c_bus_index);
    if (NULL == (i2c_bus = rt_i2c_bus_device_find(i2c_name))) {
        LOG_E("Can't found i2c bus %d\n", cfg->i2c_bus_index);

        goto _err_find_i2c_bus;
    }

    if (RT_EOK != (ret = rt_device_open((rt_device_t)i2c_bus, RT_DEVICE_FLAG_RDWR))) {
        LOG_E("Open Touch Device i2c%d failed: %d", cfg->i2c_bus_index, ret);

        goto _err_open_i2c_bus;
    }

    if (0x00 != cfg->i2c_bus_speed) {
        if (RT_EOK != (ret = rt_device_control((rt_device_t)i2c_bus, RT_I2C_DEV_CTRL_CLK, (uint32_t*)&cfg->i2c_bus_speed))) {
            LOG_E("Set Touch Device i2c%d failed: %d", cfg->i2c_bus_index, ret);
            goto _err_set_i2c_clock;
        }
    }

    dev->pin.intr      = cfg->pin_intr;
    dev->pin.intr_edge = cfg->intr_value ? GPIO_PE_RISING : GPIO_PE_FALLING;
    dev->pin.rst       = cfg->pin_reset;
    dev->pin.rst_valid = cfg->reset_value;

    // setup reset pin
    if ((0 <= cfg->pin_reset) && (63 >= cfg->pin_reset)) {
        kd_pin_mode(cfg->pin_reset, GPIO_DM_OUTPUT);
        kd_pin_write(cfg->pin_reset, cfg->reset_value ? 0 : 1);
    }

    // setup intr pin
    if ((0 <= cfg->pin_intr) && (63 >= cfg->pin_intr)) {
        dev->pin.fake_intr = 0;

        kd_pin_mode(cfg->pin_intr, GPIO_DM_INPUT);
        kd_pin_attach_irq(cfg->pin_intr, dev->pin.intr_edge, touch_int_irq, dev);
    } else {
        dev->pin.fake_intr = 1;
    }

    dev->i2c.index     = cfg->i2c_bus_index;
    dev->i2c.speed     = cfg->i2c_bus_speed;
    dev->i2c.bus       = i2c_bus;
    dev->i2c.addr      = 0x00;
    dev->i2c.reg_width = 0x01;
    strncpy(dev->i2c.name, i2c_name, sizeof(dev->i2c.name));

    /* Probe for touch controller */
    for (int i = 0; touch_probes[i]; i++) {
        if (0x00 == touch_probes[i](dev)) {
            break;
        }
    }

    if ((NULL == dev->dev.read_register) || (NULL == dev->dev.parse_register)) {
        LOG_E("touch probe failed.");

        goto _err_probe_device;
    }
    rt_kprintf("find touch %s on i2c%d\n", dev->dev.drv_name, dev->i2c.index);

    // apply user range_x and range_y
    if (0x00 != cfg->range_x) {
        dev->touch.range_x = cfg->range_x;
    }
    if (0x00 != cfg->range_y) {
        dev->touch.range_y = cfg->range_y;
    }

#ifdef TOUCH_DRV_MODEL_INT_WITH_THREAD
    /* Initialize thread resources */
    rt_sem_init(&dev->thr.ctrl_sem, "touch_ctrl", 0, RT_IPC_FLAG_FIFO);
    rt_mq_init(&dev->thr.ctrl_mq, "touch_int", &dev->thr.ctrl_mq_pool[0], sizeof(uint32_t), sizeof(dev->thr.ctrl_mq_pool),
               RT_IPC_FLAG_FIFO);
    rt_mq_init(&dev->thr.read_mq, "touch_read", &dev->thr.read_mq_pool[0], sizeof(touch_read_mq_msg_type),
               sizeof(dev->thr.read_mq_pool), RT_IPC_FLAG_FIFO);
    rt_thread_init(&dev->thr.thr, "touch_read", touch_read_thread, dev, &dev->thr.thread_stack[0],
                   sizeof(dev->thr.thread_stack), TOUCH_DRV_THREAD_PRIO, 5);
#endif

    if (0x00 == dev->pin.fake_intr) {
        kd_pin_irq_enable(cfg->pin_intr, KD_GPIO_IRQ_ENABLE);
    }

    snprintf(dev_name, sizeof(dev_name) - 1, "touch%d", cfg->touch_dev_index);

    /* Register touch device */
    dev->dev.touch.config.dev_name  = (char*)dev_name;
    dev->dev.touch.config.user_data = dev;

    dev->dev.touch.info.type   = RT_TOUCH_TYPE_CAPACITANCE;
    dev->dev.touch.info.vendor = RT_TOUCH_VENDOR_UNKNOWN;

    dev->dev.touch.info.point_num = dev->touch.point_num;
    dev->dev.touch.info.range_x   = dev->touch.range_x;
    dev->dev.touch.info.range_y   = dev->touch.range_y;

    dev->dev.touch.ops = &drv_touch_ops;

    if (RT_EOK != (ret = rt_hw_touch_register(&dev->dev.touch, dev->dev.touch.config.dev_name, 0, RT_NULL))) {
        LOG_E("Register Touch Device failed %d.", ret);
        goto _err_register_device;
    }

    dev->dev.dev_index = cfg->touch_dev_index;
    rt_memcpy(&dev->config, cfg, sizeof(dev->config));

    dev->config.range_x = dev->touch.range_x;
    dev->config.range_y = dev->touch.range_y;

#ifdef TOUCH_DRV_MODEL_INT_WITH_THREAD
    rt_thread_startup(&dev->thr.thr);
#endif

    LOG_I("Touch device %s registered successfully", name);

    return dev;

_err_register_device:
    if (0x00 == dev->pin.fake_intr) {
        kd_pin_detach_irq(cfg->pin_intr);
    }

#ifdef TOUCH_DRV_MODEL_INT_WITH_THREAD
    rt_sem_detach(&dev->thr.ctrl_sem);
    rt_mq_detach(&dev->thr.ctrl_mq);
    rt_mq_detach(&dev->thr.read_mq);
    rt_thread_detach(&dev->thr.thr);
#endif

_err_probe_device:
_err_set_i2c_clock:
    rt_device_close((rt_device_t)i2c_bus);
_err_open_i2c_bus:
_err_find_i2c_bus:
_err_alloc_dev:
_err_invalid_cfg:

    return NULL;
}

static int drv_touch_destroy_device(struct drv_touch_dev* dev)
{
    if (NULL == dev) {
        return -1;
    }

    rt_device_unregister(&dev->dev.touch.parent);

    rt_device_close((rt_device_t)dev->i2c.bus);

    if (0x00 == dev->pin.fake_intr) {
        kd_pin_detach_irq(dev->pin.intr);
    }

#ifdef TOUCH_DRV_MODEL_INT_WITH_THREAD
    rt_mq_detach(&dev->thr.ctrl_mq);
    rt_mq_detach(&dev->thr.read_mq);
    rt_sem_detach(&dev->thr.ctrl_sem);
    rt_thread_detach(&dev->thr.thr);
#endif

    rt_free_align(dev);

    return 0;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
struct drv_touch_mgmt_t {
    int inited;

    struct rt_mutex mutex;

    rt_list_t dev_list;
};

static struct drv_touch_mgmt_t drv_touch_mgmt = { .inited = 0 };

static int drv_touch_mgmt_init(void)
{
    if (drv_touch_mgmt.inited) {
        return 0;
    }

    rt_list_init(&drv_touch_mgmt.dev_list);

    if (RT_EOK != rt_mutex_init(&drv_touch_mgmt.mutex, "touch_mgmt", RT_IPC_FLAG_FIFO)) {
        return -1;
    }

    drv_touch_mgmt.inited = 1;

    return 0;
}

static int drv_touch_mgmt_add_device(struct drv_touch_dev* dev)
{
    if (!dev) {
        LOG_E("Invalid dev");
        return -1;
    }

    if (!drv_touch_mgmt.inited) {
        LOG_E("drv touch mgmt not init");
        return -1;
    }

    rt_mutex_take_interruptible(&drv_touch_mgmt.mutex, RT_WAITING_FOREVER);

    rt_list_init(&dev->list);
    rt_list_insert_after(&drv_touch_mgmt.dev_list, &dev->list);

    rt_mutex_release(&drv_touch_mgmt.mutex);

    return 0;
}

static int drv_touch_mgmt_remove_device(struct drv_touch_dev* dev)
{
    struct drv_touch_dev *node, *temp;

    if (!dev) {
        LOG_E("Invalid dev");
        return -1;
    }

    if (!drv_touch_mgmt.inited) {
        LOG_E("drv touch mgmt not init");
        return -1;
    }

    rt_mutex_take_interruptible(&drv_touch_mgmt.mutex, RT_WAITING_FOREVER);

    rt_list_for_each_entry_safe(node, temp, &drv_touch_mgmt.dev_list, list)
    {
        if (dev->dev.dev_index == node->dev.dev_index) {
            rt_list_remove(&node->list);
            break;
        }
    }

    rt_mutex_release(&drv_touch_mgmt.mutex);

    return 0;
}

static struct drv_touch_dev* drv_touch_mgmt_find_by_index(int index)
{
    struct drv_touch_dev *node = NULL, *dev = NULL;

    if (!drv_touch_mgmt.inited) {
        LOG_E("drv touch mgmt not init");
        return NULL;
    }

    rt_mutex_take_interruptible(&drv_touch_mgmt.mutex, RT_WAITING_FOREVER);

    rt_list_for_each_entry(node, &drv_touch_mgmt.dev_list, list)
    {
        if (index == node->dev.dev_index) {
            dev = node;
            break;
        }
        node = NULL;
    }

    rt_mutex_release(&drv_touch_mgmt.mutex);

    return dev;
}

int drv_touch_mgmt_create_device(struct drv_touch_config* cfg)
{
    struct drv_touch_dev* dev = NULL;

    if (!cfg) {
        return -1;
    }

    if (0x00 == cfg->touch_dev_index) {
        LOG_E("can not add new touch0");
        return -1;
    }

    if (NULL != (dev = drv_touch_mgmt_find_by_index(cfg->touch_dev_index))) {
        LOG_E("touch device already created");
        return -1;
    }

    if (NULL == (dev = drv_touch_create_device(cfg))) {
        LOG_E("drv touch mgmt create device failed");

        return -1;
    }

    if (0x00 != drv_touch_mgmt_add_device(dev)) {
        drv_touch_destroy_device(dev);

        LOG_E("drv touch mgmt add device failed");

        return -1;
    }

    return 0;
}

int drv_touch_mgmt_delete_device(int index)
{
    int ret = 0;

    struct drv_touch_dev* dev = NULL;

    if (0x00 == index) {
        LOG_E("can not remove touch0");
        return -1;
    }

    if (NULL == (dev = drv_touch_mgmt_find_by_index(index))) {
        LOG_E("drv touch mgmt can not find device with index %d\n", index);

        return -1;
    }

    if (0x00 != drv_touch_mgmt_remove_device(dev)) {
        ret--;

        LOG_E("drv touch mgmt remove failed");
    }

    if (0x00 != drv_touch_destroy_device(dev)) {
        ret--;

        LOG_E("drv touch mgmt destroy failed");
    }

    return 0;
}

#if defined(TOUCH_DEFAULT_DEVICE)
static int drv_touch_register_default(void)
{
    const struct drv_touch_config default_touch = {
        .touch_dev_index = 0,

        .range_x = 0,
        .range_y = 0,

        .pin_intr    = TOUCH_DEV_INT_PIN,
        .intr_value  = TOUCH_DEV_INT_EDGE,
        .pin_reset   = TOUCH_DEV_RST_PIN,
        .reset_value = TOUCH_DEV_RST_PIN_VALID_VALUE,

        .i2c_bus_index = TOUCH_DEV_I2C_BUS,
        .i2c_bus_speed = TOUCH_DEV_I2C_BUS_SPEED,
    };

    struct drv_touch_dev* dev = NULL;

    if (NULL == (dev = drv_touch_create_device((struct drv_touch_config*)&default_touch))) {
        LOG_E("Register default touch failed");
        return -1;
    }

    drv_touch_mgmt_add_device(dev);

    return 0;
}
#endif

static int drv_touch_init(void)
{
    if (0x00 != drv_touch_mgmt_init()) {
        LOG_E("touch mgmt init failed");
        return -1;
    }

#if defined(TOUCH_DEFAULT_DEVICE)
    drv_touch_register_default();
#endif

    return 0;
}
INIT_COMPONENT_EXPORT(drv_touch_init);

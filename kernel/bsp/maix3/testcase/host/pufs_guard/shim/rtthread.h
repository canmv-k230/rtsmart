#pragma once
#include <stdio.h>
#include <stdint.h>
#define RT_TICK_PER_SECOND 1000
#define rt_kprintf printf
typedef uint32_t rt_tick_t;
rt_tick_t rt_tick_get(void);
static inline long rt_thread_mdelay(int ms) { (void)ms; return 0; }

/* Device model, just enough for drv_hwrng.c. */
typedef long rt_err_t;
typedef long rt_base_t;
typedef unsigned long rt_size_t;
typedef long rt_off_t;
typedef uint16_t rt_uint16_t;
#define RT_EOK 0
#define RT_EIO 8
#define RT_DEVICE_FLAG_RDWR 0x003
#define RT_DEVICE_FLAG_RDONLY 0x001
struct rt_device;
typedef struct rt_device *rt_device_t;
struct rt_device_ops {
    rt_err_t (*init)(rt_device_t dev);
    rt_err_t (*open)(rt_device_t dev, rt_uint16_t oflag);
    rt_err_t (*close)(rt_device_t dev);
    rt_size_t (*read)(rt_device_t dev, rt_off_t pos, void *buffer, rt_size_t size);
    rt_size_t (*write)(rt_device_t dev, rt_off_t pos, const void *buffer, rt_size_t size);
    rt_err_t (*control)(rt_device_t dev, int cmd, void *args);
};
struct rt_device { const struct rt_device_ops *ops; };
rt_err_t rt_device_register(rt_device_t dev, const char *name, rt_uint16_t flags);
void rt_set_errno(rt_err_t error);

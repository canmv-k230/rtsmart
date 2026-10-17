#ifndef CAMERA_TEST_RTDEF_H
#define CAMERA_TEST_RTDEF_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
typedef uint8_t rt_uint8_t;
typedef uint32_t rt_uint32_t;
typedef int32_t rt_int32_t;
typedef int rt_bool_t;
typedef int rt_err_t;
typedef size_t rt_size_t;
#define RT_NULL NULL
#define RT_TRUE 1
#define RT_FALSE 0
#define RT_EOK 0
#define rt_snprintf snprintf
#define INIT_DEVICE_EXPORT(fn)
struct rt_device { int placeholder; };
struct rt_event { int placeholder; };
struct rt_i2c_msg { int placeholder; };
struct rt_i2c_bus_device_ops { int placeholder; };
struct rt_i2c_bus_device {
    const struct rt_i2c_bus_device_ops *ops;
    int lock;
};
void *rt_ioremap(void *, rt_size_t);
void rt_iounmap(void *);
void rt_hw_interrupt_mask(int);
void rt_event_detach(struct rt_event *);
void rt_mutex_detach(int *);
rt_err_t rt_i2c_bus_device_register(struct rt_i2c_bus_device *, const char *);
#endif

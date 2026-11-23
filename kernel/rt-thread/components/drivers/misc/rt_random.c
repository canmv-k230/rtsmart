/*
 * Copyright (c) 2011-2020, Shanghai Real-Thread Electronic Technology Co.,Ltd
 *
 * Change Logs:
 * Date           Author       Notes
 * 2020-12-18     quanzhao     the first version
 */

#include "rtdef.h"
#include <errno.h>
#include <string.h>
#include <rtthread.h>

static struct rt_device random_dev;
static unsigned long seed;
static struct rt_mutex hw_rng_probe_lock;

/*
 * Both devices, and so getrandom(), read from the board's hardware RNG
 * ("hwrng") when one is registered. The LCGs below have 32 bits of state and
 * start from seed 0, so only boards without required hardware entropy may
 * fall back to them. A failed hardware read never uses the fallback.
 */
static rt_err_t hw_rng_get(rt_device_t *result)
{
    static rt_device_t hw = RT_NULL;
    static rt_bool_t warned = RT_FALSE;
    static rt_tick_t last_failure;
    rt_device_t dev = __atomic_load_n(&hw, __ATOMIC_ACQUIRE);

    if (dev != RT_NULL)
    {
        *result = dev;
        return RT_EOK;
    }

    rt_err_t ret = rt_mutex_take(&hw_rng_probe_lock, RT_WAITING_FOREVER);
    if (ret != RT_EOK)
        return ret;

    dev = __atomic_load_n(&hw, __ATOMIC_RELAXED);
    /* A missing or unopenable device can be retried after one second. */
    if (dev == RT_NULL &&
        (!warned || (rt_tick_t)(rt_tick_get() - last_failure) >= RT_TICK_PER_SECOND))
    {
        dev = rt_device_find("hwrng");

        if (dev != RT_NULL && rt_device_open(dev, RT_DEVICE_OFLAG_RDONLY) == RT_EOK)
        {
            __atomic_store_n(&hw, dev, __ATOMIC_RELEASE);
        }
        else
        {
            dev = RT_NULL;
            last_failure = rt_tick_get();
            if (!warned)
            {
                warned = RT_TRUE;
#ifdef RT_RANDOM_REQUIRE_HWRNG
                rt_kprintf("random: no hardware RNG; random reads will fail\n");
#else
                rt_kprintf("random: no hardware RNG; /dev/random and /dev/urandom are predictable\n");
#endif
            }
        }
    }
    rt_mutex_release(&hw_rng_probe_lock);
    *result = dev;
    return RT_EOK;
}

static rt_size_t random_read_common(void *buffer, rt_size_t size, unsigned long *state)
{
    rt_device_t hw;

    if (size == 0)
        return 0;
    if (hw_rng_get(&hw) != RT_EOK)
        return (rt_size_t)-EIO;
    if (hw != RT_NULL)
    {
        rt_size_t count = rt_device_read(hw, 0, buffer, size);
        return count == 0 || count > size ? (rt_size_t)-EIO : count;
    }

#ifdef RT_RANDOM_REQUIRE_HWRNG
    (void)state;
    return (rt_size_t)-EIO;
#else
    rt_uint8_t *data = buffer;
    rt_size_t offset = 0;

    while (offset < size)
    {
        *state = 214013L * *state + 2531011L;
        rt_uint16_t value = (*state >> 16) & 0x7FFF;
        rt_size_t copy_size = size - offset;

        if (copy_size > sizeof(value))
            copy_size = sizeof(value);
        rt_memcpy(data + offset, &value, copy_size);
        offset += copy_size;
    }
    return size;
#endif
}

static rt_size_t random_read(rt_device_t dev, rt_off_t pos, void *buffer, rt_size_t size)
{
    return random_read_common(buffer, size, &seed);
}

static rt_size_t random_write(rt_device_t dev, rt_off_t pos, const void *buffer, rt_size_t size)
{
    rt_size_t copy_size = size;

    if (copy_size > sizeof(seed))
    {
        copy_size = sizeof(seed);
    }
    if (copy_size > 0)
    {
        rt_memcpy(&seed, buffer, copy_size);
    }

    return size;
}

static rt_err_t  random_control(rt_device_t dev, int cmd, void *args)
{
    return RT_EOK;
}

#ifdef RT_USING_DEVICE_OPS
const static struct rt_device_ops random_ops =
{
    RT_NULL,
    RT_NULL,
    RT_NULL,
    random_read,
    random_write,
    random_control
};
#endif

static void random_device_init(void)
{
    RT_ASSERT(!rt_device_find("random"));
    random_dev.type    = RT_Device_Class_Miscellaneous;

#ifdef RT_USING_DEVICE_OPS
    random_dev.ops     = &random_ops;
#else
    random_dev.init    = RT_NULL;
    random_dev.open    = RT_NULL;
    random_dev.close   = RT_NULL;
    random_dev.read    = random_read;
    random_dev.write   = random_write;
    random_dev.control = random_control;
#endif

    /* no private */
    random_dev.user_data = RT_NULL;

    rt_device_register(&random_dev, "random", RT_DEVICE_FLAG_RDWR);
}

static struct rt_device urandom_dev;
static unsigned long useed;

static rt_size_t random_uread(rt_device_t dev, rt_off_t pos, void *buffer, rt_size_t size)
{
    return random_read_common(buffer, size, &useed);
}

static rt_size_t random_uwrite(rt_device_t dev, rt_off_t pos, const void *buffer, rt_size_t size)
{
    rt_size_t copy_size = size;

    if (copy_size > sizeof(useed))
    {
        copy_size = sizeof(useed);
    }
    if (copy_size > 0)
    {
        rt_memcpy(&useed, buffer, copy_size);
    }

    return size;
}

static rt_err_t random_ucontrol(rt_device_t dev, int cmd, void *args)
{
    return RT_EOK;
}

#ifdef RT_USING_DEVICE_OPS
const static struct rt_device_ops urandom_ops =
{
    RT_NULL,
    RT_NULL,
    RT_NULL,
    random_uread,
    random_uwrite,
    random_ucontrol
};
#endif

static void urandom_device_init(void)
{
    RT_ASSERT(!rt_device_find("urandom"));
    urandom_dev.type    = RT_Device_Class_Miscellaneous;

#ifdef RT_USING_DEVICE_OPS
    urandom_dev.ops     = &urandom_ops;
#else
    urandom_dev.init    = RT_NULL;
    urandom_dev.open    = RT_NULL;
    urandom_dev.close   = RT_NULL;
    urandom_dev.read    = random_uread;
    urandom_dev.write   = random_uwrite;
    urandom_dev.control = random_ucontrol;
#endif

    /* no private */
    urandom_dev.user_data = RT_NULL;

    rt_device_register(&urandom_dev, "urandom", RT_DEVICE_FLAG_RDWR);
}

static int random_devices_init(void)
{
    rt_err_t ret = rt_mutex_init(&hw_rng_probe_lock, "rngprobe", RT_IPC_FLAG_PRIO);

    if (ret != RT_EOK)
        return ret;
    random_device_init();
    urandom_device_init();
    return 0;
}
INIT_DEVICE_EXPORT(random_devices_init);

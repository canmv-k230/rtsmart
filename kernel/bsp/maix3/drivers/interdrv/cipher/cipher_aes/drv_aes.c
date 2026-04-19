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

#include <rtthread.h>
#include "drv_aes.h"
#include <rtdbg.h>

#define DBG_TAG "AES"
#ifdef RT_DEBUG
#define DBG_LVL DBG_LOG
#else
#define DBG_LVL DBG_WARNING
#endif
#define DBG_COLOR

/*
 * Legacy /dev/aes stub — all crypto now goes through /dev/pufs.
 * This device is kept for backward compatibility; all operations
 * return -ENOSYS.
 */

static rt_err_t aes_control(rt_device_t dev, int cmd, void* args)
{
    (void)dev; (void)cmd; (void)args;
    return -RT_ENOSYS;
}

static rt_err_t aes_open(rt_device_t dev, rt_uint16_t oflag)
{
    rt_kprintf("WARNING: /dev/aes is deprecated, use /dev/pufs instead\n");
    return RT_EOK;
}

static rt_err_t aes_close(rt_device_t dev)
{
    return RT_EOK;
}

const static struct rt_device_ops aes_ops = {
    RT_NULL,
    aes_open,
    aes_close,
    RT_NULL,
    RT_NULL,
    aes_control,
};

int rt_hw_aes_device_init(void)
{
    static struct rt_device aes_dev;

    if (RT_EOK != rt_device_register(&aes_dev, "aes", RT_DEVICE_FLAG_RDWR)) {
        LOG_E("hwaes device register fail!\n");
        return -RT_ERROR;
    }

    aes_dev.ops = &aes_ops;

    return 0;
}
INIT_DEVICE_EXPORT(rt_hw_aes_device_init);

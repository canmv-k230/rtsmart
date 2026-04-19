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
#include <rtdbg.h>

#define DBG_TAG "OTP"
#ifdef RT_DEBUG
#define DBG_LVL DBG_LOG
#else
#define DBG_LVL DBG_WARNING
#endif
#define DBG_COLOR

/*
 * Legacy /dev/otp stub — all OTP access now goes through /dev/pufs.
 * This device is kept for backward compatibility; all operations
 * return -ENOSYS.
 */

static rt_err_t otp_open(rt_device_t dev, rt_uint16_t oflag)
{
    rt_kprintf("WARNING: /dev/otp is deprecated, use /dev/pufs instead\n");
    return RT_EOK;
}

static rt_err_t otp_close(rt_device_t dev)
{
    return RT_EOK;
}

static rt_size_t otp_read(rt_device_t dev, rt_off_t pos, void *buffer, rt_size_t size)
{
    (void)dev; (void)pos; (void)buffer; (void)size;
    return -RT_ENOSYS;
}

static rt_size_t otp_write(rt_device_t dev, rt_off_t pos, const void *buffer, rt_size_t size)
{
    (void)dev; (void)pos; (void)buffer; (void)size;
    return -RT_ENOSYS;
}

const static struct rt_device_ops otp_ops = {
    RT_NULL,
    otp_open,
    otp_close,
    otp_read,
    otp_write,
    RT_NULL,
};

int rt_hw_otp_init(void)
{
    static struct rt_device otp_dev;

    if (RT_EOK != rt_device_register(&otp_dev, "otp", RT_DEVICE_FLAG_RDWR)) {
        LOG_E("otp device register fail!\n");
        return -RT_ERROR;
    }

    otp_dev.ops = &otp_ops;
    return 0;
}
INIT_DEVICE_EXPORT(rt_hw_otp_init);

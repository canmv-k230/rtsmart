/* Copyright (c) 2023, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * /dev/hwrng: the PUFrt random number register.
 *
 * This is the system's entropy source: rt_random.c backs /dev/random,
 * /dev/urandom and getrandom() with it, and the WLAN supplicant reads it for
 * WPA nonces. The /dev/pufs RNG ioctl uses the same pufs_rng_fill().
 *
 * Reads do not take the PUFS hardware lock: they only read the RN register, which is
 * readable whenever the RT PMP window is (read-only by default).
 */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <rtthread.h>
#include <rtdevice.h>
#include <rthw.h>

#include "rvv_ops.h"
#include "sbi.h"
#include "drv_pufs_internal.h"
#include "pufs_internal.h"
#include "pufs_rt_internal.h"

/*
 * Discard the startup sample and wait for a fresh word before returning any
 * bytes. Remember the last word across reads, including one-byte reads.
 */
#define RNG_RETRY_LIMIT 8

static uint32_t rng_last;
static bool rng_ready, rng_stuck;
#ifdef RT_USING_SMP
static struct rt_spinlock rng_health_lock;
#endif
static struct rt_device hwrng_dev;

static bool rng_next_word(uint32_t *word)
{
    rt_base_t level = rt_spin_lock_irqsave(&rng_health_lock);

    for (unsigned int i = 0; i < RNG_RETRY_LIMIT; i++) {
        uint32_t sample = rt_regs->rn;

        if (sample != rng_last) {
            rng_last = sample;
            rng_stuck = false;
            *word = sample;
            rt_spin_unlock_irqrestore(&rng_health_lock, level);
            return true;
        }
    }

    bool report = !rng_stuck;
    rng_stuck = true;
    rt_spin_unlock_irqrestore(&rng_health_lock, level);
    if (report)
        LOG_ERROR("RNG output stuck, reads refused");
    return false;
}

int pufs_rng_fill(uint8_t *buf, size_t len)
{
    if (len != 0 && !__atomic_load_n(&rng_ready, __ATOMIC_ACQUIRE))
        return -EIO;

    for (size_t off = 0; off < len; ) {
        uint32_t word;
        size_t n = (len - off < sizeof(word)) ? len - off : sizeof(word);

        if (!rng_next_word(&word)) {
            rvv_memset(buf, 0, len);
            return -EIO;
        }
        memcpy(buf + off, &word, n);
        off += n;
    }
    return 0;
}

static rt_size_t hwrng_read(rt_device_t dev, rt_off_t pos, void *buffer, rt_size_t size)
{
    (void)dev;
    (void)pos;

    /* devfs interprets negative device read results as POSIX errors. */
    return pufs_rng_fill(buffer, size) == 0 ? size : (rt_size_t)-EIO;
}

static const struct rt_device_ops hwrng_ops = {
    .read = hwrng_read,
};

/*
 * Called by pufs_device_init() once the PUFS block is mapped and the RT module
 * is initialized. Takes the PUFS lock itself (for the RT window).
 */
int pufs_hwrng_init(void)
{
    rt_err_t ret;

    /* The boot ROM leaves the RNG clock on today; do not depend on it. */
    if (pufs_drv_hw_lock() != 0)
        goto clock_error;
    pufs_status_t status = pufs_recover_pmp();
    if (status == SUCCESS && !rt_check_rngclk_enable()) {
        status = pufs_rt_pmp_set(SBI_PMP_PERM_R | SBI_PMP_PERM_W);
        if (status == SUCCESS) {
            rt_write_rngclk(true);
            status = pufs_rt_pmp_set(SBI_PMP_PERM_R);
        }
    }
    pufs_drv_hw_unlock();
    if (status != SUCCESS || !rt_check_rngclk_enable())
        goto clock_error;

    rt_spin_lock_init(&rng_health_lock);
    /* Use the startup sample only as a baseline. Reads check for a fresh
     * word, so a temporarily stalled register does not prevent registration. */
    rng_last = rt_regs->rn;

    __atomic_store_n(&rng_ready, true, __ATOMIC_RELEASE);
    hwrng_dev.ops = &hwrng_ops;
    ret = rt_device_register(&hwrng_dev, "hwrng", RT_DEVICE_FLAG_RDONLY);
    if (ret != RT_EOK)
        LOG_WARN("failed to register hwrng device: %d", (int)ret);
    return ret;

clock_error:
    LOG_ERROR("PUFS RNG clock initialization failed");
    return -RT_EIO;
}

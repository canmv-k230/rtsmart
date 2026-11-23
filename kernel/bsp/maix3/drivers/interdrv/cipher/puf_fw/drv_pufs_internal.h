#ifndef __DRV_PUFS_INTERNAL_H__
#define __DRV_PUFS_INTERNAL_H__

#include <stddef.h>
#include <stdint.h>

/* 0, -RT_ENOSYS (driver init failed), -RT_ETIMEOUT (lock busy for 1 s) or
 * -RT_EIO (OTP re-mask failed). */
int pufs_drv_hw_lock(void);
void pufs_drv_hw_unlock(void);

/* drv_hwrng.c: /dev/hwrng and the RNG read shared with the /dev/pufs ioctl. */
int pufs_rng_fill(uint8_t *buf, size_t len);   /* 0, or -EIO if the RNG is stuck */
int pufs_hwrng_init(void);

#endif

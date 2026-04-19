#include "rvv_ops.h"

#include "rtthread.h"
#include "encoding.h"

#include <stdint.h>
#include <string.h>

static inline int rvv_available(void)
{
    size_t sstatus;
    asm volatile("csrr %0, sstatus" : "=r"(sstatus));
    return (sstatus & SSTATUS_VS) != 0;
}

void *rvv_memcpy(void *dst, const void *src, size_t n)
{
    const uint8_t *s = (const uint8_t *)src;
    uint8_t *d = (uint8_t *)dst;
    size_t remaining = n;

    if ((8 > n) || !rvv_available()) {
        return rt_memcpy(dst, src, n);
    }

    while (remaining > 0) {
        size_t vl;

        asm volatile(
            ".option push\n"
            ".option arch, +v\n"
            "vsetvli %0, %1, e8, m8, ta, ma\n"
            "vle8.v v8, (%2)\n"
            "vse8.v v8, (%3)\n"
            ".option pop\n"
            : "=&r"(vl)
            : "r"(remaining), "r"(s), "r"(d)
            : "v8", "memory");

        s += vl;
        d += vl;
        remaining -= vl;
    }

    return dst;
}

void *rvv_memset(void *dst, int value, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    size_t remaining = n;
    uintptr_t fill = (uint8_t)value;

    if ((8 > n) || !rvv_available()) {
        return rt_memset(dst, value, n);
    }

    while (remaining > 0) {
        size_t vl;

        asm volatile(
            ".option push\n"
            ".option arch, +v\n"
            "vsetvli %0, %1, e8, m8, ta, ma\n"
            "vmv.v.x v8, %2\n"
            "vse8.v v8, (%3)\n"
            ".option pop\n"
            : "=&r"(vl)
            : "r"(remaining), "r"(fill), "r"(d)
            : "v8", "memory");

        d += vl;
        remaining -= vl;
    }

    return dst;
}

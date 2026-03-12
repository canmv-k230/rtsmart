#ifndef RVV_OPS_H__
#define RVV_OPS_H__

#include <stddef.h>

void *rvv_memcpy(void *dst, const void *src, size_t n);
void *rvv_memset(void *dst, int value, size_t n);

#endif /* RVV_OPS_H__ */

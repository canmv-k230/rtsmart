#pragma once
#include <stddef.h>
void rt_hw_cpu_dcache_invalidate(void *addr, size_t len);
void rt_hw_cpu_dcache_clean(void *addr, size_t len);

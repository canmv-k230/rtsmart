#pragma once
#include <stdint.h>
#include <stddef.h>
#define SBI_SUCCESS 0
#define SBI_ERR_DENIED (-4)
#define SBI_PMP_PERM_R 1
#define SBI_PMP_PERM_W 2
struct sbi_ret { long error, value; };
struct sbi_ret sbi_pmp_set(uintptr_t addr, size_t len, unsigned long perm);

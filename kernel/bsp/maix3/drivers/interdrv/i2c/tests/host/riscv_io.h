#include <stdint.h>
#define writel(value, address) (*(address) = (uint32_t)(value))

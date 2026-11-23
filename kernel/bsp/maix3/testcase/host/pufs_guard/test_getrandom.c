/* Exercise the syscall itself in both RT-Smart memory models. */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define GRND_RANDOM 2
#define RT_EOK 0
#define RT_DEVICE_OFLAG_RDONLY 1
#define ARCH_PAGE_SIZE 16
typedef int rt_device_t;
typedef size_t rt_size_t;
static int available, opens, reads;
static size_t reply[4];
static rt_device_t rt_device_find(const char *name) { return available; }
static int rt_device_open(rt_device_t dev, int flags) { opens++; return RT_EOK; }
static void rt_device_close(rt_device_t dev) { opens--; }
static size_t rt_device_read(rt_device_t dev, size_t pos, void *dst, size_t len)
{
    assert(reads < 4);
    size_t n = reply[reads++];
    if (n <= len)
        memset(dst, 0x57, n);
    return n;
}
#ifdef RT_USING_USERSPACE
static char scratch[ARCH_PAGE_SIZE];
static void *kmem_get(size_t n) { assert(n <= sizeof(scratch)); return scratch; }
static void kmem_put(void *p)
{
    assert(p == scratch);
    for (size_t i = 0; i < sizeof(scratch); i++)
        assert(scratch[i] == 0); /* returned bytes are scrubbed */
}
#define rt_memset memset
static int lwp_user_accessable(void *p, size_t n) { return p != NULL; }
static size_t lwp_put_to_user(void *dst, void *src, size_t n) { memcpy(dst, src, n); return n; }
#endif
#include "getrandom.inc"

int main(void)
{
    unsigned char buf[32] = {0};
    assert(sys_getrandom(NULL, 0, 0) == 0);
    assert(sys_getrandom(buf, 1, 0) == -EIO);
    available = 1;
    assert(sys_getrandom(buf, 12, 0) == -EIO && opens == 0);
    reads = 0; reply[0] = (size_t)-EIO;
    assert(sys_getrandom(buf, 12, 0) == -EIO && opens == 0);
    reads = 0; reply[0] = 13;
    assert(sys_getrandom(buf, 12, 0) == -EIO && !buf[0]);
    reads = 0; reply[0] = 3; reply[1] = (size_t)-EIO;
    assert(sys_getrandom(buf, 12, GRND_RANDOM) == 3 && buf[2] == 0x57 && !buf[3]);
    reads = 0; reply[0] = 16; reply[1] = 2; reply[2] = 14;
    assert(sys_getrandom(buf, sizeof(buf), 0) == sizeof(buf) && buf[31] == 0x57 && !opens);
    puts("getrandom: absent provider, unsigned errors, short and zero reads: PASS");
    return 0;
}

#ifndef PDMA_HOST_SHIM_H
#define PDMA_HOST_SHIM_H
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t rt_uint8_t;
typedef uint32_t rt_uint32_t;
typedef uint64_t rt_uint64_t;
typedef int32_t rt_int32_t;
typedef uintptr_t rt_base_t;
typedef uint32_t rt_tick_t;
typedef int rt_bool_t;
typedef int rt_err_t;
struct rt_mutex { int held; };
struct rt_spinlock { int held; };
typedef struct rt_spinlock rt_spinlock_t;
struct rt_event { uint32_t bits; };
struct rt_completion { int done; };
#define RT_NULL NULL
#define RT_TRUE 1
#define RT_FALSE 0
#define RT_UINT32_MAX UINT32_MAX
#define RT_ALIGN_SIZE 8
#define RT_CPU_CACHE_LINE_SZ 64
#define ALIGN(x) __attribute__((aligned(x)))
#define RT_ASSERT(x) assert(x)
#define RT_EOK 0
#define RT_ERROR 1
#define RT_ETIMEOUT 110
#define RT_EINVAL 22
#define RT_EBUSY 16
#define RT_ENOMEM 12
#define RT_WAITING_FOREVER (-1)
#define RT_IPC_FLAG_PRIO 0
#define RT_EVENT_FLAG_OR 1
#define RT_EVENT_FLAG_CLEAR 2
#define DBG_LOG 1
#define DBG_WARNING 2
#define LOG_D(...) ((void)0)
#define LOG_W(...) ((void)0)
#define LOG_E(...) ((void)0)
#define INIT_PREV_EXPORT(x)

static int host_irq_depth;
static int host_allocations;
static int host_frees;
static int host_masks[256];
static rt_tick_t host_ticks;
static void (*host_event_hook)(void);
static void (*host_completion_hook)(void);

static inline char* rt_strdup(const char* s)
{
    assert(host_irq_depth == 0);
    host_allocations++;
    size_t len = strlen(s) + 1;
    char* copy = malloc(len);
    assert(copy);
    memcpy(copy, s, len);
    return copy;
}
static inline void rt_free(void* p)
{
    assert(host_irq_depth == 0);
    if (p) host_frees++;
    free(p);
}
static inline rt_err_t rt_mutex_init(struct rt_mutex* m, const char* n, int f)
{ (void)n; (void)f; m->held = 0; return RT_EOK; }
static inline rt_err_t rt_mutex_take(struct rt_mutex* m, rt_int32_t t)
{ (void)t; assert(!m->held); m->held = 1; return RT_EOK; }
static inline void rt_mutex_release(struct rt_mutex* m)
{ assert(m->held); m->held = 0; }
static inline void rt_mutex_detach(struct rt_mutex* m) { assert(!m->held); }
static inline void rt_spin_lock_init(struct rt_spinlock* l) { l->held = 0; }
static inline rt_base_t rt_spin_lock_irqsave(struct rt_spinlock* l)
{ assert(!l->held); l->held = 1; return 0; }
static inline void rt_spin_unlock_irqrestore(struct rt_spinlock* l, rt_base_t b)
{ (void)b; assert(l->held); l->held = 0; }
static inline rt_err_t rt_event_init(struct rt_event* e, const char* n, int f)
{ (void)n; (void)f; e->bits = 0; return RT_EOK; }
static inline rt_err_t rt_event_recv(struct rt_event* e, rt_uint32_t mask,
                                    int flags, rt_int32_t timeout,
                                    rt_uint32_t* result)
{
    if (!(e->bits & mask) && timeout != 0 && host_event_hook) {
        void (*hook)(void) = host_event_hook;
        host_event_hook = NULL;
        hook();
    }
    rt_uint32_t bits = e->bits & mask;
    if (!bits) {
        if (timeout > 0) host_ticks += (rt_tick_t)timeout;
        return -RT_ETIMEOUT;
    }
    if (result) *result = bits;
    if (flags & RT_EVENT_FLAG_CLEAR) e->bits &= ~bits;
    return RT_EOK;
}
static inline void rt_event_send(struct rt_event* e, rt_uint32_t bits)
{ e->bits |= bits; }
static inline rt_tick_t rt_tick_get(void) { return host_ticks; }
static inline rt_tick_t rt_tick_from_millisecond(int v) { return (rt_tick_t)v; }
static inline int rt_interrupt_get_nest(void) { return host_irq_depth; }
static inline void rt_hw_interrupt_mask(int irq) { assert(irq < 256); host_masks[irq] = 1; }
static inline void rt_hw_interrupt_umask(int irq) { assert(irq < 256); host_masks[irq] = 0; }
static inline void rt_completion_init(struct rt_completion* c) { c->done = 0; }
static inline void rt_completion_done(struct rt_completion* c) { c->done = 1; }
static inline void rt_completion_wait(struct rt_completion* c, int timeout)
{
    (void)timeout;
    if (!c->done && host_completion_hook) {
        void (*hook)(void) = host_completion_hook;
        host_completion_hook = NULL;
        hook();
    }
    assert(c->done);
}
static inline void rt_hw_cpu_dcache_clean(void* p, size_t n) { (void)p; (void)n; }
static inline void* rt_ioremap(void* a, size_t n) { (void)a; return calloc(1, n); }
static inline void rt_iounmap(void* a) { free(a); }
static inline void rt_hw_interrupt_install(int irq, void (*f)(int, void*), void* p, const char* n)
{ (void)irq; (void)f; (void)p; (void)n; }

/* Compile the existing built-in tests too, but abort any attempt to use their
 * UART path on the host. Synthetic address constants are never dereferenced. */
#ifdef RT_PDMA_ENABLE_BUILTIN_TESTS
typedef size_t rt_size_t;
typedef void* rt_device_t;
struct rt_semaphore { int unused; };
typedef struct rt_semaphore* rt_sem_t;
#define RT_ENOSYS 38
#define RT_DEVICE_FLAG_RDWR 3
#define UART2_BASE_ADDR ((uintptr_t)0)
#define UART3_BASE_ADDR ((uintptr_t)0)
#define MSH_CMD_EXPORT(fn, desc) \
    static __typeof__(&(fn)) const host_export_##fn __attribute__((unused)) = fn
#define rt_kprintf(...) ((void)0)
#define rt_memset memset
static inline void* rt_malloc(size_t n) { (void)n; abort(); }
static inline void* rt_malloc_align(size_t n, size_t a) { (void)n; (void)a; abort(); }
static inline void rt_free_align(void* p) { (void)p; abort(); }
static inline rt_device_t rt_device_find(const char* n) { (void)n; abort(); }
static inline int rt_device_open(rt_device_t d, int f) { (void)d; (void)f; abort(); }
static inline int rt_device_close(rt_device_t d) { (void)d; abort(); }
static inline int rt_sem_init(rt_sem_t s, const char* n, unsigned v, int f)
{ (void)s; (void)n; (void)v; (void)f; abort(); }
static inline int rt_sem_take(rt_sem_t s, rt_tick_t t) { (void)s; (void)t; abort(); }
static inline void rt_sem_release(rt_sem_t s) { (void)s; abort(); }
static inline void rt_sem_detach(rt_sem_t s) { (void)s; abort(); }
static inline void rt_thread_mdelay(int t) { (void)t; abort(); }
static inline void rt_hw_cpu_dcache_invalidate(void* p, size_t n) { (void)p; (void)n; abort(); }
static inline void writel(uint32_t v, volatile void* p) { (void)v; (void)p; abort(); }
#endif
#endif

/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2020-11-18     Jesven       first version
 * 2021-02-03     lizhirui     port to riscv64
 * 2021-02-06     lizhirui     add thread filter
 * 2021-02-19     lizhirui     port to new version of rt-smart
 * 2021-03-02     lizhirui     add a auxillary function for interrupt
 * 2021-03-04     lizhirui     delete thread filter
 * 2021-03-04     lizhirui     modify for new version of rt-smart
 * 2021-11-22     JasonHu      add lwp_set_thread_context
 * 2021-11-30     JasonHu      add clone/fork support
 */

#include <rthw.h>
#include <stddef.h>

#ifdef RT_USING_USERSPACE

#include <mmu.h>
#include <page.h>
#include <lwp_mm_area.h>
#include <lwp_mm.h>
#include <lwp_user_mm.h>
#include <lwp_arch.h>
#include <lwp_signal.h>
#include <lwp_syscall.h>

#define DBG_TAG "LWP.arch"
#define DBG_LVL DBG_WARNING
#include <rtdbg.h>

#include <stack.h>
#include <cpuport.h>
#include <encoding.h>

extern rt_ubase_t MMUTable[];

int arch_expand_user_stack(void *addr)
{
    int ret = 0;
    rt_ubase_t stack_addr = (rt_ubase_t)addr;

    stack_addr &= ~PAGE_OFFSET_MASK;
    if ((stack_addr >= (rt_ubase_t)USER_STACK_VSTART) && (stack_addr < (rt_ubase_t)USER_STACK_VEND))
    {
        void *map = lwp_map_user(lwp_self(), (void *)stack_addr, PAGE_SIZE, RT_FALSE);

        if (map || lwp_user_accessable(addr, 1))
        {
            ret = 1;
        }
    }
    return ret;
}

void *lwp_copy_return_code_to_user_stack()
{
    void lwp_thread_return();
    void lwp_thread_return_end();
    rt_thread_t tid = rt_thread_self();

    if (tid->user_stack != RT_NULL)
    {
        rt_size_t size = (rt_size_t)lwp_thread_return_end - (rt_size_t)lwp_thread_return;
        rt_size_t userstack = (rt_size_t)tid->user_stack + tid->user_stack_size - size;
        rt_memcpy((void *)userstack, lwp_thread_return, size);
        return (void *)userstack;
    }

    return RT_NULL;
}

rt_mmu_info *arch_kernel_get_mmu_info(void)
{
    extern rt_mmu_info *mmu_info;

    return mmu_info;
}

rt_ubase_t lwp_fix_sp(rt_ubase_t cursp)
{
    void lwp_thread_return();
    void lwp_thread_return_end();

    if (cursp == 0)
    {
        return 0;
    }

    return cursp - ((rt_size_t)lwp_thread_return_end - (rt_size_t)lwp_thread_return);
}

rt_thread_t rt_thread_sp_to_thread(void *spmember_addr)
{
    return (rt_thread_t)(((rt_ubase_t)spmember_addr) - (offsetof(struct rt_thread, sp)));
}

void *get_thread_kernel_stack_top(rt_thread_t thread)
{
    return (void *)(((rt_size_t)thread->stack_addr) + ((rt_size_t)thread->stack_size));
}

void *arch_get_user_sp(void)
{
    /* user sp saved in interrupt context */
    rt_thread_t self = rt_thread_self();
    rt_uint8_t *stack_top = (rt_uint8_t *)self->stack_addr + self->stack_size;
    struct rt_hw_stack_frame *frame = (struct rt_hw_stack_frame *)(stack_top - sizeof(struct rt_hw_stack_frame));

    return (void *)frame->user_sp_exc_stack;
}

int arch_user_space_init(struct rt_lwp *lwp)
{
    rt_ubase_t *mmu_table;

    mmu_table = (rt_ubase_t *)rt_pages_alloc(0);
    if (!mmu_table)
    {
        return -1;
    }

    lwp->end_heap = USER_HEAP_VADDR;

    rt_memcpy(mmu_table, MMUTable, PAGE_SIZE);
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, mmu_table, 4 * PAGE_SIZE);
    rt_hw_mmu_map_init(&lwp->mmu_info, (void *)USER_VADDR_START, USER_VADDR_TOP - USER_VADDR_START, (rt_size_t *)mmu_table, 0);

    return 0;
}

void *arch_kernel_mmu_table_get(void)
{
    return (void *)((char *)MMUTable);
}

void arch_user_space_vtable_free(struct rt_lwp *lwp)
{
    if (lwp && lwp->mmu_info.vtable)
    {
        rt_pages_free(lwp->mmu_info.vtable, 0);
    }
}

long _sys_clone(void *arg[]);
long sys_clone(void *arg[])
{
    return _sys_clone(arg);
}

long _sys_fork(void);
long sys_fork(void)
{
    return _sys_fork();
}

long _sys_vfork(void);
long sys_vfork(void)
{
    return _sys_fork();
}

/**
 * set exec context for fork/clone.
 */
int arch_set_thread_context(void (*exit)(void), void *new_thread_stack,
                            void *user_stack, void **thread_sp)
{
    RT_ASSERT(exit != RT_NULL);
    RT_ASSERT(user_stack != RT_NULL);
    RT_ASSERT(new_thread_stack != RT_NULL);
    RT_ASSERT(thread_sp != RT_NULL);
    struct rt_hw_stack_frame *syscall_frame;
    struct rt_hw_stack_frame *thread_frame;

    rt_uint8_t *stk;
    rt_uint8_t *syscall_stk;

    stk = (rt_uint8_t *)new_thread_stack;
    /* reserve syscall context, all the registers are copyed from parent */
    stk -= CTX_REG_NR * REGBYTES;
    syscall_stk = stk;

    syscall_frame = (struct rt_hw_stack_frame *)stk;

    /* modify user sp */
    syscall_frame->user_sp_exc_stack = (rt_ubase_t)user_stack;

    /* skip ecall */
    syscall_frame->epc += 4;

    /* child return value is 0 */
    syscall_frame->a0 = 0;
    syscall_frame->a1 = 0;

    /* reset thread area */
    rt_thread_t thread = rt_container_of((unsigned long)thread_sp, struct rt_thread, sp);
    syscall_frame->tp = (rt_ubase_t)thread->thread_idr;

#ifdef ARCH_USING_NEW_CTX_SWITCH
    extern void *_rt_hw_stack_init(rt_ubase_t *sp, rt_ubase_t ra, rt_ubase_t sstatus);
    rt_ubase_t sstatus = read_csr(sstatus) | SSTATUS_SPP;
    sstatus &= ~SSTATUS_SIE;

    /* compatible to RESTORE_CONTEXT */
    stk = (void *)_rt_hw_stack_init((rt_ubase_t *)stk, (rt_ubase_t)exit, sstatus);
#else
    /* build temp thread context */
    stk -= sizeof(struct rt_hw_stack_frame);

    thread_frame = (struct rt_hw_stack_frame *)stk;

    int i;
    for (i = 0; i < sizeof(struct rt_hw_stack_frame) / sizeof(rt_ubase_t); i++)
    {
        ((rt_ubase_t *)thread_frame)[i] = 0xdeadbeaf;
    }

    /* set pc for thread */
    thread_frame->epc     = (rt_ubase_t)exit;

    /* set old exception mode as supervisor, because in kernel */
    thread_frame->sstatus = read_csr(sstatus) | SSTATUS_SPP;
    thread_frame->sstatus &= ~SSTATUS_SIE; /* must disable interrupt */

    /* set stack as syscall stack */
    thread_frame->user_sp_exc_stack = (rt_ubase_t)syscall_stk;

#endif /* ARCH_USING_NEW_CTX_SWITCH */
    /* save new stack top */
    *thread_sp = (void *)stk;

    /**
     * The stack for child thread:
     *
     * +------------------------+ --> kernel stack top
     * | syscall stack          |
     * |                        |
     * | @sp                    | --> `user_stack`
     * | @epc                   | --> user ecall addr + 4 (skip ecall)
     * | @a0&a1                 | --> 0 (for child return 0)
     * |                        |
     * +------------------------+ --> temp thread stack top
     * | temp thread stack      |           ^
     * |                        |           |
     * | @sp                    | ---------/
     * | @epc                   | --> `exit` (arch_clone_exit/arch_fork_exit)
     * |                        |
     * +------------------------+ --> thread sp
     */
}

/**
 * void lwp_exec_user(void *args, void *kernel_stack, void *user_entry)
 */
void lwp_exec_user(void *args, void *kernel_stack, void *user_entry)
{
    arch_start_umode(args, user_entry, (void*)USER_STACK_VEND, kernel_stack);
}

void *arch_get_usp_from_uctx(struct rt_user_context *uctx)
{
    return uctx->sp;
}

extern void rt_hw_sync_cache_local(void *addr, int size);
extern char lwp_sigreturn[];
extern char lwp_sigreturn_end[];

static void __attribute__((noreturn))
signal_kill_bad_context(const char *reason, rt_ubase_t addr)
{
    rt_thread_t thread = rt_thread_self();

    LOG_E("User Fault, killing thread: %s, tid: %d (%s: 0x%lx)",
          thread->name, thread->tid, reason, (unsigned long)addr);
    lwp_terminate_with_status(lwp_self(), LWP_WAIT_STATUS_SIGNAL(SIGSEGV));
    sys_exit(-1);
    for (;;)
        ;
}

/**
 * Prepare a user signal handler frame without ever switching kernel sp to
 * user space. Validates that the user stack range is in user space and
 * writable (demand-paging the main stack region if needed), copies the saved
 * context, trampoline, and siginfo via lwp_put_to_user(), and saves the
 * trusted sstatus in kernel memory (uctx->flag).
 */
void arch_signal_prepare(struct rt_hw_stack_frame *frame)
{
    rt_ubase_t user_sp = frame->user_sp_exc_stack;
    rt_ubase_t frame_addr = 0, sigret_addr = 0, siginfo_addr = 0;
    size_t sigret_size = (size_t)(lwp_sigreturn_end - lwp_sigreturn);
    size_t needed = sizeof(*frame) + sigret_size + sizeof(siginfo_t) + 32;
    siginfo_t kinfo;
    lwp_sighandler_t handler;
    rt_ubase_t page_start, page_end, va;
    int sig;

    if (user_sp >= (rt_ubase_t)USER_VADDR_START + needed &&
        user_sp <= (rt_ubase_t)USER_VADDR_TOP)
    {
        frame_addr = user_sp - sizeof(*frame);
        sigret_addr = (frame_addr - sigret_size) & ~(rt_ubase_t)7;
        siginfo_addr = (sigret_addr - sizeof(siginfo_t)) & ~(rt_ubase_t)15;
        if (siginfo_addr < (rt_ubase_t)USER_VADDR_START)
        {
            frame_addr = 0;
        }
    }

    for (;;)
    {
        sig = lwp_signal_backup((void *)frame_addr, RT_NULL,
                                (void *)frame->sstatus);
        if (sig == 0)
        {
            return;
        }
        if (sig < 0)
        {
            signal_kill_bad_context("signal nesting limit exceeded", user_sp);
        }

        rt_memset(&kinfo, 0, sizeof(kinfo));
        handler = lwp_sighandler_get(sig, &kinfo);
        if (!handler)
        {
            lwp_signal_restore();
            continue;
        }
        break;
    }

    if (!frame_addr)
    {
        signal_kill_bad_context("invalid signal stack", user_sp);
    }

    page_start = siginfo_addr & ~PAGE_OFFSET_MASK;
    page_end = (user_sp - 1) & ~PAGE_OFFSET_MASK;
    for (va = page_start; ; va += PAGE_SIZE)
    {
        if (!lwp_user_accessable((void *)va, 1) &&
            !arch_expand_user_stack((void *)va))
        {
            signal_kill_bad_context("unmapped signal stack", va);
        }
        if (va == page_end)
        {
            break;
        }
    }

#ifdef ENABLE_VECTOR
    if (!(frame->sstatus & SSTATUS_VS_CLEAN))
    {
        if (!(frame->sstatus & SSTATUS_VS))
        {
            rt_memset(frame->v, 0, sizeof(frame->v));
        }
        else
        {
            /*
             * SAVE_ALL saved vector state for VS_INITIAL, but RESTORE_ALL only
             * restores vectors when SSTATUS_VS_CLEAN is set. Promote so
             * rvv_memcpy() inside lwp_put_to_user() cannot leave dirty
             * hardware vector registers visible to user mode.
             */
            frame->sstatus = (frame->sstatus & ~SSTATUS_VS) | SSTATUS_VS_CLEAN;
        }
    }
#endif

    if (lwp_put_to_user((void *)frame_addr, frame, sizeof(*frame)) != sizeof(*frame) ||
        lwp_put_to_user((void *)sigret_addr, lwp_sigreturn, sigret_size) != sigret_size ||
        lwp_put_to_user((void *)siginfo_addr, &kinfo, sizeof(kinfo)) != sizeof(kinfo))
    {
        signal_kill_bad_context("unwritable signal stack", user_sp);
    }

    rt_hw_sync_cache_local((void *)sigret_addr, (int)sigret_size);

    frame->epc = (rt_ubase_t)handler;
    frame->ra = sigret_addr;
    frame->user_sp_exc_stack = siginfo_addr;
    frame->a0 = (rt_ubase_t)sig;
    frame->a1 = siginfo_addr;
    frame->a2 = 0;
    frame->sstatus &= ~(SSTATUS_SPP | SSTATUS_SIE);
    frame->sstatus |= SSTATUS_SPIE;
}

/**
 * Restore the pre-signal user frame from user memory onto the kernel stack
 * via lwp_get_from_user(), never trusting user-controlled sstatus or running
 * RESTORE_ALL/SAVE_ALL on user sp.
 */
void arch_signal_restore(struct rt_hw_stack_frame *frame)
{
    struct rt_user_context *uctx;
    void *user_frame_va;
    rt_ubase_t saved_sstatus;
    size_t copied;

    uctx = lwp_signal_restore();
    if (!uctx)
    {
        signal_kill_bad_context("unexpected sigreturn", 0);
    }

    user_frame_va = arch_get_usp_from_uctx(uctx);
    saved_sstatus = (rt_ubase_t)uctx->flag;

    rt_mm_lock();
    copied = lwp_get_from_user(frame, user_frame_va, sizeof(*frame));
    rt_mm_unlock();
    if (copied != sizeof(*frame))
    {
        signal_kill_bad_context("unreadable sigreturn frame",
                                (rt_ubase_t)user_frame_va);
    }

    /* Restore sstatus from the kernel-saved copy, never from user memory. */
    saved_sstatus &= ~(SSTATUS_SPP | SSTATUS_SIE);
    saved_sstatus |= SSTATUS_SPIE;
#ifdef ENABLE_VECTOR
    if ((saved_sstatus & SSTATUS_VS) == SSTATUS_VS_INITIAL)
    {
        saved_sstatus = (saved_sstatus & ~SSTATUS_VS) | SSTATUS_VS_CLEAN;
    }
#endif
    frame->sstatus = saved_sstatus;
}

#endif /* RT_USING_USERSPACE */

/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2021/1/30      lizirui      first version
 * 2021/10/20     JasonHu      move to trap.c
 */

#include <rthw.h>
#include <rtthread.h>
#define DBG_LVL DBG_LOG
#include <rtdbg.h>

#include "board.h"
#include "tick.h"

#include "drv_uart.h"
#include "encoding.h"
#include "stack.h"
#include "sbi.h"
#include "riscv.h"

#include "rt_interrupt.h"
#include "plic.h"

#ifdef RT_USING_USERSPACE
    #include "riscv_mmu.h"
    #include "mmu.h"
    #include "page.h"
    #include "lwp_arch.h"
    #include "lwp_user_mm.h"
#endif

extern rt_ubase_t __text_start[];
extern rt_ubase_t __text_end[];
extern rt_ubase_t __stack_start__[];
extern rt_ubase_t __stack_cpu0[];

/* Forward declarations for crash diagnostics. */
static const char *get_exception_msg(int id);
void dump_regs(struct rt_hw_stack_frame *regs);

/* Re-entrancy guard: this port runs on one hart, with interrupts disabled
 * on trap entry. A future SMP port needs a separate guard for each hart. */
static volatile int crash_handler_depth = 0;

/* Startup and trap_entry use CPU0's stack; the linker also reserves an unused
 * CPU1 stack. Keep classification and frame walking on the same CPU0 range. */
static rt_bool_t on_boot_stack(rt_ubase_t sp)
{
    return sp >= (rt_ubase_t)__stack_start__ && sp < (rt_ubase_t)__stack_cpu0;
}

#ifdef RT_USING_USERSPACE
static rt_ubase_t user_stack_top(rt_thread_t thread, rt_ubase_t sp)
{
    rt_ubase_t bottom, size;
    struct lwp_avl_struct *node;
    const struct rt_mm_area_struct *area;
    const struct rt_lwp *lwp;

    if (!thread || !thread->lwp || sp < USER_VADDR_START || sp >= USER_VADDR_TOP)
        return 0;

    /* sys_thread_create() records explicit stack bounds. The main-thread
     * entry clears user_stack, so it reaches the reserved-range fallback. */
    bottom = (rt_ubase_t)thread->user_stack;
    size = thread->user_stack_size;
    if (bottom && size)
    {
        if (bottom < USER_VADDR_START || bottom >= USER_VADDR_TOP ||
            size > USER_VADDR_TOP - bottom || sp < bottom || sp - bottom >= size)
            return 0;
        return bottom + size;
    }

    /* The main stack is demand-mapped a page at a time. Its reserved range
     * is a better bound than any one currently mapped page. */
    if (sp >= USER_STACK_VSTART && sp < USER_STACK_VEND)
        return USER_STACK_VEND;

    /* clone() leaves user_stack/size unset. For pthreads, use the data mapping
     * containing SP as a conservative bound; it can include TLS or other data.
     * Do not take the MM mutex in a fault handler. This single-hart port enters
     * traps with interrupts disabled, so the mapping tree cannot change here.
     * A future SMP port needs a crash-safe snapshot or synchronization scheme;
     * blocking on the MM mutex could deadlock if the faulting code holds it.
     * Every frame is still copied through lwp_get_from_user(). */
    lwp = thread->lwp;
    node = lwp_map_find(lwp->map_area, sp);
    if (!node || !node->data)
        return 0;
    area = node->data;
    bottom = area->addr;
    size = area->size;
    if (area->type != MM_AREA_TYPE_DATA || bottom < USER_VADDR_START ||
        bottom >= USER_VADDR_TOP || size > USER_VADDR_TOP - bottom ||
        sp < bottom || sp - bottom >= size)
        return 0;
    return bottom + size;
}
#endif

/*
 * Try to safely read one word from a kernel address.
 * Returns 0 on success and stores the value in *out.
 * Returns -1 if the address is clearly invalid (NULL, unaligned, outside kernel range).
 */
static int safe_read_kern(rt_ubase_t addr, rt_ubase_t *out)
{
    if (addr == 0 || (addr & (sizeof(rt_ubase_t) - 1)) != 0)
        return -1;

    /* Accept kernel text/data/bss range and ISR stack range. */
    if (addr >= (rt_ubase_t)__text_start && addr < (rt_ubase_t)&__bss_end)
    {
        *out = *(volatile rt_ubase_t *)addr;
        return 0;
    }

    /* Also accept dynamically allocated kernel heap (rough range). */
    if (addr >= 0x100000 && addr < 0x80000000UL)
    {
        *out = *(volatile rt_ubase_t *)addr;
        return 0;
    }

    return -1;
}

/*
 * Dump raw stack memory around an address.
 */
static void dump_stack_memory(rt_ubase_t sp, int words_before, int words_after)
{
    rt_ubase_t start, end, addr, val;

    start = (sp - words_before * sizeof(rt_ubase_t)) & ~(rt_ubase_t)0x7;
    end   = sp + words_after  * sizeof(rt_ubase_t);

    rt_kprintf("------------- Stack Memory Dump -------------\n");
    rt_kprintf("  sp = 0x%016lx\n", (unsigned long)sp);

    for (addr = start; addr < end; addr += sizeof(rt_ubase_t))
    {
        if (safe_read_kern(addr, &val) == 0)
        {
            rt_kprintf("  %s0x%016lx: 0x%016lx",
                       (addr == sp) ? ">" : " ",
                       (unsigned long)addr, (unsigned long)val);

            /* Annotate if value looks like a kernel text address. */
            if (val >= (rt_ubase_t)&__text_start && val < (rt_ubase_t)&__text_end)
                rt_kprintf("  <- kernel text");

            rt_kprintf("\n");
        }
        else
        {
            rt_kprintf("   0x%016lx: <inaccessible>\n", (unsigned long)addr);
        }
    }
}

/*
 * Walk the RISC-V frame pointer chain and print the available backtrace.
 * For -fno-omit-frame-pointer compiled code, the convention is:
 *   fp[-1]  = saved ra  (return address)
 *   fp[-2]  = saved fp  (previous frame pointer)
 *
 * How to use the crash trace:
 *
 * 1. Add these flags to both C and C++ compilation, then clean-rebuild the
 *    kernel or application being diagnosed and the relevant libraries:
 *      -g -fno-omit-frame-pointer -fno-optimize-sibling-calls
 *    Apply them when compiling each source file, not only when linking.
 *    The SDK's usual optimized builds do not preserve frame pointers by
 *    default; enabling this reporter alone does not create a frame chain.
 *
 * 2. Keep the exact matching, unstripped ELF on the build host. To reduce the
 *    deployed application's file size, strip a separate copy, for example:
 *      riscv64-unknown-linux-musl-strip --strip-debug -o app.deploy.elf app.elf
 *    Stripping debug information preserves frame pointers. Debug sections
 *    increase ELF file size but normally are not loaded into target RAM.
 *
 * 3. After a hardware fault, run the printed addr2line command on the host.
 *    For a kernel fault, use the matching rtthread.elf. For a user fault,
 *    replace the printed board application path with its local ELF path.
 *    Add -C to demangle C++ class and template names, for example:
 *      riscv64-unknown-linux-musl-addr2line -e app.elf -a -f -C ADDRESS...
 *    Replace ADDRESS... with the hexadecimal addresses from the report.
 *
 * Frame walking stops at a missing, invalid or inaccessible frame. Debug
 * symbols cannot restore frames removed by inlining, tail calls or omitted
 * frame pointers. The saved ra may duplicate the first frame's return address;
 * return addresses can resolve to the source line after a call.
 *
 * C++ hardware faults, including faults inside catch handlers, use this path.
 * Uncaught exceptions using the default terminate/abort path do not produce
 * this hardware-trap report; they need separate application/runtime diagnostics.
 */
static void dump_backtrace_fp_chain(rt_ubase_t fp, rt_ubase_t sepc,
                                    rt_ubase_t stack_pointer, rt_ubase_t saved_ra,
                                    rt_ubase_t status)
{
    rt_ubase_t ra, prev_fp;
    rt_ubase_t addrs[33];
    int naddr = 0;
    int depth;
    int is_kernel = (status & SSTATUS_SPP) != 0;
    rt_ubase_t text_lo, text_hi;
    rt_ubase_t stack_lo = 0, stack_hi = 0;
    rt_thread_t thread = rt_thread_self();

    rt_kprintf("------------- Frame Pointer Backtrace -------\n");

    if (sepc)
        rt_kprintf("  #0  pc = 0x%016lx\n", (unsigned long)sepc);

    /* Determine privilege from saved SSTATUS_SPP, independently of SP and FP. */
    if (!is_kernel)
    {
        text_lo = USER_VADDR_START;
        text_hi = USER_VADDR_TOP;
#ifdef RT_USING_USERSPACE
        stack_lo = stack_pointer;
        stack_hi = user_stack_top(thread, stack_pointer);
#endif
    }
    else
    {
        text_lo = (rt_ubase_t)&__text_start;
        text_hi = (rt_ubase_t)&__text_end;
        if (thread && stack_pointer >= (rt_ubase_t)thread->stack_addr &&
            stack_pointer < (rt_ubase_t)thread->stack_addr + thread->stack_size)
        {
            stack_lo = stack_pointer;
            stack_hi = (rt_ubase_t)thread->stack_addr + thread->stack_size;
        }
        else if (on_boot_stack(stack_pointer))
        {
            stack_lo = stack_pointer;
            stack_hi = (rt_ubase_t)__stack_cpu0;
        }
    }

    if (saved_ra >= text_lo && saved_ra < text_hi)
        addrs[naddr++] = saved_ra;

    for (depth = 1; depth < 32; depth++)
    {
        if (fp == 0 || (fp & (sizeof(rt_ubase_t) - 1)) != 0)
        {
            rt_kprintf("  -- fp=0x%lx invalid (NULL or unaligned), stop\n",
                       (unsigned long)fp);
            break;
        }

        /* Optimized code can use s0 as a general saved register. */
        if (!stack_hi || fp < stack_lo || fp - stack_lo < 2 * sizeof(rt_ubase_t) ||
            fp > stack_hi)
        {
            rt_kprintf("  -- s0/fp is outside the active stack; no frame chain available\n");
            break;
        }

        if (is_kernel)
        {
            if (safe_read_kern(fp - 1 * sizeof(rt_ubase_t), &ra) != 0 ||
                safe_read_kern(fp - 2 * sizeof(rt_ubase_t), &prev_fp) != 0)
            {
                rt_kprintf("  -- fp=0x%lx inaccessible, stop\n", (unsigned long)fp);
                break;
            }
        }
        else
        {
#ifdef RT_USING_USERSPACE
            /* Copy through the page tables, never dereference a user fp. */
            rt_ubase_t frame[2];

            if (!thread || !thread->lwp ||
                lwp_get_from_user(frame, (void *)(fp - sizeof(frame)),
                                  sizeof(frame)) != sizeof(frame))
            {
                rt_kprintf("  -- fp=0x%lx inaccessible, stop\n", (unsigned long)fp);
                break;
            }
            prev_fp = frame[0];
            ra = frame[1];
#else
            break;
#endif
        }

        rt_kprintf("  #%d  ra = 0x%016lx  (fp was 0x%016lx)",
                   depth, (unsigned long)ra, (unsigned long)fp);

        if (is_kernel && ra >= text_lo && ra < text_hi)
            rt_kprintf("  [kernel]");
        else if (ra >= USER_VADDR_START && ra < USER_VADDR_TOP)
            rt_kprintf("  [user]");
        else
            rt_kprintf("  [INVALID]");

        rt_kprintf("\n");

        if (ra < text_lo || ra >= text_hi)
        {
            rt_kprintf("  -- no valid return address in frame; stop\n");
            break;
        }

        if (naddr < 33)
            addrs[naddr++] = ra;

        if (prev_fp == 0)
        {
            rt_kprintf("  -- reached bottom of stack (prev_fp=0)\n");
            break;
        }

        if (prev_fp <= fp)
        {
            rt_kprintf("  -- prev_fp(0x%lx) <= fp(0x%lx), invalid frame chain; stop\n",
                       (unsigned long)prev_fp, (unsigned long)fp);
            break;
        }

        fp = prev_fp;
    }

    /* Print a single addr2line command with all addresses. */
    rt_kprintf("------------- addr2line command --------------\n");
    rt_kprintf("riscv64-unknown-linux-musl-addr2line -e ");
#ifdef RT_USING_USERSPACE
    if (!is_kernel && thread && thread->lwp)
    {
        const struct rt_lwp *lwp = thread->lwp;

        rt_kprintf("'%.*s'", (int)sizeof(lwp->cmd), lwp->cmd);
    }
    else
#endif
        rt_kprintf("rtthread.elf");
    rt_kprintf(" -a -f");
    if (sepc)
        rt_kprintf(" 0x%lx", (unsigned long)sepc);
    {
        int i;
        for (i = 0; i < naddr; i++)
            rt_kprintf(" 0x%lx", (unsigned long)addrs[i]);
    }
    rt_kprintf("\n");
}

/*
 * Dump the instruction bytes around the crash point.
 */
static void dump_instructions(rt_ubase_t sepc)
{
    rt_ubase_t addr;
    rt_ubase_t val;

    if (!sepc)
        return;

    rt_kprintf("------------- Code Around sepc --------------\n");

    /* Show 8 halfwords before and 8 after sepc (RISC-V instructions are 16 or 32 bit). */
    for (addr = (sepc & ~(rt_ubase_t)1) - 16; addr <= sepc + 16; addr += 2)
    {
        rt_ubase_t hw;

        rt_ubase_t aligned = addr & ~(rt_ubase_t)(sizeof(rt_ubase_t) - 1);

        if (safe_read_kern(aligned, &val) != 0)
        {
            rt_kprintf("  0x%08lx: <inaccessible>\n", (unsigned long)addr);
            continue;
        }

        /* Extract the 16-bit halfword from the 8-byte word. */
        hw = (val >> (8 * (addr - aligned))) & 0xffff;

        rt_kprintf("  %s0x%08lx: %04lx",
                   (addr == (sepc & ~(rt_ubase_t)1)) ? ">>>" : "   ",
                   (unsigned long)addr,
                   (unsigned long)hw);

        /* If this is a 32-bit instruction (low 2 bits of opcode == 0b11), show next halfword too. */
        if ((hw & 0x3) == 0x3 && addr + 2 <= sepc + 16)
        {
            rt_ubase_t hw2;
            rt_ubase_t addr2 = addr + 2;

            rt_ubase_t aligned2 = addr2 & ~(rt_ubase_t)(sizeof(rt_ubase_t) - 1);

            if (safe_read_kern(aligned2, &val) == 0)
            {
                hw2 = (val >> (8 * (addr2 - aligned2))) & 0xffff;
                rt_kprintf(" %04lx", (unsigned long)hw2);
            }
        }

        rt_kprintf("\n");
    }
}

/*
 * Dump current thread information.
 */
static void dump_thread_info(void)
{
    rt_thread_t thread = rt_thread_self();

    if (!thread)
    {
        rt_kprintf("  current thread: <none>\n");
        return;
    }

    rt_kprintf("------------- Thread Info -------------------\n");
    rt_kprintf("  name        : %s\n", thread->name);
    rt_kprintf("  status      : 0x%02x\n", thread->stat);
    rt_kprintf("  stack_addr  : 0x%016lx\n", (unsigned long)(rt_ubase_t)thread->stack_addr);
    rt_kprintf("  stack_size  : 0x%x (%u)\n", thread->stack_size, thread->stack_size);
    rt_kprintf("  stack_top   : 0x%016lx\n",
               (unsigned long)((rt_ubase_t)thread->stack_addr + thread->stack_size));

    /* Show stack usage: scan from bottom for fill pattern.
     * RTSmart uses '#' (0x23) for thread stacks (rt_thread_init),
     * or 0xdeadbeef in some configurations. Check both.
     */
    {
        rt_uint8_t *bottom = (rt_uint8_t *)thread->stack_addr;
        rt_uint8_t *top    = (rt_uint8_t *)((rt_ubase_t)thread->stack_addr + thread->stack_size);
        rt_uint8_t *p;
        rt_uint32_t used;
        rt_uint8_t fill = *bottom;  /* Read what the first byte actually is. */

        /* Only scan if the bottom looks like a fill pattern. */
        if (fill == '#' || fill == 0xef || fill == 0x23)
        {
            for (p = bottom; p < top; p++)
            {
                if (*p != fill)
                    break;
            }

            used = (rt_uint32_t)((rt_ubase_t)top - (rt_ubase_t)p);
            rt_kprintf("  stack_used  : 0x%x (%u) = %d%% of %u  (fill=0x%02x)\n",
                       used, used, (used * 100) / thread->stack_size,
                       thread->stack_size, fill);

            if (used >= thread->stack_size - 64)
                rt_kprintf("  *** STACK OVERFLOW DETECTED ***\n");
            else if (used >= (thread->stack_size * 90) / 100)
                rt_kprintf("  *** WARNING: stack usage > 90%% ***\n");
        }
        else
        {
            rt_kprintf("  stack_used  : unknown (no fill pattern, first byte=0x%02x)\n",
                       fill);
        }
    }
}

/*
 * Scan ALL threads and report stack usage, especially overflows.
 * This helps identify which thread is the corruption source.
 */
static void dump_all_thread_stacks(void)
{
    struct rt_object_information *info;
    struct rt_list_node *list, *node;
    struct rt_thread *thread;
    int count = 0;

    info = rt_object_get_information(RT_Object_Class_Thread);
    if (!info)
        return;

    rt_kprintf("------------- All Thread Stacks -------------\n");
    rt_kprintf("  %-20s  %10s  %10s  %5s  %s\n",
               "THREAD", "STACK_ADDR", "SIZE", "USED%", "STATUS");

    list = &info->object_list;
    for (node = list->next; node != list && count < 64; node = node->next, count++)
    {
        rt_uint8_t *p;
        rt_uint32_t used = 0;
        int pct = -1;
        int overflow = 0;

        thread = rt_list_entry(node, struct rt_thread, list);

        if (thread->stack_addr == RT_NULL || thread->stack_size == 0)
            continue;

        /* Detect the actual fill byte: '#' (0x23) for LWP, could differ. */
        p = (rt_uint8_t *)thread->stack_addr;
        {
            rt_uint8_t fill = *p;
            if (fill == '#' || fill == 0x23)
            {
                rt_uint8_t *top = (rt_uint8_t *)thread->stack_addr + thread->stack_size;
                while (p < top && *p == fill)
                    p++;
                used = (rt_uint32_t)((rt_ubase_t)top - (rt_ubase_t)p);
                pct = (used * 100) / thread->stack_size;
                if (used >= thread->stack_size - 64)
                    overflow = 1;
            }
        }

        if (pct >= 0)
        {
            rt_kprintf("  %-20.*s  0x%08lx  %10u  %3d%%%s%s\n",
                       RT_NAME_MAX, thread->name,
                       (unsigned long)(rt_ubase_t)thread->stack_addr,
                       thread->stack_size,
                       pct,
                       overflow ? "  *** OVERFLOW! ***" : "",
                       (pct >= 80) ? "  [HIGH]" : "");
        }
        else
        {
            rt_kprintf("  %-20.*s  0x%08lx  %10u   ???\n",
                       RT_NAME_MAX, thread->name,
                       (unsigned long)(rt_ubase_t)thread->stack_addr,
                       thread->stack_size);
        }
    }
    rt_kprintf("  Total: %d threads\n", count);
}

/*
 * Enhanced crash diagnostics — called from both handle_user and handle_trap.
 */
static void dump_crash_info(rt_size_t scause, rt_size_t stval, rt_size_t sepc,
                            struct rt_hw_stack_frame *sp)
{
    rt_size_t id = __MASKVALUE(scause, __MASK(63UL));
    int is_supervisor = (sp->sstatus & SSTATUS_SPP) != 0;
    rt_uint8_t irq_nest;

    crash_handler_depth++;
    if (crash_handler_depth > 1)
    {
        rt_kprintf("\n*** RECURSIVE CRASH (depth=%d) at sepc=0x%lx stval=0x%lx ***\n",
                   crash_handler_depth, (unsigned long)sepc, (unsigned long)stval);
        rt_kprintf("*** Halting to prevent infinite loop ***\n");
        while (1) ;
    }

    irq_nest = rt_interrupt_get_nest();
    rt_kprintf("\n============ CRASH REPORT ===================\n");
    rt_kprintf("Exception %ld: %s\n", id, get_exception_msg(id));
    rt_kprintf("  scause = 0x%016lx\n", (unsigned long)scause);
    rt_kprintf("  stval  = 0x%016lx  (fault address)\n", (unsigned long)stval);
    rt_kprintf("  sepc   = 0x%016lx  (program counter)\n", (unsigned long)sepc);
    rt_kprintf("  ra     = 0x%016lx  (return address)\n", (unsigned long)sp->ra);
    rt_kprintf("  sp     = 0x%016lx\n", (unsigned long)sp->user_sp_exc_stack);
    rt_kprintf("  frame  = 0x%016lx  (trap frame on stack)\n", (unsigned long)(rt_ubase_t)sp);
    if (is_supervisor && on_boot_stack(sp->user_sp_exc_stack))
        rt_kprintf("  sp     : ON ISR/BOOT STACK [0x%lx - 0x%lx)\n",
                   (unsigned long)(rt_ubase_t)__stack_start__,
                   (unsigned long)(rt_ubase_t)__stack_cpu0);
    rt_kprintf("  fp/s0  = 0x%016lx\n", (unsigned long)sp->s0_fp);
    rt_kprintf("  mode   = %s (SPP=%d)\n",
               is_supervisor ? "SUPERVISOR" : "USER", is_supervisor);
    /* The boot stack alone does not imply an active interrupt handler. */
    if (is_supervisor && irq_nest)
        rt_kprintf("  context: INTERRUPT CONTEXT! (nest=%u)\n", (unsigned)irq_nest);

    /* Check if sepc is within known code ranges. */
    if (sepc >= (rt_ubase_t)&__text_start && sepc < (rt_ubase_t)&__text_end)
        rt_kprintf("  sepc   : in kernel text [0x%lx - 0x%lx]\n",
                   (unsigned long)(rt_ubase_t)&__text_start,
                   (unsigned long)(rt_ubase_t)&__text_end);
    else if (sepc >= USER_VADDR_START && sepc < USER_VADDR_TOP)
        rt_kprintf("  sepc   : in user space\n");
    else
        rt_kprintf("  sepc   : OUTSIDE any known code range!\n");

    /* Check if stval is a valid address. */
    if (stval == 0)
        rt_kprintf("  stval  : NULL pointer dereference\n");
    else if (stval >= (rt_ubase_t)__text_start && stval < (rt_ubase_t)&__bss_end)
        rt_kprintf("  stval  : in kernel memory\n");
    else if (stval >= USER_VADDR_START && stval < USER_VADDR_TOP)
        rt_kprintf("  stval  : in user space\n");
    else
        rt_kprintf("  stval  : INVALID address (unmapped?)\n");

    rt_kprintf("=============================================\n\n");

    dump_regs(sp);
    dump_thread_info();
    dump_instructions(sepc);
    dump_backtrace_fp_chain(sp->s0_fp, sepc, sp->user_sp_exc_stack, sp->ra,
                            sp->sstatus);
    dump_stack_memory(sp->user_sp_exc_stack, 8, 24);

    /* Dump ALL thread stacks FIRST — this is the most important info. */
    dump_all_thread_stacks();
    crash_handler_depth--;
}

void dump_regs(struct rt_hw_stack_frame *regs)
{
    rt_kprintf("--------------Dump Registers-----------------\n");

    rt_kprintf("Function Registers:\n");
    rt_kprintf("\tra(x1) = 0x%p(",regs->ra);
    rt_kprintf(")\n");
    rt_kprintf("\tuser_sp(x2) = 0x%p(",regs->user_sp_exc_stack);
    rt_kprintf(")\n");
    rt_kprintf("\tgp(x3) = 0x%p(",regs->gp);
    rt_kprintf(")\n");
    rt_kprintf("\ttp(x4) = 0x%p(",regs->tp);
    rt_kprintf(")\n");
    rt_kprintf("Temporary Registers:\n");
    rt_kprintf("\tt0(x5) = 0x%p(",regs->t0);
    rt_kprintf(")\n");
    rt_kprintf("\tt1(x6) = 0x%p(",regs->t1);
    rt_kprintf(")\n");
    rt_kprintf("\tt2(x7) = 0x%p(",regs->t2);
    rt_kprintf(")\n");
    rt_kprintf("\tt3(x28) = 0x%p(",regs->t3);
    rt_kprintf(")\n");
    rt_kprintf("\tt4(x29) = 0x%p(",regs->t4);
    rt_kprintf(")\n");
    rt_kprintf("\tt5(x30) = 0x%p(",regs->t5);
    rt_kprintf(")\n");
    rt_kprintf("\tt6(x31) = 0x%p(",regs->t6);
    rt_kprintf(")\n");
    rt_kprintf("Saved Registers:\n");
    rt_kprintf("\ts0/fp(x8) = 0x%p(",regs->s0_fp);
    rt_kprintf(")\n");
    rt_kprintf("\ts1(x9) = 0x%p(",regs->s1);
    rt_kprintf(")\n");
    rt_kprintf("\ts2(x18) = 0x%p(",regs->s2);
    rt_kprintf(")\n");
    rt_kprintf("\ts3(x19) = 0x%p(",regs->s3);
    rt_kprintf(")\n");
    rt_kprintf("\ts4(x20) = 0x%p(",regs->s4);
    rt_kprintf(")\n");
    rt_kprintf("\ts5(x21) = 0x%p(",regs->s5);
    rt_kprintf(")\n");
    rt_kprintf("\ts6(x22) = 0x%p(",regs->s6);
    rt_kprintf(")\n");
    rt_kprintf("\ts7(x23) = 0x%p(",regs->s7);
    rt_kprintf(")\n");
    rt_kprintf("\ts8(x24) = 0x%p(",regs->s8);
    rt_kprintf(")\n");
    rt_kprintf("\ts9(x25) = 0x%p(",regs->s9);
    rt_kprintf(")\n");
    rt_kprintf("\ts10(x26) = 0x%p(",regs->s10);
    rt_kprintf(")\n");
    rt_kprintf("\ts11(x27) = 0x%p(",regs->s11);
    rt_kprintf(")\n");
    rt_kprintf("Function Arguments Registers:\n");
    rt_kprintf("\ta0(x10) = 0x%p(",regs->a0);
    rt_kprintf(")\n");
    rt_kprintf("\ta1(x11) = 0x%p(",regs->a1);
    rt_kprintf(")\n");
    rt_kprintf("\ta2(x12) = 0x%p(",regs->a2);
    rt_kprintf(")\n");
    rt_kprintf("\ta3(x13) = 0x%p(",regs->a3);
    rt_kprintf(")\n");
    rt_kprintf("\ta4(x14) = 0x%p(",regs->a4);
    rt_kprintf(")\n");
    rt_kprintf("\ta5(x15) = 0x%p(",regs->a5);
    rt_kprintf(")\n");
    rt_kprintf("\ta6(x16) = 0x%p(",regs->a6);
    rt_kprintf(")\n");
    rt_kprintf("\ta7(x17) = 0x%p(",regs->a7);
    rt_kprintf(")\n");
    rt_kprintf("sstatus = 0x%p\n",regs->sstatus);
    rt_kprintf("\t%s\n",(regs->sstatus & SSTATUS_SIE) ? "Supervisor Interrupt Enabled" : "Supervisor Interrupt Disabled");
    rt_kprintf("\t%s\n",(regs->sstatus & SSTATUS_SPIE) ? "Last Time Supervisor Interrupt Enabled" : "Last Time Supervisor Interrupt Disabled");
    rt_kprintf("\t%s\n",(regs->sstatus & SSTATUS_SPP) ? "Last Privilege is Supervisor Mode" : "Last Privilege is User Mode");
    rt_kprintf("\t%s\n",(regs->sstatus & SSTATUS_PUM) ? "Permit to Access User Page" : "Not Permit to Access User Page");
    rt_kprintf("\t%s\n",(regs->sstatus & (1 << 19)) ? "Permit to Read Executable-only Page" : "Not Permit to Read Executable-only Page");
    rt_size_t satp_v = read_csr(satp);
    rt_kprintf("satp = 0x%p\n",satp_v);

#ifdef RT_USING_USERSPACE
    rt_kprintf("\tCurrent Page Table(Physical) = 0x%p\n",__MASKVALUE(satp_v,__MASK(44)) << PAGE_OFFSET_BIT);
    rt_kprintf("\tCurrent ASID = 0x%p\n",__MASKVALUE(satp_v >> 44,__MASK(16)) << PAGE_OFFSET_BIT);
#endif

    const char *mode_str = "Unknown Address Translation/Protection Mode";

    switch(__MASKVALUE(satp_v >> 60,__MASK(4)))
    {
        case 0:
            mode_str = "No Address Translation/Protection Mode";
            break;

        case 8:
            mode_str = "Page-based 39-bit Virtual Addressing Mode";
            break;

        case 9:
            mode_str = "Page-based 48-bit Virtual Addressing Mode";
            break;
    }

    rt_kprintf("\tMode = %s\n",mode_str);
    rt_kprintf("-----------------Dump OK---------------------\n");
}

static const char *Exception_Name[] =
                                {
                                    "Instruction Address Misaligned",
                                    "Instruction Access Fault",
                                    "Illegal Instruction",
                                    "Breakpoint",
                                    "Load Address Misaligned",
                                    "Load Access Fault",
                                    "Store/AMO Address Misaligned",
                                    "Store/AMO Access Fault",
                                    "Environment call from U-mode",
                                    "Environment call from S-mode",
                                    "Reserved-10",
                                    "Reserved-11",
                                    "Instruction Page Fault",
                                    "Load Page Fault",
                                    "Reserved-14",
                                    "Store/AMO Page Fault"
                                };

static const char *Interrupt_Name[] =
                                {
                                    "User Software Interrupt",
                                    "Supervisor Software Interrupt",
                                    "Reversed-2",
                                    "Reversed-3",
                                    "User Timer Interrupt",
                                    "Supervisor Timer Interrupt",
                                    "Reversed-6",
                                    "Reversed-7",
                                    "User External Interrupt",
                                    "Supervisor External Interrupt",
                                    "Reserved-10",
                                    "Reserved-11",
                                };

extern struct rt_irq_desc isr_table[];

void generic_handle_irq(int irq)
{
    rt_isr_handler_t isr;
    void *param;

    if (irq < 0 || irq >= IRQ_MAX_NR)
    {
        LOG_E("bad irq number %d!\n", irq);
        return;
    }

    if (!irq)   // irq = 0 => no irq
    {
        LOG_W("no irq!\n");
        return;
    }
    isr = isr_table[IRQ_OFFSET + irq].handler;
    param = isr_table[IRQ_OFFSET + irq].param;
    if (isr != RT_NULL)
    {
        isr(irq, param);
    }
    /* complete irq. */
    plic_complete(irq);
}

static const char *get_exception_msg(int id)
{
    const char *msg;
    if (id < sizeof(Exception_Name) / sizeof(const char *))
    {
        msg = Exception_Name[id];
    }
    else
    {
        msg = "Unknown Exception";
    }
    return msg;
}

#ifdef RT_USING_USERSPACE
static int user_exception_signal(rt_size_t id)
{
    switch (id)
    {
    case EP_ILLEGAL_INSTRUCTION:
        return SIGILL;
    case EP_BREAKPOINT:
        return SIGTRAP;
    case EP_LOAD_ADDRESS_MISALIGNED:
    case EP_LOAD_ACCESS_FAULT:
    case EP_STORE_ADDRESS_MISALIGNED:
    case EP_STORE_ACCESS_FAULT:
        return SIGBUS;
    default:
        return SIGSEGV;
    }
}
#endif

void handle_user(rt_size_t scause, rt_size_t stval, rt_size_t sepc, struct rt_hw_stack_frame *sp)
{
    rt_size_t id = __MASKVALUE(scause, __MASK(63UL));

#ifdef RT_USING_USERSPACE
    /* user page fault */
    if (id == EP_LOAD_PAGE_FAULT ||
        id == EP_STORE_PAGE_FAULT)
    {
        if (arch_expand_user_stack((void *)stval))
        {
            return;
        }
    }
#endif

    /* Enhanced crash dump with full diagnostics. */
    dump_crash_info(scause, stval, sepc, sp);

    LOG_E("User Fault, killing thread: %s, tid: %d", rt_thread_self()->name, rt_thread_self()->tid);

#if defined (RT_RECOVERY_MPY_AUTO_EXEC_PY)
if(0x00 == rt_strncmp("micropython", rt_thread_self()->name, sizeof("micropython") - 1)) {
        extern void canmv_on_micropython_error(void);
        canmv_on_micropython_error();
    }
#endif

#ifdef RT_USING_USERSPACE
    lwp_terminate_with_status(lwp_self(),
                              LWP_WAIT_STATUS_SIGNAL(user_exception_signal(id)));
#endif
    sys_exit(-1);
}

static void vector_enable(struct rt_hw_stack_frame *sp)
{
    sp->sstatus |= SSTATUS_VS_INITIAL;
}

/**
 * we use quick method to detect V / D support, and do not distinguish V/D instruction
 */
static int illegal_inst_recoverable(rt_ubase_t stval, struct rt_hw_stack_frame *sp)
{
    // first 7 bits is opcode
    int opcode = stval & 0x7f;
    int csr = (stval & 0xFFF00000) >> 20;
    // ref riscv-v-spec-1.0, [Vector Instruction Formats]
    int width = ((stval & 0x7000) >> 12) - 1;
    int flag = 0;

    switch (opcode)
    {
    case 0x57: // V
    case 0x27: // scalar FLOAT
    case 0x07:
    case 0x73: // CSR
        flag = 1;
        break;
    }

    if (flag)
    {
        vector_enable(sp);
    }

    return flag;
}

static rt_bool_t trap_is_user_fault(rt_size_t cause, rt_size_t address,
                                    rt_ubase_t status)
{
    rt_thread_t thread = rt_thread_self();

    /* Supervisor accesses may demand-page a process's user stack, but a
     * kernel thread (or an ISR) must never enter process-exit handling. */
    if (!thread || !thread->lwp || rt_interrupt_get_nest())
        return RT_FALSE;

    return !(status & SSTATUS_SPP) ||
           ((cause == EP_LOAD_PAGE_FAULT || cause == EP_STORE_PAGE_FAULT) &&
            address >= USER_VADDR_START && address < USER_VADDR_TOP);
}

/* Trap entry */
void handle_trap(rt_size_t scause,rt_size_t stval,rt_size_t sepc,struct rt_hw_stack_frame *sp)
{
    rt_size_t id = __MASKVALUE(scause,__MASK(63UL));
    const char *msg;

    /* supervisor external interrupt */
    if ((SCAUSE_INTERRUPT & scause) && SCAUSE_S_EXTERNAL_INTR == (scause & 0xff))
    {
#ifdef RT_USING_USAGE_ROMOVE_TRAP_TIME
        rt_thread_t current_thread;
        volatile uint64_t enter_trap;
        volatile uint64_t leave_trap;
        current_thread = rt_thread_self();
        enter_trap = cpu_ticks();
        rt_interrupt_enter();
        plic_handle_irq();
        leave_trap = cpu_ticks();
        if(current_thread->user_data > (leave_trap - enter_trap))
            current_thread->user_data -= (leave_trap - enter_trap);
        rt_interrupt_leave();
#else
        rt_interrupt_enter();
        plic_handle_irq();
        rt_interrupt_leave();
#endif
        return;
    }
    else if ((SCAUSE_INTERRUPT | SCAUSE_S_TIMER_INTR) == scause)
    {
        /* supervisor timer */
        rt_interrupt_enter();
        tick_isr();
        rt_interrupt_leave();
        return;
    }
    else if (SCAUSE_INTERRUPT & scause)
    {
        if(id < sizeof(Interrupt_Name) / sizeof(const char *))
        {
            msg = Interrupt_Name[id];
        }
        else
        {
            msg = "Unknown Interrupt";
        }
        LOG_E("Unhandled Interrupt %ld:%s\n",id,msg);
    }
    else
    {
        if (scause == 0x2)
        {
            if (!(sp->sstatus & SSTATUS_VS) && illegal_inst_recoverable(stval, sp))
                return;
        }
        if (trap_is_user_fault(id, stval, sp->sstatus))
        {
            handle_user(scause, stval, sepc, sp);
            // after handle_user(), return to user space.
            // otherwise it never returns
            return ;
        }

        // handle kernel exception:
        rt_kprintf("Unhandled Exception %ld:%s\n", id, get_exception_msg(id));
    }

    /* Enhanced crash dump for kernel exceptions too. */
    dump_crash_info(scause, stval, sepc, sp);

    rt_kprintf("--------------Thread list--------------\n");
    rt_kprintf("current thread: %s\n", rt_thread_self()->name);
    list_process();

    while (1)
        ;
}

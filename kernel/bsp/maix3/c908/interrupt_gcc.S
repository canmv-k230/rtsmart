/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2018/10/02     Bernard      The first version
 * 2018/12/27     Jesven       Add SMP schedule
 * 2021/02/02     lizhirui     Add userspace support
 * 2021/12/24     JasonHu      Add user setting save/restore
 */

#define __ASSEMBLY__
#include "cpuport.h"
#include "encoding.h"
#include "stackframe.h"

    .macro COPY_TRAP_FRAME src, dst, count, tmp
    li \count, CTX_REG_NR
.Lcopy_trap_frame_\@:
    LOAD \tmp, 0(\src)
    STORE \tmp, 0(\dst)
    addi \count, \count, -1
    addi \src, \src, REGBYTES
    addi \dst, \dst, REGBYTES
    bnez \count, .Lcopy_trap_frame_\@
    .endm

  .section      .text.entry
  .align 2
  .global trap_entry
  .extern __stack_cpu0
  .extern get_current_thread_kernel_stack_top
trap_entry:
    //backup sp
    csrrw sp, sscratch, sp
    //load interrupt stack
    la sp, __stack_cpu0
    //backup context
    SAVE_ALL
    
    RESTORE_SYS_GP

    //check syscall
    csrr t0, scause
    li t1, 8//environment call from u-mode
    beq t0, t1, syscall_entry

    /* A user page fault may sleep in handle_user() (the global mm_lock), and
     * __stack_cpu0 is shared by every trap: the next trap would overwrite
     * the context of a handler asleep on it. trap_exception_stack() returns
     * where to handle the exception instead: the thread's kernel stack top
     * for a trap from user mode, the interrupted kernel sp for a kernel-mode
     * fault on a user page or an exception taken on this stack itself (in
     * an interrupt handler), 0 to stay here, or 1 to stay without attempting
     * demand paging. Interrupts stay here. */
    csrr a0, scause
    li a4, 1                        //allow user stack expansion
    bltz a0, .Lhandle_trap          //interrupt
    csrr a1, stval
    mv a2, sp
    call trap_exception_stack
    li a4, 1
    li t0, 1                        //frame cannot move; do not sleep
    beq a0, t0, .Lno_user_stack_expand
    beqz a0, .Lhandle_trap
    mv t1, sp//src
    mv sp, a0//switch stack
    addi sp, sp, -CTX_REG_NR * REGBYTES
    mv t2, sp//dst
    COPY_TRAP_FRAME t1, t2, s0, t0
    j .Lhandle_trap
.Lno_user_stack_expand:
    li a4, 0
.Lhandle_trap:
    csrr a0, scause
    csrrc a1, stval, zero
    csrr  a2, sepc
    mv    a3, sp

    /* scause, stval, sepc, sp */
    call  handle_trap

    /* handle_user() may have slept (rt_mm_lock) and resumed from
     * rt_hw_context_switch() with SIE=1. Keep interrupts off through the
     * exit path, arch_ret_to_user/user_do_signal, and RESTORE_ALL's
     * sepc/sstatus restore. */
    CLOSE_INTERRUPT

    /* need to switch new thread */
    la    s0, rt_thread_switch_interrupt_flag
    lw    s2, 0(s0)
    beqz  s2, spurious_interrupt
    sw    zero, 0(s0)

.global rt_hw_context_switch_interrupt_do
rt_hw_context_switch_interrupt_do:

//swap to thread kernel stack
    csrr t0, sstatus
    andi t0, t0, 0x100
    beqz t0, __restore_sp_from_tcb_interrupt

__restore_sp_from_sscratch_interrupt:
    csrr t0, sscratch
    j __move_stack_context_interrupt

__restore_sp_from_tcb_interrupt:
    la    s0, rt_interrupt_from_thread
    LOAD  a0, 0(s0)
    jal rt_thread_sp_to_thread
    jal get_thread_kernel_stack_top
    mv t0, a0

__move_stack_context_interrupt:
    mv t1, sp//src
    mv sp, t0//switch stack
    addi sp, sp, -CTX_REG_NR * REGBYTES
    //copy context
    mv t2, sp//dst
    COPY_TRAP_FRAME t1, t2, s0, t0

    la    s0, rt_interrupt_from_thread
    LOAD  s1, 0(s0)
    STORE sp, 0(s1)

    la    s0, rt_interrupt_to_thread
    LOAD  s1, 0(s0)
    LOAD  sp, 0(s1)

    #ifdef RT_USING_USERSPACE
        mv a0, s1
        jal rt_thread_sp_to_thread
        jal lwp_mmu_switch
    #endif

    LOAD t0, 2 * REGBYTES(sp)
    andi t0, t0, 0x100
    beqz t0, arch_ret_to_user

    RESTORE_ALL
    sret

spurious_interrupt:
    LOAD t0, 2 * REGBYTES(sp)
    andi t0, t0, 0x100
    bnez t0, 1f

    mv s2, sp
    call lwp_user_return_need_work
    beqz a0, 1f

    la a0, rt_current_thread
    LOAD a0, 0(a0)
    jal get_thread_kernel_stack_top
    mv t0, a0

    mv t1, s2
    mv sp, t0
    addi sp, sp, -CTX_REG_NR * REGBYTES
    mv t2, sp
    COPY_TRAP_FRAME t1, t2, s0, t0

    j arch_ret_to_user
1:
    RESTORE_ALL
    sret

.global rt_hw_interrupt_enable
rt_hw_interrupt_enable:
    csrs sstatus, a0    /* restore to old csr */
    jr ra

.global rt_hw_interrupt_disable
rt_hw_interrupt_disable:
    csrrci a0, sstatus, 2   /* clear SIE */
    jr ra

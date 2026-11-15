#!/usr/bin/env python3
"""Exercise DWC2 descriptor-DMA isochronous scheduling (CherryUSB/port/dwc2_linux/hcd_ddma.c).

hcd_ddma.c is compiled unchanged with the port's real core.h/hcd.h against a
model of the core: a frame counter, the periodic frame list and schinfo, and
the HS (256-slot) or FS (64-slot) isochronous descriptor list. The core
fetches each slot's descriptor one (micro)frame early, delivers a packet of a
length unique to that (micro)frame if it is active and writes the status back
(clearing the active bit), optionally only after the next SOF (late
transfers); IOC descriptors latch XferCompl and raise the channel interrupt,
which runs dwc2_hcd_complete_xfer_ddma() after a scripted latency. Handlers
and init may cross a (micro)frame boundary after sampling HFNUM. Completed
URBs are resubmitted after a scripted bottom-half delay.

Checked throughout: the core services armed packets exactly in submission
order (no lap-late or out-of-order descriptors), a re-arm never rewrites a
descriptor the core has not processed, every completed packet reports the
length and status the core delivered for it, and the stream keeps running.
Scenarios: UVC queue depth at interval 1 with random interrupt latency and
extra scheduling passes (no overstep recovery may fire), a genuine overstep
(the core runs past td_last while software is held off), coalesced IOC
interrupts after a long interrupt stall, intervals 2 and 4, deep queues that
leave QTDs partly armed, late writebacks (once around a stall, and all of
them), a stall over one second (frame counter half-range), and the FS list.
Run with python3 -B kernel/bsp/maix3/testcase/host/test_dwc2_isoc_ddma.py
[path/to/hcd_ddma.c].
"""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

BSP = Path(__file__).resolve().parents[2]
KERNEL = BSP.parents[1]
RTT = KERNEL / "rt-thread"
USB = BSP / "components/CherryUSB"
PORT = USB / "port/dwc2_linux"

RTCONFIG = r"""
#pragma once
#define RT_NAME_MAX 20
#define RT_ALIGN_SIZE 8
#define RT_THREAD_PRIORITY_MAX 32
#define RT_TICK_PER_SECOND 1000
#define RT_USING_DEVICE
#define RT_USING_MUTEX
#define RT_USING_SEMAPHORE
#define RT_USING_MEMPOOL
#define RT_USING_HEAP
#define RT_USING_MUSL
#define RT_USING_SYSTEM_WORKQUEUE
#define ARCH_CPU_64BIT
#define CHERRY_USB_HC_DRV_DWC2
"""

# Board headers pulled in by the port's adapter.h.
HEADERS = {
    "riscv_io.h": "#pragma once\n#include <stdint.h>\n"
                  "uint32_t readl(const volatile void *addr);\n"
                  "void writel(uint32_t value, volatile void *addr);\n",
    "tick.h": "#pragma once\n",
    "mmu.h": "#pragma once\ntypedef struct { int unused; } rt_mmu_info;\n"
             "void *rt_hw_mmu_v2p(rt_mmu_info *info, void *v_addr);\n",
    "cache.h": "#pragma once\nvoid rt_hw_cpu_dcache_clean(void *addr, int size);\n"
               "void rt_hw_cpu_dcache_invalidate(void *addr, int size);\n",
    "page.h": "#pragma once\n#define PAGE_SIZE 4096\n#define PV_OFFSET 0\n"
              "void *rt_pages_alloc(rt_uint32_t size_bits);\n"
              "int rt_pages_free(void *addr, rt_uint32_t size_bits);\n",
    "ioremap.h": "#pragma once\nvoid *rt_ioremap(void *paddr, size_t size);\n"
                 "void rt_iounmap(volatile void *addr);\n",
}

MODEL = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hcd_ddma.c"

/* ---- platform ---- */
static u8 regs[0x10000];
uint32_t readl(const volatile void *addr) { uint32_t v; memcpy(&v, (const void *)addr, 4); return v; }
void writel(uint32_t v, volatile void *addr) { memcpy((void *)addr, &v, 4); }
rt_mmu_info mmu_info;
void *rt_hw_mmu_v2p(rt_mmu_info *info, void *v) { (void)info; return v; }
void rt_hw_cpu_dcache_clean(void *a, int s) { (void)a; (void)s; }
void rt_hw_cpu_dcache_invalidate(void *a, int s) { (void)a; (void)s; }
rt_base_t rt_hw_interrupt_disable(void) { return 0; }
void rt_hw_interrupt_enable(rt_base_t level) { (void)level; }
void *rt_calloc(rt_size_t count, rt_size_t size) { return calloc(count, size); }
void rt_free(void *p) { free(p); }
void *rt_malloc_align(rt_size_t size, rt_size_t align) { return aligned_alloc(align, (size + align - 1) / align * align); }
void rt_free_align(void *p) { free(p); }
void *rt_memset(void *p, int c, rt_ubase_t n) { return memset(p, c, n); }
void *rt_mp_alloc_align(rt_mp_t mp, rt_int32_t time) { (void)mp; (void)time; return NULL; }
void rt_mp_free_align(struct rt_mempool *mp, void *block) { (void)mp; (void)block; }
void rt_mp_free(void *block) { free(block); }

/* ---- scenario state ---- */
#define MAX_URBS 8
#define MAX_PKTS 32
#define DMA_BASE 0x40000000u
#define DMA_STRIDE 0x100000u

struct murb {
    struct dwc2_hcd_urb *urb;
    struct urb *uu;
    long seq;               /* stream position of the current submission */
    int submitted;
    long resubmit_at;       /* -1: not pending */
    int serviced[MAX_PKTS];
    u32 len[MAX_PKTS];
};

static struct {
    const char *name;
    int hs, inc, P, nurb, maxlen, n;
    long now, start, end;
    unsigned rng;
    int lat_max, resub_min, resub_max, kick_on_submit, kick_every;
    long stall_from, stall_to;      /* channel interrupt held off */
    int straddle;                   /* 1/n handlers cross a (micro)frame boundary */
    int init_straddle, in_kick;     /* 1/n inits cross one after sampling HFNUM */
    int late, late_left;            /* 1/n IOC writebacks land after the next SOF */
    long late_from, late_to;        /* ... within this window, at most late_left (<0: any) */
    int check_idle;
    /* core: descriptors of this and the next slot are already fetched */
    struct fetch { int valid; long slot; struct dwc2_dma_desc d; } cur, next;
    long armed_at[256];
    long wb_gen, wb_at[256];        /* order of core status writebacks */
    int irq_pending;
    long irq_at;
    /* stream */
    long next_seq, exp_seq, exp_done;
    int exp_k;
    long completions, last_completion, idle_backlog, rearms;
    long kick_at, kick_mark, kick_latency, first_rearm_at;
} M;
static struct murb U[MAX_URBS];
static struct dwc2_hsotg H;
static struct dwc2_qh Q;
static struct dwc2_host_chan C;
static u32 frame_list[FRLISTEN_64_SIZE];

#define fail(...) do { \
    printf("FAIL %s at %s %ld (slot %ld): ", M.name, M.hs ? "uframe" : "frame", \
           M.now, M.now & (M.n - 1)); \
    printf(__VA_ARGS__); printf("\n"); exit(1); } while (0)

static unsigned rnd(unsigned range)
{
    M.rng ^= M.rng << 13; M.rng ^= M.rng >> 17; M.rng ^= M.rng << 5;
    return range ? M.rng % range : 0;
}

static int in_stall(void) { return M.now >= M.stall_from && M.now < M.stall_to; }

/* ---- dwc2 functions outside hcd_ddma.c ---- */
static void core_fetch(void);
static void core_finish(void);
int dwc2_hcd_get_frame_number(struct dwc2_hsotg *hsotg)
{
    int frame = (int)(M.now & HFNUM_MAX_FRNUM);

    (void)hsotg;
    if (M.in_kick && M.init_straddle && !rnd(M.init_straddle)) {
        /* the (micro)frame ends after init sampled it, before it writes */
        M.in_kick = 0;
        core_finish();
        M.now++;
        core_fetch();
    }
    return frame;
}
void dwc2_hc_start_transfer_ddma(struct dwc2_hsotg *hsotg, struct dwc2_host_chan *chan) { (void)hsotg; chan->xfer_started = 1; }
void dwc2_hc_halt(struct dwc2_hsotg *hsotg, struct dwc2_host_chan *chan, enum dwc2_halt_status s) { (void)hsotg; (void)chan; (void)s; }
void dwc2_hc_cleanup(struct dwc2_hsotg *hsotg, struct dwc2_host_chan *chan) { (void)hsotg; chan->xfer_started = 0; }
int dwc2_hcd_qh_add(struct dwc2_hsotg *hsotg, struct dwc2_qh *qh) { (void)hsotg; (void)qh; return 0; }
void dwc2_hcd_qh_unlink(struct dwc2_hsotg *hsotg, struct dwc2_qh *qh) { (void)hsotg; list_del_init(&qh->qh_list_entry); }
enum dwc2_transaction_type dwc2_hcd_select_transactions(struct dwc2_hsotg *hsotg) { (void)hsotg; return DWC2_TRANSACTION_NONE; }
void dwc2_hcd_save_data_toggle(struct dwc2_hsotg *hsotg, struct dwc2_host_chan *chan, int chnum, struct dwc2_qtd *qtd)
{ (void)hsotg; (void)chan; (void)chnum; (void)qtd; }

void dwc2_hcd_qh_giveback_qtds(struct dwc2_hsotg *hsotg, struct dwc2_qh *qh, int status)
{
    struct dwc2_qtd *qtd, *qtd_tmp;

    list_for_each_entry_safe(qtd, qtd_tmp, &qh->qtd_list, qtd_list_entry) {
        dwc2_host_complete(hsotg, qtd, status);
        dwc2_hcd_qtd_unlink_and_free(hsotg, qtd, qh);
    }
}

static void kick(void);
void dwc2_hcd_queue_transactions(struct dwc2_hsotg *hsotg, enum dwc2_transaction_type tr_type)
{
    (void)hsotg; (void)tr_type;
    if (Q.channel)
        kick();
}

void dwc2_host_complete(struct dwc2_hsotg *hsotg, struct dwc2_qtd *qtd, int status)
{
    struct dwc2_hcd_urb *urb = qtd->urb;
    int i, k;

    (void)hsotg;
    for (i = 0; i < M.nurb && U[i].urb != urb; i++)
        ;
    if (i == M.nurb || !U[i].submitted)
        fail("completion of an URB that is not submitted");
    if (status)
        fail("URB seq %ld completed with status %d", U[i].seq, status);
    if (U[i].seq != M.exp_done)
        fail("URB seq %ld completed, expected seq %ld first", U[i].seq, M.exp_done);
    for (k = 0; k < M.P; k++) {
        if (!U[i].serviced[k])
            fail("URB seq %ld completed before the core serviced packet %d", U[i].seq, k);
        if (urb->iso_descs[k].actual_length != U[i].len[k] || urb->iso_descs[k].status)
            fail("URB seq %ld packet %d: actual_length %u status %d, the core delivered %u",
                 U[i].seq, k, urb->iso_descs[k].actual_length, (int)urb->iso_descs[k].status,
                 U[i].len[k]);
    }
    if (urb->error_count)
        fail("URB seq %ld error_count %u", U[i].seq, urb->error_count);
    U[i].submitted = 0;
    U[i].resubmit_at = M.now + M.resub_min + rnd(M.resub_max - M.resub_min + 1);
    M.exp_done++;
    M.completions++;
    M.last_completion = M.now;
    qtd->urb = NULL;    /* dwc2_host_complete() frees the dwc2 URB */
}

/* ---- software ---- */

/* Scheduling pass over the QH (dwc2_queue_transaction() -> start_xfer) */
static void kick(void)
{
    static struct dwc2_dma_desc before[256];
    int mask = M.n - 1, j, started = C.xfer_started, ntd = Q.ntd;
    int td_last = Q.td_last, first = -1, first_dist = M.n;
    long t0 = M.now, gen0 = M.wb_gen;

    memcpy(before, Q.desc_list, M.n * sizeof(before[0]));
    M.in_kick = 1;
    dwc2_hcd_start_xfer_ddma(&H, &Q);
    M.in_kick = 0;

    for (j = 0; j < M.n; j++) {
        struct dwc2_dma_desc *b = &before[j], *a = &Q.desc_list[j];

        if (M.wb_at[j] > gen0)
            continue;   /* the core wrote it back while init crossed a boundary */
        if ((b->status & HOST_DMA_A) &&
            (a->buf != b->buf || (a->status & ~HOST_DMA_IOC) != (b->status & ~HOST_DMA_IOC)))
            fail("descriptor slot %d was re-armed while still active", j);
        if ((a->status & HOST_DMA_A) && (!(b->status & HOST_DMA_A) || a->buf != b->buf)) {
            int dist = (j - td_last) & mask;

            if (((j - M.now) & mask) <= 1)
                fail("armed slot %d, which the core has already fetched", j);
            M.armed_at[j] = M.now;
            if (dist < first_dist) {
                first_dist = dist;
                first = j;
            }
        }
    }
    if (first >= 0 && ntd && started && first != td_last) {
        /* td_last moved ahead: overstep recovery */
        if (!M.rearms++)
            M.first_rearm_at = t0;
        if (((first - M.now) & mask) > (M.hs ? 4 : 3) + M.inc)
            fail("re-armed at slot %d, %ld slots ahead of the core", first,
                 (long)((first - M.now) & mask));
    }
}

static void assign_channel(void)
{
    list_del_init(&C.hc_list_entry);
    C.qh = &Q;
    C.xfer_started = 0;
    C.halt_status = 0;
    Q.channel = &C;
    list_move_tail(&Q.qh_list_entry, &H.periodic_sched_assigned);
}

static void queue(int i)
{
    struct dwc2_hcd_urb *urb = U[i].urb;
    struct dwc2_qtd *qtd = calloc(1, sizeof(*qtd));
    int k;

    for (k = 0; k < M.P; k++) {
        urb->iso_descs[k].actual_length = 0xdead;
        urb->iso_descs[k].status = 0x7777;
        U[i].serviced[k] = 0;
    }
    urb->error_count = 0;
    qtd->urb = urb;
    qtd->qh = &Q;
    urb->qtd = qtd;
    U[i].seq = M.next_seq++;
    U[i].submitted = 1;
    list_add_tail(&qtd->qtd_list_entry, &Q.qtd_list);
}

/* URB enqueue: a QH without a channel gets one and starts; else it waits */
static void submit(int i, int may_kick)
{
    queue(i);
    if (!Q.channel) {
        assign_channel();
        kick();
    } else if (may_kick && M.kick_on_submit) {
        kick();
    }
}

/* ---- core ---- */
static int scheduled_at(long t)
{
    long fr = M.hs ? t >> 3 : t;

    if (!Q.channel || !C.xfer_started)
        return 0;
    if (!(H.frame_list[fr & (FRLISTEN_64_SIZE - 1)] & (1u << C.hc_num)))
        return 0;
    return !M.hs || (C.schinfo & (1u << (t & 7)));
}

static int backlog(void)
{
    int i, k;

    for (i = 0; i < M.nurb; i++)
        if (U[i].submitted)
            for (k = 0; k < M.P; k++)
                if (!U[i].serviced[k])
                    return 1;
    return 0;
}

/*
 * Start of (micro)frame now: the core reads the descriptor of slot now + 1
 * ("HC always fetches the descriptor corresponding to curr_frame+1"), so
 * software can no longer change what slots now and now + 1 do.
 */
static void core_fetch(void)
{
    long t = M.now + 1;

    M.cur = M.next;
    M.next.valid = scheduled_at(t);
    M.next.slot = t & (M.n - 1);
    if (M.next.valid)
        M.next.d = Q.desc_list[M.next.slot];
}

/* Transaction of (micro)frame @t done: status written back, IOC raised */
static void core_service(const struct fetch *f, long t)
{
    long slot = f->slot;
    u32 buf, nbytes, len;
    int i, k;

    buf = f->d.buf;
    i = (buf - DMA_BASE) / DMA_STRIDE;
    k = (buf - DMA_BASE) % DMA_STRIDE / M.maxlen;
    if (buf < DMA_BASE || i >= M.nurb || (buf - DMA_BASE) % DMA_STRIDE % M.maxlen || k >= M.P)
        fail("active descriptor with buffer %#x", buf);
    if (!U[i].submitted || U[i].seq != M.exp_seq || k != M.exp_k)
        fail("core serviced URB seq %ld packet %d, expected seq %ld packet %d",
             U[i].submitted ? U[i].seq : -1, k, M.exp_seq, M.exp_k);
    if (t - M.armed_at[slot] >= M.n)
        fail("descriptor armed at %ld serviced a lap late", M.armed_at[slot]);
    if (M.kick_mark >= 0 && M.armed_at[slot] >= M.kick_mark) {
        M.kick_latency = t - M.kick_mark;
        M.kick_mark = -1;
    }
    nbytes = (f->d.status & HOST_DMA_ISOC_NBYTES_MASK) >> HOST_DMA_ISOC_NBYTES_SHIFT;
    if (nbytes != (u32)M.maxlen)
        fail("descriptor armed for %u bytes", nbytes);
    len = 1 + (u32)((t * 2654435761u) >> 7) % M.maxlen;
    U[i].serviced[k] = 1;
    U[i].len[k] = len;
    M.wb_at[slot] = ++M.wb_gen;
    Q.desc_list[slot].status = (f->d.status & HOST_DMA_IOC) |
        ((nbytes - len) << HOST_DMA_ISOC_NBYTES_SHIFT & HOST_DMA_ISOC_NBYTES_MASK);
    if (++M.exp_k == M.P) {
        M.exp_k = 0;
        M.exp_seq++;
    }
    if (f->d.status & HOST_DMA_IOC) {
        /* XferCompl latches in HCINT until the handler clears it */
        writel(readl(regs + HCINT(C.hc_num)) | HCINTMSK_XFERCOMPL, regs + HCINT(C.hc_num));
        if (!M.irq_pending) {
            M.irq_pending = 1;
            M.irq_at = M.now + 1 + rnd(M.lat_max + 1);
        }
    }
}

static struct { int valid; struct fetch f; long t; } late;

/* End of the (micro)frame */
static void core_late(void);
static void core_finish(void)
{
    if (late.valid && late.t < M.now)
        core_late();    /* lands before the next transfer completes */
    if (!M.cur.valid || !scheduled_at(M.now))
        return;
    if (!(M.cur.d.status & HOST_DMA_A)) {
        if (M.now >= M.end - 3000 && M.now >= M.start + 500 && M.check_idle && backlog())
            M.idle_backlog++;
        return;
    }
    if (M.late && (M.cur.d.status & HOST_DMA_IOC) && !late.valid && M.late_left &&
        M.now >= M.late_from && M.now < M.late_to && !rnd(M.late)) {
        /* a late transfer: writeback and IOC land after the next SOF */
        late.valid = 1;
        late.f = M.cur;
        late.t = M.now;
        if (M.late_left > 0)
            M.late_left--;
        return;
    }
    core_service(&M.cur, M.now);
}

/* A late writeback lands, after the handler of the following (micro)frame */
static void core_late(void)
{
    if (late.valid) {
        late.valid = 0;
        core_service(&late.f, late.t);
    }
}

/* ---- scenario driver ---- */
static void setup(const char *name, int hs, int inc, int P, int nurb, unsigned seed)
{
    int i;

    for (i = 0; i < M.nurb; i++) {
        free(U[i].urb);
        free(U[i].uu);
    }
    while (Q.desc_list && !list_empty(&Q.qtd_list)) {
        struct dwc2_qtd *qtd = list_first_entry(&Q.qtd_list, struct dwc2_qtd, qtd_list_entry);

        list_del(&qtd->qtd_list_entry);
        free(qtd);
    }
    free(Q.desc_list);
    free(Q.n_bytes);
    memset(&M, 0, sizeof(M));
    memset(U, 0, sizeof(U));
    memset(&H, 0, sizeof(H));
    memset(&Q, 0, sizeof(Q));
    memset(&C, 0, sizeof(C));
    memset(frame_list, 0, sizeof(frame_list));

    M.name = name;
    M.hs = hs;
    M.inc = inc;
    M.P = P;
    M.nurb = nurb;
    M.maxlen = hs ? 3072 : 1023;
    M.n = hs ? MAX_DMA_DESC_NUM_HS_ISOC : MAX_DMA_DESC_NUM_GENERIC;
    M.rng = seed;
    M.stall_from = M.stall_to = -1;
    M.late_from = 0;
    M.late_to = 1L << 40;
    M.late_left = -1;
    late.valid = 0;
    M.kick_mark = M.kick_at = M.first_rearm_at = -1;
    M.kick_latency = -1;
    M.resub_min = 1;
    M.resub_max = 8;
    M.check_idle = 1;
    /* start close to the 14-bit frame counter wrap */
    M.now = M.start = HFNUM_MAX_FRNUM + 1 - 300;

    H.regs = (void *)regs;
    H.frame_list = frame_list;
    H.frame_list_sz = sizeof(frame_list);
    INIT_LIST_HEAD(&H.periodic_sched_assigned);
    INIT_LIST_HEAD(&H.free_hc_list);
    Q.ep_type = USB_ENDPOINT_XFER_ISOC;
    Q.ep_is_in = 1;
    Q.dev_speed = hs ? USB_SPEED_HIGH : USB_SPEED_FULL;
    Q.host_interval = inc;
    Q.maxp = M.maxlen;
    Q.next_active_frame = 0x1235;
    Q.desc_list = calloc(M.n, sizeof(struct dwc2_dma_desc));
    Q.n_bytes = calloc(M.n, sizeof(u32));
    Q.desc_list_sz = M.n * sizeof(struct dwc2_dma_desc);
    INIT_LIST_HEAD(&Q.qtd_list);
    INIT_LIST_HEAD(&Q.qh_list_entry);
    C.hc_num = 0;
    C.ep_type = USB_ENDPOINT_XFER_ISOC;
    C.ep_is_in = 1;
    C.speed = Q.dev_speed;
    C.max_packet = M.maxlen;
    INIT_LIST_HEAD(&C.hc_list_entry);
    list_add_tail(&C.hc_list_entry, &H.free_hc_list);

    for (i = 0; i < nurb; i++) {
        int k;

        U[i].urb = calloc(1, sizeof(struct dwc2_hcd_urb) + P * sizeof(struct dwc2_hcd_iso_packet_desc));
        U[i].uu = calloc(1, sizeof(struct urb) + P * sizeof(struct usbh_iso_frame_packet));
        U[i].uu->num_of_iso_packets = P;
        U[i].urb->priv = U[i].uu;
        U[i].urb->packet_count = P;
        U[i].urb->dma = DMA_BASE + i * DMA_STRIDE;
        for (k = 0; k < P; k++) {
            U[i].urb->iso_descs[k].offset = k * M.maxlen;
            U[i].urb->iso_descs[k].length = M.maxlen;
        }
        U[i].resubmit_at = -1;
    }
}

/* Queue the first @armed URBs, start the channel, queue the rest unarmed */
static void begin(int armed)
{
    int i;

    for (i = 0; i < armed; i++)
        queue(i);
    assign_channel();
    kick();
    for (i = armed; i < M.nurb; i++)
        submit(i, 0);
}

static void run(long frames)
{
    M.end = M.now + frames;
    M.idle_backlog = 0;     /* counted over the last 3000 slots of this run */
    for (; M.now < M.end; M.now++) {
        int i;

        core_fetch();
        if (M.irq_pending && M.now >= M.irq_at && !in_stall()) {
            M.irq_pending = 0;
            writel(readl(regs + HCINT(C.hc_num)) & ~HCINTMSK_XFERCOMPL, regs + HCINT(C.hc_num));
            if (M.straddle && !rnd(M.straddle)) {
                /* the (micro)frame ends between clearing HCINT and the scan */
                core_finish();
                M.now++;
                core_fetch();
            }
            if (Q.channel)
                dwc2_hcd_complete_xfer_ddma(&H, &C, C.hc_num, DWC2_HC_XFER_COMPLETE);
        }
        core_late();
        for (i = 0; i < M.nurb; i++)
            if (U[i].resubmit_at >= 0 && M.now >= U[i].resubmit_at && !in_stall()) {
                U[i].resubmit_at = -1;
                submit(i, 1);
            }
        if (M.kick_every && Q.channel && !rnd(M.kick_every))
            kick();
        if (M.now == M.kick_at) {
            /* scheduling pass from another endpoint's traffic */
            M.kick_mark = M.now;
            kick();
        }
        core_finish();
    }
}

static void expect_streaming(void)
{
    long urb_time = (long)M.P * M.inc;

    if (M.now - M.last_completion > urb_time * (M.nurb + 2) + 64)
        fail("stream stalled: last completion at %ld, %ld completions", M.last_completion,
             M.completions);
    if (M.idle_backlog)
        fail("core idled %ld times in the last 3000 slots with packets queued", M.idle_backlog);
}

static void steady(const char *name, int hs, int inc, int P, int nurb, unsigned seed,
                   int kick_on_submit, int kick_every, long frames)
{
    setup(name, hs, inc, P, nurb, seed);
    /* up to 0.4 ms interrupt latency, 5 ms bottom-half resubmission delay */
    M.lat_max = hs ? 3 : 0;
    M.resub_max = hs ? 40 : 5;
    M.kick_on_submit = kick_on_submit;
    M.kick_every = kick_every;
    M.straddle = 4;
    M.init_straddle = 5;
    /* a list holding less than two URBs restarts between them */
    M.check_idle = nurb > 1 && M.n / inc >= 2 * P;
    begin(nurb);
    run(frames);
    expect_streaming();
    if (M.completions < frames / ((long)P * inc + (M.check_idle ? 0 : M.resub_max + 48)) -
        nurb - 4)
        fail("only %ld completions", M.completions);
}

/* (a) UVC depth, interval 1: the core never oversteps, so nothing is re-armed */
static void scenario_steady(void)
{
    unsigned seed;

    for (seed = 1; seed <= 6; seed++) {
        steady("steady-hs-interval1", 1, 1, 32, seed <= 4 ? 5 : 7, seed, seed & 1,
               seed > 2 ? 7 : 0, 40000);
        if (M.rearms)
            fail("overstep recovery fired %ld times without an overstep", M.rearms);
    }
    puts("ok steady-hs-interval1: 5 and 7 x 32 packets, random latency and scheduling passes");
}

/* Absolute (micro)frame at which the core reaches td_last, right after begin() */
static long td_last_time(void)
{
    return M.now + ((Q.td_last - M.now) & (M.n - 1));
}

/*
 * (b) Overstep: URBs armed, more queued unarmed, the interrupt held off while
 * the core runs up to and past td_last; a scheduling pass (another
 * endpoint's traffic) then arms the queued URBs. From the slot before
 * td_last on (the core has fetched td_last by then) they must land just
 * ahead of the core; earlier, plainly at td_last.
 */
static void overstep(const char *name, int hs, int inc, int P, int nurb, int armed, long p)
{
    setup(name, hs, inc, P, nurb, 7 + p);
    M.lat_max = 0;
    M.resub_max = hs ? 20 : 3;
    begin(armed);
    M.init_straddle = 2;
    M.kick_at = td_last_time() + p;
    M.stall_from = M.now;
    M.stall_to = M.kick_at + (hs ? 60 : 20);
    run(hs ? 6000 : 3000);
    if (p >= -2 && M.first_rearm_at != M.kick_at)
        fail("core %ld past td_last: overstep recovery first fired at %ld", p,
             M.first_rearm_at);
    if (p < -2 && M.rearms)
        fail("core %ld before td_last: overstep recovery fired", -p);
    /* re-armed 4 uframes (2 frames) ahead, rounded up to the interval phase */
    if (M.kick_latency < 0)
        fail("core %ld past td_last: the armed packets never reached the core", p);
    if (M.kick_latency > (p < -2 ? -p : (hs ? 4 : 3) + inc - 1) + 1)
        fail("core %ld past td_last: armed packets reached the core %ld slots later", p,
             M.kick_latency);
    expect_streaming();
}

static void scenario_overstep(void)
{
    long p;

    for (p = -4; p <= 110; p += p < 4 ? 1 : 9)
        overstep("overstep-hs", 1, 1, 32, 5, 3, p);
    /* intervals 2 and 4: re-armed slots keep the endpoint's uframe phase */
    for (p = -8; p <= 100; p += p < 8 ? 1 : 7) {
        overstep("overstep-hs-interval2", 1, 2, 32, 4, 2, p);
        overstep("overstep-hs-interval4", 1, 4, 32, 3, 1, p);
    }
    puts("ok overstep-hs: re-armed ahead of the core from the slot before td_last on, "
         "intervals 1/2/4");
}

/*
 * Coalesced IOCs: streaming, the interrupt is held off long enough for the
 * core to finish every armed URB, so one interrupt reports them all.
 */
static void scenario_coalesced(void)
{
    long behind;

    for (behind = 0; behind < 400; behind += 13) {
        setup("coalesced-ioc-hs", 1, 1, 32, 5, 101 + behind);
        M.lat_max = 2;
        M.resub_max = 30;
        M.straddle = 3;
        M.init_straddle = 5;
        begin(5);
        run(3000);
        M.stall_from = M.now;
        M.stall_to = M.now + 100 + behind;
        run(9000);
        expect_streaming();
    }
    puts("ok coalesced-ioc-hs: stream resumes after interrupt stalls of 100..400 uframes");
}

/* (c) Intervals 2 and 4: each packet keeps its own status and length */
static void scenario_interval(void)
{
    steady("interval2-hs", 1, 2, 32, 3, 21, 0, 0, 20000);
    steady("interval2-hs-kicks", 1, 2, 32, 3, 22, 1, 9, 20000);
    steady("interval4-hs", 1, 4, 32, 1, 23, 0, 0, 20000);
    steady("interval4-hs-kicks", 1, 4, 32, 1, 24, 1, 9, 20000);
    puts("ok interval2/4-hs: every packet reports its own length");
}

/*
 * (e) Deep queues: more packets queued than slots, so a fresh start must keep
 * the slots before td_first empty, and interrupt stalls restart the list.
 */
static void scenario_deep(void)
{
    static const int cfg[][2] = { { 2, 5 }, { 4, 3 }, { 1, 8 } };
    unsigned i, seed;

    for (i = 0; i < sizeof(cfg) / sizeof(cfg[0]); i++)
        for (seed = 1; seed <= 3; seed++) {
            steady("deep-hs", 1, cfg[i][0], 32, cfg[i][1], 31 + seed + 7 * i, seed & 1,
                   seed == 3 ? 11 : 0, 30000);
            M.stall_from = M.now;
            M.stall_to = M.now + 150 + 97 * seed;
            run(12000);
            expect_streaming();
        }
    puts("ok deep-hs: intervals 1/2/4 with more packets queued than slots");
}

/*
 * Partly armed QTDs: at interval 8 with 32 packets (16 with 16) one URB spans
 * the whole list, so it is armed in two passes; whether the first pass gets
 * 31 or 32 slots depends on the microframe the list starts in. Also an
 * overstep while such a QTD is half armed and unscanned.
 */
static void scenario_partial(void)
{
    static const int cfg[][3] = { { 8, 32, 2 }, { 8, 32, 1 }, { 16, 16, 2 } };
    unsigned i, off;
    long p;

    for (i = 0; i < sizeof(cfg) / sizeof(cfg[0]); i++)
        for (off = 0; off < 8; off++) {
            setup("partial-hs", 1, cfg[i][0], cfg[i][1], cfg[i][2], 71 + off + 8 * i);
            M.lat_max = 3;
            M.resub_max = 40;
            M.kick_on_submit = off & 1;
            M.kick_every = off & 2 ? 13 : 0;
            M.straddle = 4;
            M.init_straddle = 5;
            M.check_idle = 0;   /* the list restarts within every URB */
            M.now += off;
            M.start = M.now;
            begin(cfg[i][2]);
            run(40000);
            expect_streaming();
            if (M.completions < 40000 / ((long)cfg[i][1] * cfg[i][0] + 100) - 4)
                fail("only %ld completions", M.completions);
        }
    for (off = 0; off < 8; off++)
        for (p = 1; p < 100; p += 17) {
            setup("partial-overstep-hs", 1, 8, 32, 2, 91 + off + p);
            M.resub_max = 20;
            M.check_idle = 0;
            M.now += off;
            M.start = M.now;
            begin(2);
            M.kick_at = td_last_time() + p;
            M.stall_from = M.now;
            M.stall_to = M.kick_at + 40;
            run(8000);
            expect_streaming();
        }
    puts("ok partial-hs: URBs armed in two passes, also across an overstep");
}

/*
 * Late writeback: a transfer finishing late (high-bandwidth packet, other
 * periodic channels, DDR contention) writes its status and raises IOC after
 * the next SOF, i.e. after the frame number already shows the core past it.
 * Once right around the end of an interrupt stall (the merged scan must not
 * retire that QTD with its last packet unread), and for every IOC.
 */
static void scenario_late(void)
{
    long behind;
    unsigned seed;

    for (behind = 0; behind < 64; behind++) {
        setup("late-writeback-once-hs", 1, 1, 32, 5, 500 + behind);
        M.resub_max = 10;
        begin(5);
        run(3000 + (behind & 31));
        M.stall_from = M.now;
        M.stall_to = M.now + 20 + behind;
        M.late = 1;
        M.late_left = 1;
        M.late_from = M.stall_to - 40;
        M.late_to = M.stall_to + 40;
        run(9000);
        expect_streaming();
    }
    for (seed = 1; seed <= 12; seed++) {
        setup("late-writeback-all-hs", 1, seed & 4 ? 2 : 1, 32, 5, 600 + seed);
        M.lat_max = 2;
        M.resub_max = 30;
        M.straddle = 3;
        M.init_straddle = 5;
        M.kick_every = seed & 1 ? 11 : 0;
        M.late = seed & 2 ? 1 : 3;
        M.check_idle = M.inc == 1;
        begin(seed & 4 ? 3 : 5);
        run(3000);
        M.stall_from = M.now;
        M.stall_to = M.now + 37 * seed;
        run(9000);
        expect_streaming();
    }
    for (seed = 1; seed <= 6; seed++) {
        setup("late-writeback-all-fs", 0, 1, 16, 3, 700 + seed);
        M.resub_max = 3;
        M.late = seed & 1 ? 1 : 2;
        M.init_straddle = 5;
        begin(3);
        run(2000);
        M.stall_from = M.now;
        M.stall_to = M.now + 9 * seed;
        run(5000);
        expect_streaming();
    }
    puts("ok late-writeback: one late IOC around a stall, and every IOC late (HS, FS)");
}

/* Over a second without service: the 14-bit frame difference wraps */
static void scenario_long_stall(void)
{
    unsigned seed;

    for (seed = 1; seed <= 4; seed++) {
        setup("long-stall-hs", 1, 1, 32, 5, 800 + seed);
        M.resub_max = 20;
        begin(5);
        run(2000);
        M.stall_from = M.now;
        M.stall_to = M.now + 8200 + 1500 * seed;
        run(20000);
        expect_streaming();
    }
    puts("ok long-stall-hs: stream resumes after interrupt stalls over 1 s");
}

/* (d) FS: 64-slot list, one slot per frame */
static void scenario_fs(void)
{
    unsigned seed;
    long p;

    for (seed = 1; seed <= 3; seed++) {
        steady("steady-fs", 0, 1, 16, 3, 41 + seed, seed & 1, seed == 3 ? 5 : 0, 20000);
        if (M.rearms)
            fail("overstep recovery fired without an overstep");
    }
    for (p = -4; p <= 28; p++)
        overstep("overstep-fs", 0, 1, 16, 3, 2, p);
    /*
     * FS isochronous intervals are 2^(bInterval - 1) frames
     * (usbh_get_urb_interval()), so they are powers of two like HS ones.
     * One URB of 8 packets armed covers half the 64-slot list. Past p = 24
     * a re-arm 3..6 frames ahead of the core would pass td_first + 64, so
     * recovery rightly waits for the IOC there.
     */
    for (p = -8; p <= 24; p++)
        overstep("overstep-fs-interval4", 0, 4, 8, 4, 1, p);
    for (p = 0; p < 120; p += 9) {
        steady("coalesced-ioc-fs", 0, 1, 16, 3, 61 + p, 0, 0, 2000);
        M.stall_from = M.now;
        M.stall_to = M.now + 40 + p;
        run(5000);
        expect_streaming();
    }
    puts("ok fs: steady, interval-4 overstep and coalesced interrupts on the 64-slot list");
}

int main(int argc, char **argv)
{
    const char *which = argc > 1 ? argv[1] : "all";
    int all = !strcmp(which, "all");

    if (all || !strcmp(which, "steady"))
        scenario_steady();
    if (all || !strcmp(which, "overstep"))
        scenario_overstep();
    if (all || !strcmp(which, "coalesced"))
        scenario_coalesced();
    if (all || !strcmp(which, "interval"))
        scenario_interval();
    if (all || !strcmp(which, "deep"))
        scenario_deep();
    if (all || !strcmp(which, "partial"))
        scenario_partial();
    if (all || !strcmp(which, "late"))
        scenario_late();
    if (all || !strcmp(which, "long-stall"))
        scenario_long_stall();
    if (all || !strcmp(which, "fs"))
        scenario_fs();
    return 0;
}
"""

SCENARIOS = ("steady", "overstep", "coalesced", "interval", "deep", "partial", "late",
             "long-stall", "fs")


def main():
    source = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else PORT / "hcd_ddma.c"
    with tempfile.TemporaryDirectory(prefix="k230-dwc2-isoc-test-") as directory:
        build = Path(directory)
        (build / "rtconfig.h").write_text(RTCONFIG)
        for name, text in HEADERS.items():
            (build / name).write_text(text)
        (build / "hcd_ddma.c").write_text(source.read_text())
        (build / "test.c").write_text(MODEL)
        cc = [os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Werror", "-O1",
              "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-but-set-variable",
              "-Wno-unused-label", "-Wno-pointer-sign", "-Wno-int-to-pointer-cast",
              "-Wno-pointer-to-int-cast", "-Wno-address-of-packed-member",
              "-fsanitize=undefined,address", "-fno-sanitize-recover=all",
              "-I", str(build), "-I", str(PORT), "-I", str(USB), "-I", str(USB / "common"),
              "-I", str(USB / "core"), "-I", str(USB / "class/hub"),
              "-I", str(RTT / "include"), "-I", str(RTT / "components/drivers/include")]
        binary = build / "test"
        subprocess.run([*cc, str(build / "test.c"), "-o", str(binary)], check=True)
        failed = []
        for scenario in SCENARIOS:
            result = subprocess.run([str(binary), scenario], timeout=300,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            sys.stdout.write(result.stdout)
            if result.returncode:
                failed.append(scenario)
        if failed:
            sys.exit("FAILED: " + ", ".join(failed))
        print("PASS dwc2 isoc DDMA: overstep re-arm, coalesced IOCs, late writebacks, "
              "per-packet status at intervals 1-8, FS interval 4, "
              "deep queues, long stalls, FS list")


if __name__ == "__main__":
    main()

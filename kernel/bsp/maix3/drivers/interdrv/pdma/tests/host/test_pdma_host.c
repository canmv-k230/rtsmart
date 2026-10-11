/*
 * Executes the actual candidate C driver with deterministic RTOS/MMIO shims.
 * No UART, DMA, device, or target firmware is touched. IRQ injection below
 * models terminal delivery and W1C completion at the injection boundary; it
 * does not validate actual register ordering, cache coherence, or SMP races.
 */
#include <stdio.h>
#include "../after/drv_pdma.c"

static pdma_reg_t host_registers;
static int cases;

static void reset_driver(void)
{
    assert(host_allocations == host_frees);
    memset(&pdma_dev, 0, sizeof(pdma_dev));
    memset(&host_registers, 0, sizeof(host_registers));
    memset(host_masks, 0, sizeof(host_masks));
    pdma_dev.reg = &host_registers;
    for (int i = 0; i < PDMA_CH_MAX; ++i)
        pdma_dev.chan[i].irq = 139 + i;
    host_irq_depth = 0;
    host_ticks = 0;
    host_event_hook = NULL;
    host_completion_hook = NULL;
}

static pdma_transfer_cfg_t config_for_length(rt_uint32_t length)
{
    pdma_transfer_cfg_t cfg = {0};
    cfg.device = UART2_TX;
    cfg.src_addr = (void*)(uintptr_t)0x1000;
    cfg.dst_addr = (void*)(uintptr_t)0x2000;
    cfg.length = length;
    cfg.ch_cfg.ch_src_type = TX;
    return cfg;
}

static int request_configured(void)
{
    int chan = rt_dma_chan_request("host_pdma");
    assert(chan >= 0);
    pdma_transfer_cfg_t cfg = config_for_length(32);
    assert(rt_dma_chan_config(chan, &cfg) == 0);
    return chan;
}

static void inject_irq(int chan, uint32_t status)
{
    assert(!pdma_dev.state_lock.held);
    host_registers.dma_int_stat = status << chan;
    host_irq_depth++;
    pdma_irq(pdma_dev.chan[chan].irq, (void*)(uintptr_t)chan);
    host_irq_depth--;
    /* Ordinary host memory cannot implement W1C writes; acknowledge here. */
    host_registers.dma_int_stat = 0;
    assert(!pdma_dev.state_lock.held);
}

struct callback_context { int chan, calls, limit, rearm_error; };
static void chain_callback(void* param)
{
    struct callback_context* ctx = param;
    pdma_completion_snapshot_t snapshot;
    assert(host_irq_depth == 1);
    assert(!pdma_dev.state_lock.held);
    ctx->calls++;
    assert(rt_dma_chan_get_completion_snapshot(ctx->chan, &snapshot) == 0);
    assert(snapshot.generation == pdma_dev.chan[ctx->chan].generation);
    assert(rt_dma_chan_release(ctx->chan) == -RT_EBUSY);
    assert(rt_dma_chan_callback(ctx->chan, NULL, NULL) == -RT_EBUSY);
    if (ctx->calls < ctx->limit) {
        pdma_transfer_cfg_t cfg = config_for_length(32);
        ctx->rearm_error = rt_dma_chan_config(ctx->chan, &cfg);
        if (!ctx->rearm_error)
            ctx->rearm_error = rt_dma_chan_start(ctx->chan);
    }
}

static void test_callback_only_1000(void)
{
    reset_driver();
    int chan = request_configured();
    struct callback_context ctx = {chan, 0, 1000, 0};
    assert(rt_dma_chan_callback(chan, chain_callback, &ctx) == 0);
    assert(pdma_dev.chan[chan].completion_mode == PDMA_COMPLETION_CALLBACK_ONLY);
    assert(rt_dma_chan_start(chan) == 0);
    assert(rt_dma_chan_done(chan, 0) == -RT_EINVAL);
    int allocations = host_allocations;
    for (int i = 0; i < 1000; ++i) {
        inject_irq(chan, PDONE_INT);
        assert(ctx.calls == i + 1 && ctx.rearm_error == 0);
        assert(pdma_dev.chan[chan].completion_count == 0);
    }
    assert(host_allocations == allocations);
    assert(!pdma_dev.chan[chan].running);
    assert(rt_dma_chan_done(chan, 0) == -RT_EINVAL);
    assert(rt_dma_chan_release(chan) == 0);
    cases++;
}

static void test_hybrid_late_done(void)
{
    reset_driver();
    int chan = request_configured();
    struct callback_context ctx = {chan, 0, 2, 0};
    assert(rt_dma_chan_callback_ex(chan, chain_callback, &ctx, PDMA_COMPLETION_HYBRID) == 0);
    assert(rt_dma_chan_start(chan) == 0);
    inject_irq(chan, PDONE_INT);
    assert(pdma_dev.chan[chan].running && ctx.rearm_error == 0);
    inject_irq(chan, PDONE_INT);
    assert(pdma_dev.chan[chan].completion_count == 2);
    assert(rt_dma_chan_callback(chan, NULL, NULL) == -RT_EBUSY);
    assert(rt_dma_chan_done(chan, 0) == 0);
    assert(pdma_dev.chan[chan].completion_count == 1);
    assert(rt_dma_chan_done(chan, 0) == 0);
    assert(pdma_dev.chan[chan].completion_count == 0);
    assert(rt_dma_chan_callback(chan, NULL, NULL) == 0);
    assert(pdma_dev.chan[chan].completion_mode == PDMA_COMPLETION_WAIT);
    assert(rt_dma_chan_release(chan) == 0);
    cases++;
}

static void passive_callback(void* param) { (*(int*)param)++; }

static void test_queue_modes(void)
{
    for (int mode = PDMA_COMPLETION_WAIT; mode <= PDMA_COMPLETION_HYBRID; mode += 2) {
        reset_driver();
        int chan = request_configured(), callbacks = 0;
        assert(rt_dma_chan_callback_ex(chan, mode ? passive_callback : NULL,
               &callbacks, (pdma_completion_mode_t)mode) == 0);
        for (int i = 0; i < 8; ++i) {
            assert(rt_dma_chan_start(chan) == 0);
            inject_irq(chan, i == 2 ? PTOUT_INT : PDONE_INT);
        }
        assert(callbacks == (mode ? 8 : 0));
        assert(rt_dma_chan_start(chan) == -RT_EBUSY);
        assert(rt_dma_chan_done(chan, 0) == 0);
        assert(rt_dma_chan_start(chan) == 0);
        /* Pop older results while a newer generation remains running. */
        for (int i = 1; i < 8; ++i) {
            assert(rt_dma_chan_done(chan, 0) == (i == 2 ? -RT_ETIMEOUT : 0));
            assert(pdma_dev.chan[chan].running);
            assert(host_registers.pdma_ch_reg[chan].ch_ctl == 1);
        }
        inject_irq(chan, PDONE_INT);
        assert(rt_dma_chan_done(chan, 0) == 0);
        assert(rt_dma_chan_release(chan) == 0);
        cases++;
    }
}

static int hook_chan;
static void timeout_irq_hook(void) { inject_irq(hook_chan, PTOUT_INT); }
static void done_irq_hook(void) { inject_irq(hook_chan, PDONE_INT); }

static void test_callback_mode_migration_events(void)
{
    for (int mode = PDMA_COMPLETION_WAIT; mode <= PDMA_COMPLETION_HYBRID; mode += 2) {
        uint32_t old_status[] = {PDONE_INT, PITEM_INT, PPAUSE_INT};
        for (unsigned i = 0; i < sizeof(old_status) / sizeof(*old_status); ++i) {
            reset_driver();
            int chan = request_configured(), callbacks = 0;
            assert(rt_dma_chan_callback(chan, passive_callback, &callbacks) == 0);
            assert(rt_dma_chan_start(chan) == 0);
            inject_irq(chan, old_status[i]);
            if (old_status[i] != PDONE_INT) inject_irq(chan, PDONE_INT);
            assert(callbacks == 1 && !pdma_dev.chan[chan].running);
            assert(rt_dma_chan_callback_ex(chan, mode ? passive_callback : NULL,
                   &callbacks, (pdma_completion_mode_t)mode) == 0);
            assert(rt_dma_chan_start(chan) == 0);
            hook_chan = chan;
            host_event_hook = done_irq_hook;
            /* Old PITEM/PAUSE must not return 1/2 for this new generation. */
            assert(rt_dma_chan_done(chan, 20) == 0);
            assert(host_event_hook == NULL);
            assert(pdma_dev.chan[chan].completion_snapshot.generation == 2);
            assert(!pdma_dev.chan[chan].running);
            assert(callbacks == (mode ? 2 : 1));
            assert(rt_dma_chan_release(chan) == 0);
            cases++;
        }
    }
}

static void test_timeout_rearm(void)
{
    reset_driver();
    int chan = request_configured();
    struct callback_context ctx = {chan, 0, 2, 0};
    assert(rt_dma_chan_callback_ex(chan, chain_callback, &ctx, PDMA_COMPLETION_HYBRID) == 0);
    assert(rt_dma_chan_start(chan) == 0);
    hook_chan = chan;
    host_event_hook = timeout_irq_hook;
    assert(rt_dma_chan_done(chan, 20) == -RT_ETIMEOUT);
    assert(ctx.rearm_error == 0 && pdma_dev.chan[chan].generation == 2);
    assert(pdma_dev.chan[chan].running);
    assert(host_registers.pdma_ch_reg[chan].ch_ctl == 1);
    inject_irq(chan, PDONE_INT);
    assert(rt_dma_chan_done(chan, 0) == 0);
    assert(rt_dma_chan_release(chan) == 0);
    cases++;
}

static void test_soft_timeout_and_wrap(void)
{
    reset_driver();
    int chan = request_configured();
    assert(rt_dma_chan_start(chan) == 0);
    assert(rt_dma_chan_done(chan, 0) == -RT_ETIMEOUT);
    assert(!pdma_dev.chan[chan].running);
    assert(host_registers.pdma_ch_reg[chan].ch_ctl == 2);
    assert(pdma_dev.chan[chan].completion_snapshot.hardware_timeouts == 0);
    pdma_dev.chan[chan].generation = UINT32_MAX;
    assert(rt_dma_chan_start(chan) == 0);
    assert(pdma_dev.chan[chan].generation == 1);
    inject_irq(chan, PDONE_INT);
    assert(rt_dma_chan_done(chan, 0) == 0);
    assert(rt_dma_chan_release(chan) == 0);
    cases++;
}

static void test_modes_and_busy_guards(void)
{
    reset_driver();
    int chan = request_configured(), callbacks = 0;
    assert(rt_dma_chan_callback_ex(chan, NULL, NULL, (pdma_completion_mode_t)-1) == -RT_EINVAL);
    assert(rt_dma_chan_callback_ex(chan, NULL, NULL, (pdma_completion_mode_t)3) == -RT_EINVAL);
    assert(rt_dma_chan_callback_ex(chan, passive_callback, &callbacks, PDMA_COMPLETION_WAIT) == -RT_EINVAL);
    assert(rt_dma_chan_callback_ex(chan, NULL, NULL, PDMA_COMPLETION_CALLBACK_ONLY) == -RT_EINVAL);
    assert(rt_dma_chan_callback_ex(chan, NULL, NULL, PDMA_COMPLETION_HYBRID) == -RT_EINVAL);
    assert(rt_dma_chan_callback_ex(-1, NULL, NULL, PDMA_COMPLETION_WAIT) == -RT_EINVAL);
    assert(rt_dma_chan_callback_ex(8, NULL, NULL, PDMA_COMPLETION_WAIT) == -RT_EINVAL);
    assert(rt_dma_chan_start(chan) == 0);
    assert(rt_dma_chan_callback(chan, passive_callback, &callbacks) == -RT_EBUSY);
    assert(rt_dma_chan_stop(chan) == 0);
    pdma_dev.chan[chan].waiter_active = RT_TRUE;
    assert(rt_dma_chan_callback(chan, passive_callback, &callbacks) == -RT_EBUSY);
    assert(rt_dma_chan_release(chan) == -RT_EBUSY);
    pdma_dev.chan[chan].waiter_active = RT_FALSE;
    pdma_dev.chan[chan].callback_count = 1;
    assert(rt_dma_chan_callback(chan, passive_callback, &callbacks) == -RT_EBUSY);
    pdma_dev.chan[chan].callback_count = 0;
    assert(pdma_dev.chan[chan].completion_mode == PDMA_COMPLETION_WAIT);
    assert(rt_dma_chan_release(chan) == 0);
    assert(rt_dma_chan_callback(chan, NULL, NULL) == -RT_EINVAL);
    cases++;
}

static void test_snapshot_retention(void)
{
    reset_driver();
    int chan = request_configured(), callbacks = 0;
    pdma_completion_snapshot_t snapshot;
    assert(rt_dma_chan_get_completion_snapshot(chan, &snapshot) == 0);
    assert(snapshot.generation == 0 && snapshot.raw_irq_status == 0 && snapshot.hardware_timeouts == 0);
    assert(rt_dma_chan_callback(chan, passive_callback, &callbacks) == 0);
    pdma_dev.chan[chan].completion_snapshot.hardware_timeouts = UINT32_MAX - 1;
    for (int i = 0; i < 3; ++i) {
        assert(rt_dma_chan_start(chan) == 0);
        inject_irq(chan, i == 2 ? PDONE_INT : PTOUT_INT);
    }
    assert(rt_dma_chan_stop(chan) == 0);
    assert(rt_dma_chan_callback(chan, NULL, NULL) == 0);
    assert(rt_dma_chan_get_completion_snapshot(chan, &snapshot) == 0);
    assert(snapshot.generation == 3 && snapshot.raw_irq_status == (PDONE_INT << chan));
    assert(snapshot.hardware_timeouts == UINT32_MAX);
    assert(rt_dma_chan_get_completion_snapshot(chan, NULL) == -RT_EINVAL);
    assert(rt_dma_chan_get_completion_snapshot(-1, &snapshot) == -RT_EINVAL);
    assert(rt_dma_chan_release(chan) == 0);
    assert(rt_dma_chan_get_completion_snapshot(chan, &snapshot) == -RT_EINVAL);
    assert(rt_dma_chan_request("new_owner") == chan);
    assert(rt_dma_chan_get_completion_snapshot(chan, &snapshot) == 0);
    assert(snapshot.generation == 0 && snapshot.hardware_timeouts == 0);
    assert(rt_dma_chan_release(chan) == 0);
    cases++;
}

static void test_descriptors(void)
{
    reset_driver();
    int chan = request_configured();
    uint32_t sizes[] = {1, PDMA_MAX_LINE_SIZE, PDMA_MAX_LINE_SIZE + 1,
        2U * PDMA_MAX_LINE_SIZE + 1U, UINT32_MAX};
    int allocations = host_allocations;
    for (unsigned tx = 0; tx < 2; ++tx) {
        for (size_t i = 0; i < sizeof(sizes) / sizeof(*sizes); ++i) {
            pdma_transfer_cfg_t cfg = config_for_length(sizes[i]);
            cfg.ch_cfg.ch_src_type = tx ? TX : RX;
            assert(rt_dma_chan_config(chan, &cfg) == 0);
            uint32_t entries = (sizes[i] - 1U) / PDMA_MAX_LINE_SIZE + 1U;
            uint64_t total = 0;
            assert(entries <= PDMA_LLT_MAX_ENTRIES);
            for (uint32_t j = 0; j < entries; ++j) {
                pdma_llt_t* d = &pdma_dev.chan[chan].list[j];
                total += d->line_size;
                assert(d->line_size && d->node_intr == 0 && d->pause == 0);
                assert(d->next_llt_addr == (j + 1 == entries ? 0 :
                    (uint32_t)(uintptr_t)&pdma_dev.chan[chan].list[j + 1]));
                assert(d->src_addr == (uint32_t)(0x1000ULL + (tx ? (uint64_t)j * PDMA_MAX_LINE_SIZE : 0)));
                assert(d->dst_addr == (uint32_t)(0x2000ULL + (tx ? 0 : (uint64_t)j * PDMA_MAX_LINE_SIZE)));
            }
            assert(total == sizes[i]);
        }
    }
    pdma_transfer_cfg_t zero = config_for_length(0);
    assert(rt_dma_chan_config(chan, &zero) == -RT_EINVAL);
    assert(rt_dma_chan_config(chan, NULL) == -RT_EINVAL);
    assert(host_allocations == allocations);
    assert(rt_dma_chan_release(chan) == 0);
    cases++;
}

static void test_all_channels(void)
{
    reset_driver();
    for (int chan = 0; chan < PDMA_CH_MAX; ++chan) assert(request_configured() == chan);
    assert(rt_dma_chan_request("exhausted") == -RT_EBUSY);
    for (int chan = 0; chan < PDMA_CH_MAX; ++chan) {
        assert(rt_dma_chan_start(chan) == 0);
        inject_irq(chan, chan & 1 ? PTOUT_INT : PDONE_INT);
    }
    for (int chan = PDMA_CH_MAX - 1; chan >= 0; --chan) {
        assert(rt_dma_chan_done(chan, 0) == (chan & 1 ? -RT_ETIMEOUT : 0));
        assert(rt_dma_chan_release(chan) == 0);
    }
    cases++;
}

static void test_nonterminal_irq(void)
{
    reset_driver();
    int chan = request_configured(), callbacks = 0;
    assert(rt_dma_chan_callback(chan, passive_callback, &callbacks) == 0);
    assert(rt_dma_chan_start(chan) == 0);
    inject_irq(chan, PITEM_INT);
    inject_irq(chan, PPAUSE_INT);
    assert(callbacks == 0 && pdma_dev.chan[chan].running);
    assert(pdma_dev.chan[chan].completion_snapshot.generation == 0);
    assert(pdma_dev.chan[chan].completion_count == 0);
    inject_irq(chan, PDONE_INT | PTOUT_INT);
    assert(callbacks == 1);
    assert(pdma_dev.chan[chan].completion_snapshot.raw_irq_status ==
           ((PDONE_INT | PTOUT_INT) << chan));
    assert(pdma_dev.chan[chan].completion_snapshot.hardware_timeouts == 1);
    assert(rt_dma_chan_release(chan) == 0);
    cases++;
}

static void complete_inflight_callback(void)
{
    assert(pdma_dev.lifecycle_lock.held);
    assert(!pdma_dev.state_lock.held);
    assert(!pdma_chan_active(hook_chan));
    pdma_dev.chan[hook_chan].callback_count--;
    rt_completion_done(&pdma_dev.chan[hook_chan].callbacks_done);
}
static void test_release_waits_for_callback(void)
{
    reset_driver();
    int chan = request_configured();
    pdma_dev.chan[chan].callback_count = 1;
    hook_chan = chan;
    host_completion_hook = complete_inflight_callback;
    assert(rt_dma_chan_release(chan) == 0);
    assert(!pdma_dev.chan[chan].callback_count);
    assert(rt_dma_chan_request("reused") == chan);
    assert(rt_dma_chan_release(chan) == 0);
    cases++;
}

int main(void)
{
#define RUN_TEST(test) do { fprintf(stderr, "RUN %s\n", #test); test(); } while (0)
    RUN_TEST(test_callback_only_1000);
    RUN_TEST(test_hybrid_late_done);
    RUN_TEST(test_queue_modes);
    RUN_TEST(test_callback_mode_migration_events);
    RUN_TEST(test_timeout_rearm);
    RUN_TEST(test_soft_timeout_and_wrap);
    RUN_TEST(test_modes_and_busy_guards);
    RUN_TEST(test_snapshot_retention);
    RUN_TEST(test_descriptors);
    RUN_TEST(test_all_channels);
    RUN_TEST(test_nonterminal_irq);
    RUN_TEST(test_release_waits_for_callback);
    assert(host_allocations == host_frees);
    printf("PDMA_HOST_RESULT cases=%d passed=%d actual_candidate_c=true\n", cases, cases);
    return 0;
}

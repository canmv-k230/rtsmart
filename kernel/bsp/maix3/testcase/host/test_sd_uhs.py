#!/usr/bin/env python3
"""Exercise SD UHS-I initialization in RT-Thread's production sd.c.

sd.c and the power, clock, timing and signal-voltage helpers of mmcsd_core.c
are compiled unchanged against a scripted card and host. Scenarios cover the
CMD11 switch and its failures, bus speed mode selection, tuning fallback, and
the unchanged 3.3 V path for hosts and cards without UHS-I.
Run with python3 -B kernel/bsp/maix3/testcase/host/test_sd_uhs.py.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

RTT = Path(__file__).resolve().parents[4] / "rt-thread"
DRIVERS = RTT / "components/drivers"

RTCONFIG = r"""
#pragma once
#define RT_NAME_MAX 20
#define RT_ALIGN_SIZE 8
#define RT_THREAD_PRIORITY_MAX 32
#define RT_TICK_PER_SECOND 1000
#define RT_USING_DEVICE
#define RT_USING_MUTEX
#define RT_USING_EVENT
#define RT_USING_MAILBOX
#define RT_USING_SEMAPHORE
#define RT_USING_HEAP
#define RT_USING_MUSL
#define ARCH_CPU_64BIT
"""

CORE_FUNCTIONS = ("mmcsd_set_iocfg", "mmcsd_set_clock", "mmcsd_set_bus_width",
                  "mmcsd_set_timing", "mmcsd_set_data_timeout", "mmcsd_select_voltage",
                  "mmcsd_power_up", "mmcsd_power_off", "mmcsd_power_cycle",
                  "mmcsd_set_signal_voltage", "mmcsd_excute_tuning")

MODEL = r"""
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rtthread.h>
#include <drivers/mmcsd_core.h>
#include <drivers/sd.h>

void mmcsd_power_up(struct rt_mmcsd_host *host);

/* ---- scenario ---- */
static struct {
    rt_bool_t card_uhs;         /* card answers S18A */
    rt_uint8_t card_modes;      /* CMD6 group 1 support bits */
    rt_bool_t no_busy;          /* card never drives DAT low after CMD11 */
    rt_bool_t stuck_busy;       /* card keeps DAT low after the switch */
    rt_bool_t fail_sdr104_tune;
    rt_bool_t card_left_1v8;    /* a warm reset left the card at 1.8 V */
    unsigned fail_cmd6;         /* this CMD6 (1-based) fails; 0: none */
} sc;

/* ---- card ---- */
static struct {
    rt_bool_t powered, v1p8, busy, selected;
    rt_uint8_t func;
} card;
static unsigned cmd11_count, acmd41_s18r, power_offs, tunings, cmd6_sets;
static unsigned bad_signal_cmds, cmd6_calls;
static rt_bool_t app_cmd;

/* ---- host ---- */
static struct rt_mmcsd_host host;
static unsigned char host_signal = MMCSD_SIGNAL_VOLTAGE_330;
static unsigned char last_power = MMCSD_POWER_OFF;

static void card_power(rt_bool_t on)
{
    if (!on && card.powered) {
        power_offs++;
        memset(&card, 0, sizeof(card));
    }
    card.powered = on;
}

static void set_iocfg(struct rt_mmcsd_host *h, struct rt_mmcsd_io_cfg *io)
{
    if (io->power_mode != last_power) {
        card_power(io->power_mode != MMCSD_POWER_OFF);
        last_power = io->power_mode;
    }
    /* The card releases DAT[3:0] once the clock restarts at 1.8 V. */
    if (card.busy && io->clock && host_signal == MMCSD_SIGNAL_VOLTAGE_180 && !sc.stuck_busy) {
        card.busy = RT_FALSE;
        card.v1p8 = RT_TRUE;
    }
}

static rt_int32_t switch_signal(struct rt_mmcsd_host *h, struct rt_mmcsd_io_cfg *io)
{
    /* The clock must be stopped while the signaling changes. */
    if (io->signal_voltage == MMCSD_SIGNAL_VOLTAGE_180)
        assert(h->io_cfg.clock == 0);
    host_signal = io->signal_voltage;
    return RT_EOK;
}

static rt_int32_t card_busy(struct rt_mmcsd_host *h) { return card.busy; }

static rt_int32_t execute_tuning(struct rt_mmcsd_host *h, rt_int32_t opcode)
{
    assert(opcode == SEND_TUNING_BLOCK);
    tunings++;
    if (h->io_cfg.timing == MMCSD_TIMING_UHS_SDR104 && sc.fail_sdr104_tune)
        return -RT_ERROR;
    return RT_EOK;
}

static struct rt_mmcsd_host_ops ops = {
    .set_iocfg = set_iocfg,
    .execute_tuning = execute_tuning,
    .switch_signal_voltage = switch_signal,
    .card_busy = card_busy,
};

/* Commands reach the card only when both sides use the same signaling. */
static rt_bool_t signal_ok(void)
{
    if (card.v1p8 != (host_signal == MMCSD_SIGNAL_VOLTAGE_180)) {
        bad_signal_cmds++;
        return RT_FALSE;
    }
    return RT_TRUE;
}

static void card_cmd(struct rt_mmcsd_cmd *cmd, struct rt_mmcsd_data *data)
{
    rt_bool_t app = app_cmd;

    app_cmd = RT_FALSE;
    cmd->err = 0;
    memset(cmd->resp, 0, sizeof(cmd->resp));
    if (!card.powered || card.busy || !signal_ok()) {
        cmd->err = -RT_ETIMEOUT;
        return;
    }
    switch (cmd->cmd_code) {
    case SD_SEND_IF_COND:
        cmd->resp[0] = cmd->arg & 0xFFF;
        break;
    case APP_CMD:
        app_cmd = RT_TRUE;
        cmd->resp[0] = R1_APP_CMD;
        break;
    case SD_SEND_RELATIVE_ADDR:
        cmd->resp[0] = 0x1234 << 16;
        break;
    case SD_SWITCH_VOLTAGE:
        cmd11_count++;
        assert(!card.v1p8);
        card.busy = !sc.no_busy;
        break;
    default:
        if (app && cmd->cmd_code == SD_APP_OP_COND) {
            if (cmd->arg & SD_OCR_S18R)
                acmd41_s18r++;
            cmd->resp[0] = CARD_BUSY | SD_OCR_CCS |
                (sc.card_uhs && (cmd->arg & SD_OCR_S18R) && !card.v1p8 ? SD_OCR_S18R : 0);
        } else if (app && cmd->cmd_code == SD_APP_SEND_SCR) {
            rt_uint8_t *b = (rt_uint8_t *)data->buf;
            memset(b, 0, 8);
            b[0] = 0x02;    /* SD_SPEC: version 2.00 or later */
            b[1] = 0x05;    /* 1- and 4-bit bus */
        } else if (app && cmd->cmd_code == SD_APP_SD_STATUS) {
            memset(data->buf, 0, 64);
        } else if (app && cmd->cmd_code == SD_APP_SET_BUS_WIDTH) {
            /* accepted */
        } else if (cmd->cmd_code == SD_SWITCH) {
            rt_uint8_t *b = (rt_uint8_t *)data->buf;
            rt_uint8_t func = cmd->arg & 0xF;
            if (++cmd6_calls == sc.fail_cmd6) {
                cmd->err = -RT_EIO;
                return;
            }
            memset(b, 0, 64);
            b[13] = sc.card_modes;
            if (cmd->arg & 0x80000000) {
                cmd6_sets++;
                /* UHS modes need 1.8 V signaling. */
                if ((sc.card_modes & (1U << func)) && (func <= 1 || card.v1p8))
                    card.func = func;
                else
                    func = 0xF;
            }
            b[16] = func == 0xF && !(cmd->arg & 0x80000000) ? card.func : func;
        } else {
            assert(!"unexpected command");
        }
    }
}

void mmcsd_send_request(struct rt_mmcsd_host *h, struct rt_mmcsd_req *req)
{
    card_cmd(req->cmd, req->data);
    if (req->data)
        req->data->err = req->cmd->err;
}

rt_int32_t mmcsd_send_cmd(struct rt_mmcsd_host *h, struct rt_mmcsd_cmd *cmd, int retries)
{
    card_cmd(cmd, RT_NULL);
    return cmd->err;
}

rt_int32_t mmcsd_go_idle(struct rt_mmcsd_host *h) { card.func = 0; return RT_EOK; }
rt_int32_t mmcsd_all_get_cid(struct rt_mmcsd_host *h, rt_uint32_t *cid)
{
    memset(cid, 0, 16);
    return signal_ok() ? RT_EOK : -RT_ETIMEOUT;
}
rt_int32_t mmcsd_get_cid(struct rt_mmcsd_host *h, rt_uint32_t *cid) { return -RT_ENOSYS; }
rt_int32_t mmcsd_get_csd(struct rt_mmcsd_card *c, rt_uint32_t *csd)
{
    memset(csd, 0, 16);
    csd[0] = 0x40000000 | 0x32;     /* CSD 2.0, 25 MHz */
    csd[2] = 0x1000U << 16;         /* C_SIZE */
    return RT_EOK;
}
rt_int32_t mmcsd_select_card(struct rt_mmcsd_card *c) { card.selected = RT_TRUE; return RT_EOK; }
rt_int32_t mmcsd_spi_use_crc(struct rt_mmcsd_host *h, rt_int32_t use_crc) { return RT_EOK; }
rt_int32_t mmcsd_spi_read_ocr(struct rt_mmcsd_host *h, rt_int32_t hc, rt_uint32_t *ocr) { return -RT_ENOSYS; }
void mmcsd_set_bus_mode(struct rt_mmcsd_host *h, rt_uint32_t mode) { h->io_cfg.bus_mode = mode; }
void mmcsd_host_lock(struct rt_mmcsd_host *h) {}
void mmcsd_host_unlock(struct rt_mmcsd_host *h) {}
rt_int32_t rt_mmcsd_blk_probe(struct rt_mmcsd_card *c) { return RT_EOK; }
rt_err_t rt_mmcsd_blk_remove(struct rt_mmcsd_card *c) { return RT_EOK; }

void *rt_malloc(rt_size_t size) { return malloc(size); }
void rt_free(void *p) { free(p); }
void *rt_memset(void *s, int c, rt_ubase_t n) { return memset(s, c, n); }
void *rt_memcpy(void *d, const void *s, rt_ubase_t n) { return memcpy(d, s, n); }

rt_err_t rt_thread_mdelay(rt_int32_t ms) { return RT_EOK; }
rt_err_t rt_thread_delay(rt_tick_t tick) { return RT_EOK; }
rt_tick_t rt_tick_from_millisecond(rt_int32_t ms) { return ms; }
rt_tick_t rt_tick_get(void) { return 0; }
int __rt_ffs(int value) { return __builtin_ffs(value); }
"""

CASES = r"""
static void setup(rt_uint32_t flags)
{
    memset(&sc, 0, sizeof(sc));
    memset(&card, 0, sizeof(card));
    free(host.card);
    memset(&host, 0, sizeof(host));
    cmd11_count = acmd41_s18r = power_offs = tunings = cmd6_sets = bad_signal_cmds = 0;
    cmd6_calls = 0;
    app_cmd = RT_FALSE;
    host_signal = MMCSD_SIGNAL_VOLTAGE_330;
    last_power = MMCSD_POWER_OFF;
    strcpy(host.name, "sd1");
    host.ops = &ops;
    host.freq_min = 400000;
    host.freq_max = 200000000;
    host.valid_ocr = VDD_32_33 | VDD_33_34;
    host.flags = MMCSD_BUSWIDTH_4 | MMCSD_SUP_HIGHSPEED | flags;
    sc.card_uhs = RT_TRUE;
    sc.card_modes = 0x1F;   /* SDR12, SDR25, SDR50, SDR104, DDR50 */
}

static void power_up(void)
{
    mmcsd_power_up(&host);
    if (sc.card_left_1v8)
        card.v1p8 = RT_TRUE;
}

static void run(void)
{
    power_up();
    assert(init_sd(&host, VDD_32_33 | VDD_33_34) == RT_EOK);
    assert(host.card && bad_signal_cmds == 0);
}

int main(void)
{
    /* Full UHS-I host and card: SDR104, tuned once. */
    setup(MMCSD_SUP_UHS);
    run();
    assert(acmd41_s18r == 1 && cmd11_count == 1 && power_offs == 0);
    assert(host_signal == MMCSD_SIGNAL_VOLTAGE_180 && card.v1p8);
    assert(host.io_cfg.timing == MMCSD_TIMING_UHS_SDR104 && card.func == 3);
    assert(host.io_cfg.clock == 200000000 && tunings == 1);
    assert(host.io_cfg.bus_width == MMCSD_BUS_WIDTH_4);

    /* SDR104 tuning fails: SDR50 without tuning on this host. */
    setup(MMCSD_SUP_UHS);
    sc.fail_sdr104_tune = RT_TRUE;
    run();
    assert(host.io_cfg.timing == MMCSD_TIMING_UHS_SDR50 && card.func == 2);
    assert(host.io_cfg.clock == 100000000 && tunings == 1);
    /* ... and with tuning where the host requires it for SDR50. */
    setup(MMCSD_SUP_UHS | MMCSD_SDR50_TUNING);
    sc.fail_sdr104_tune = RT_TRUE;
    run();
    assert(host.io_cfg.timing == MMCSD_TIMING_UHS_SDR50 && tunings == 2);

    /* SDIO1-like host: DDR50 only, no tuning, clock bounded by the host. */
    setup(MMCSD_SUP_DDR50);
    host.freq_max = 100000000;
    run();
    assert(host.io_cfg.timing == MMCSD_TIMING_UHS_DDR50 && card.func == 4);
    assert(host.io_cfg.clock == 50000000 && tunings == 0);

    /* A card supporting only SDR25 at 1.8 V. */
    setup(MMCSD_SUP_UHS);
    sc.card_modes = 0x03;
    run();
    assert(host.io_cfg.timing == MMCSD_TIMING_UHS_SDR25 && card.func == 1);

    /* Card without UHS-I: no CMD11, unchanged high speed path at 3.3 V. */
    setup(MMCSD_SUP_UHS);
    sc.card_uhs = RT_FALSE;
    run();
    assert(acmd41_s18r == 1 && cmd11_count == 0);
    assert(host_signal == MMCSD_SIGNAL_VOLTAGE_330 && (host.card->flags & CARD_FLAG_HIGHSPEED));
    assert(host.io_cfg.timing == MMCSD_TIMING_LEGACY && host.io_cfg.clock == 50000000);

    /* Host without UHS-I: S18R is never requested; behavior as before. */
    setup(0);
    run();
    assert(acmd41_s18r == 0 && cmd11_count == 0 && host_signal == MMCSD_SIGNAL_VOLTAGE_330);
    assert(host.card->flags & CARD_FLAG_HIGHSPEED);

    /* The card never drives DAT low after CMD11: power cycle, retry at 3.3 V. */
    setup(MMCSD_SUP_UHS);
    sc.no_busy = RT_TRUE;
    run();
    assert(cmd11_count == 1 && power_offs == 1 && acmd41_s18r == 1);
    assert(host_signal == MMCSD_SIGNAL_VOLTAGE_330 && !card.v1p8);
    assert(host.card->flags & CARD_FLAG_HIGHSPEED);

    /* The card keeps DAT low after the switch: the same fallback. */
    setup(MMCSD_SUP_UHS);
    sc.stuck_busy = RT_TRUE;
    run();
    assert(cmd11_count == 1 && power_offs == 1);
    assert(host_signal == MMCSD_SIGNAL_VOLTAGE_330 && !card.v1p8);

    /* CMD6 fails at 1.8 V: power cycle, retry at 3.3 V. */
    setup(MMCSD_SUP_UHS);
    sc.fail_cmd6 = 1;
    run();
    assert(cmd11_count == 1 && power_offs == 1 && acmd41_s18r == 1);
    assert(host_signal == MMCSD_SIGNAL_VOLTAGE_330 && !card.v1p8);
    assert(host.card->flags & CARD_FLAG_HIGHSPEED);

    /* SDR104 tuning fails and the CMD6 re-read after it fails too: the
     * timing set for the failed mode does not survive the fallback. */
    setup(MMCSD_SUP_UHS);
    sc.fail_sdr104_tune = RT_TRUE;
    sc.fail_cmd6 = 3;
    run();
    assert(cmd11_count == 1 && power_offs == 1 && tunings == 1);
    assert(host_signal == MMCSD_SIGNAL_VOLTAGE_330 && !card.v1p8);
    assert(host.io_cfg.timing == MMCSD_TIMING_LEGACY && host.io_cfg.clock == 50000000);

    /* A warm reset left the card at 1.8 V: power-up restores the host to
     * 3.3 V; without a power cycle the card cannot be reached. */
    setup(MMCSD_SUP_UHS);
    host_signal = MMCSD_SIGNAL_VOLTAGE_180;
    sc.card_left_1v8 = RT_TRUE;
    power_up();
    assert(host_signal == MMCSD_SIGNAL_VOLTAGE_330);
    assert(init_sd(&host, VDD_32_33 | VDD_33_34) != RT_EOK);
    /* A card power cycle, as the host driver's card supply provides, recovers it. */
    sc.card_left_1v8 = RT_FALSE;
    mmcsd_power_cycle(&host);
    bad_signal_cmds = 0;
    assert(init_sd(&host, VDD_32_33 | VDD_33_34) == RT_EOK && bad_signal_cmds == 0);
    assert(host.io_cfg.timing == MMCSD_TIMING_UHS_SDR104);

    free(host.card);
    puts("PASS SD UHS-I: switch, fallbacks, mode selection, tuning, 3.3 V paths");
    return 0;
}
"""


def function(source, name):
    match = re.search(r"^[\w *]+\b" + re.escape(name) + r"\([^;]+?\n\{", source, re.M)
    assert match, name
    end = source.index("\n}", match.end()) + 2
    return source[match.start():end] + "\n"


def main():
    with tempfile.TemporaryDirectory(prefix="k230-sd-uhs-test-") as directory:
        build = Path(directory)
        (build / "rtconfig.h").write_text(RTCONFIG)
        core = (DRIVERS / "sdio/mmcsd_core.c").read_text()
        subset = "".join(function(core, name) for name in CORE_FUNCTIONS)
        # The scenarios power the slot up as the detect thread does.
        subset = subset.replace("static void mmcsd_power_up(", "void mmcsd_power_up(")
        (build / "core.c").write_text(
            '#include <rtthread.h>\n#include <drivers/mmcsd_core.h>\n'
            '#define DBG_TAG "SDIO"\n#define DBG_LVL DBG_WARNING\n#include <rtdbg.h>\n'
            + subset)
        (build / "test.c").write_text(MODEL + CASES)
        cc = [os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Werror",
              "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-but-set-variable",
              "-Wno-pointer-sign", "-Wno-int-conversion", "-Wno-implicit-function-declaration",
              "-Wno-builtin-declaration-mismatch", "-fsanitize=undefined,address",
              "-fno-sanitize-recover=all",
              "-I", str(build), "-I", str(RTT / "include"), "-I", str(DRIVERS / "include")]
        binary = build / "test"
        subprocess.run([*cc, str(build / "test.c"), str(build / "core.c"),
                        str(DRIVERS / "sdio/sd.c"), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    main()

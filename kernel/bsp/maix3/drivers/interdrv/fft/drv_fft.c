/* Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "drv_fft.h"

#include <board.h>
#include <ioremap.h>
#include <lwp_pid.h>
#include <lwp_user_mm.h>
#include <rthw.h>

#include "cache.h"
#include "io.h"
#include "riscv_io.h"

#ifdef RT_USING_POSIX
#include <dfs_poll.h>
#include <dfs_posix.h>
#include <posix_termios.h>
#endif

#include "k_gsdma_comm.h"

// #define DBG_LVL DBG_LOG
#define DBG_LVL DBG_WARNING

#define DBG_COLOR
#define DBG_TAG "fft"
#include <rtdbg.h>

#define FFT_IRQN               (16 + 174)
#define FFT_FIFO_REG_ADDR      (FFT_BASE_ADDR + 0x40)
#define FFT_DEFAULT_TIMEOUT_MS 1000U

#define FFT_EVENT_DONE        (1U << 0)
#define FFT_EVENT_ERR_WRITE   (1U << 1)
#define FFT_EVENT_ERR_READ    (1U << 2)
#define FFT_EVENT_ERR_TIMEOUT (1U << 3)

typedef struct {
    volatile k_fft_cfg_reg_st cfg;
    volatile uint64_t         rsv0;
    volatile uint64_t         enable;
    volatile uint64_t         rsv2;
    volatile uint64_t         int_clr;
    volatile uint64_t         rsv4;
    volatile uint64_t         int_org;
    volatile uint64_t         rsv6;
    volatile uint64_t         fft_inter_fifo;
    volatile uint64_t         rsv10;
    volatile uint64_t         intr_num;
    volatile uint64_t         rsv7;
    volatile uint64_t         debug_0;
    volatile uint64_t         rsv8;
    volatile uint64_t         debug_1;
    volatile uint64_t         rsv9;
} __attribute__((packed)) fft_reg_st;

typedef struct {
    struct rt_device     parent;
    struct rt_mutex      lock;
    struct rt_event      event;
    volatile fft_reg_st* reg;
    rt_uint32_t          last_event;
    rt_bool_t            inited;
} fft_device_t;

static fft_device_t g_fft_dev;

extern int32_t sdma_send_transfer(k_sdma_transfer_cfg_t* cfg);

static rt_uint32_t fft_timeout_to_ticks(uint32_t timeout_ms)
{
    if (timeout_ms == 0) {
        timeout_ms = FFT_DEFAULT_TIMEOUT_MS;
    }

    return rt_tick_from_millisecond(timeout_ms);
}

static uint32_t fft_point_to_code(uint32_t point)
{
    switch (point) {
    case 64:
        return FFT_N64;
    case 128:
        return FFT_N128;
    case 256:
        return FFT_N256;
    case 512:
        return FFT_N512;
    case 1024:
        return FFT_N1024;
    case 2048:
        return FFT_N2048;
    case 4096:
        return FFT_N4096;
    default:
        return 0xffffffffu;
    }
}

static uint32_t fft_expected_bytes(uint32_t point, uint32_t input_mode)
{
    uint32_t bytes = point * sizeof(int16_t) * 2;

    if (input_mode == RRRR) {
        bytes /= 2;
    }

    return bytes;
}

static int fft_validate_request(const k_fft_run_request* req)
{
    if (req == RT_NULL) {
        return -EINVAL;
    }

    if (fft_point_to_code(req->point) == 0xffffffffu) {
        return -EINVAL;
    }

    if (req->mode > IFFT_MODE || req->input_mode > RR_II || req->output_mode > RR_II_OUT) {
        return -EINVAL;
    }

    if (req->input_phy_addr == 0 || req->output_phy_addr == 0) {
        return -EINVAL;
    }

    if (req->input_len != fft_expected_bytes(req->point, req->input_mode)) {
        return -EINVAL;
    }

    if (req->output_len != req->point * sizeof(int16_t) * 2) {
        return -EINVAL;
    }

    return 0;
}

static void fft_disable(fft_device_t* dev) { writeq(0, &dev->reg->enable); }

static int fft_start_input_dma(const k_fft_run_request* req)
{
    k_sdma_transfer_cfg_t cfg = {
        .src_addr              = (void*)(uintptr_t)req->input_phy_addr,
        .dst_addr              = (void*)FFT_FIFO_REG_ADDR,
        .dimension             = DIMENSION1,
        .line_size             = req->input_len,
        .timeout_ms            = req->timeout_ms ? (int32_t)req->timeout_ms : (int32_t)FFT_DEFAULT_TIMEOUT_MS,
        .ch_cfg.dat_mode       = 0,
        .ch_cfg.src_fixed      = 0,
        .ch_cfg.dst_fixed      = 1,
        .ch_cfg.wr_outstanding = 15,
        .ch_cfg.rd_outstanding = 15,
    };

    LOG_D("start input dma phy=0x%lx len=%u timeout=%u", (unsigned long)req->input_phy_addr, req->input_len, req->timeout_ms);

    return sdma_send_transfer(&cfg);
}

static int fft_start_output_dma(const k_fft_run_request* req)
{
    k_sdma_transfer_cfg_t cfg = {
        .src_addr              = (void*)FFT_FIFO_REG_ADDR,
        .dst_addr              = (void*)(uintptr_t)req->output_phy_addr,
        .dimension             = DIMENSION1,
        .line_size             = req->output_len,
        .timeout_ms            = req->timeout_ms ? (int32_t)req->timeout_ms : (int32_t)FFT_DEFAULT_TIMEOUT_MS,
        .ch_cfg.dat_mode       = 0,
        .ch_cfg.src_fixed      = 1,
        .ch_cfg.dst_fixed      = 0,
        .ch_cfg.wr_outstanding = 15,
        .ch_cfg.rd_outstanding = 15,
    };

    LOG_D("start output dma phy=0x%lx len=%u timeout=%u", (unsigned long)req->output_phy_addr, req->output_len,
          req->timeout_ms);

    return sdma_send_transfer(&cfg);
}

static int fft_run_locked(fft_device_t* dev, const k_fft_run_request* req)
{
    rt_uint32_t      event = 0;
    k_fft_cfg_reg_st cfg_reg;
    int              ret;

    cfg_reg.cfg_value     = 0;
    cfg_reg.point         = fft_point_to_code(req->point);
    cfg_reg.mode          = (k_fft_mode_e)req->mode;
    cfg_reg.im            = (k_fft_input_mode_e)req->input_mode;
    cfg_reg.om            = (k_fft_out_mode_e)req->output_mode;
    cfg_reg.shift         = req->shift;
    cfg_reg.time_out      = req->timeout_ms;
    cfg_reg.fft_intr_mask = 0;

    LOG_D("run point=%u mode=%u im=%u om=%u shift=0x%x timeout=%u", req->point, req->mode, req->input_mode, req->output_mode,
          req->shift, req->timeout_ms);
    LOG_D("buffers in=0x%lx/%u out=0x%lx/%u reg=%p", (unsigned long)req->input_phy_addr, req->input_len,
          (unsigned long)req->output_phy_addr, req->output_len, dev->reg);

    dev->last_event = 0;
    writeq(cfg_reg.cfg_value, &dev->reg->cfg);
    writeq(1, &dev->reg->int_clr);
    writeq(1, &dev->reg->enable);

    ret = fft_start_input_dma(req);
    if (ret != 0) {
        LOG_E("input dma failed ret=%d", ret);
        fft_disable(dev);
        return ret;
    }

    ret = rt_event_recv(&dev->event, FFT_EVENT_DONE | FFT_EVENT_ERR_WRITE | FFT_EVENT_ERR_READ | FFT_EVENT_ERR_TIMEOUT,
                        RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR, fft_timeout_to_ticks(req->timeout_ms), &event);
    if (ret != RT_EOK) {
        LOG_E("event wait failed ret=%d last=0x%x", ret, dev->last_event);
        fft_disable(dev);
        return -RT_ETIMEOUT;
    }

    LOG_D("event received 0x%x", event);

    if (event & FFT_EVENT_ERR_WRITE) {
        LOG_E("hardware write error");
        fft_disable(dev);
        return -EIO;
    }

    if (event & FFT_EVENT_ERR_READ) {
        LOG_E("hardware read error");
        fft_disable(dev);
        return -EIO;
    }

    if (event & FFT_EVENT_ERR_TIMEOUT) {
        LOG_E("hardware timeout event");
        fft_disable(dev);
        return -RT_ETIMEOUT;
    }

    ret = fft_start_output_dma(req);
    if (ret != 0) {
        LOG_E("output dma failed ret=%d", ret);
    }
    fft_disable(dev);

    return ret;
}

int k230_fft_run(const k_fft_run_request* req)
{
    fft_device_t* dev = &g_fft_dev;
    int           ret;

    if (!dev->inited) {
        LOG_E("run rejected: device not initialized");
        return -ENODEV;
    }

    ret = fft_validate_request(req);
    if (ret != 0) {
        LOG_E("run rejected: invalid request ret=%d point=%u im=%u om=%u in=0x%lx/%u out=0x%lx/%u", ret, req ? req->point : 0,
              req ? req->input_mode : 0, req ? req->output_mode : 0, (unsigned long)(req ? req->input_phy_addr : 0),
              req ? req->input_len : 0, (unsigned long)(req ? req->output_phy_addr : 0), req ? req->output_len : 0);
        return ret;
    }

    LOG_D("mutex take");
    rt_mutex_take(&dev->lock, RT_WAITING_FOREVER);
    ret = fft_run_locked(dev, req);
    rt_mutex_release(&dev->lock);
    LOG_D("run complete ret=%d", ret);

    return ret;
}

static rt_err_t fft_device_open(rt_device_t dev, rt_uint16_t oflag)
{
    (void)dev;
    (void)oflag;

    LOG_D("device open");

    return RT_EOK;
}

static rt_err_t fft_device_close(rt_device_t dev)
{
    (void)dev;

    LOG_D("device close");

    return RT_EOK;
}

static rt_err_t fft_device_control(rt_device_t dev, int cmd, void* args)
{
    k_fft_run_request req;
    int               pid;

    (void)dev;

    if (args == RT_NULL) {
        LOG_W("ioctl cmd=0x%x null args", cmd);
        return -RT_EINVAL;
    }

    LOG_D("ioctl cmd=0x%x args=%p", cmd, args);

    pid = lwp_getpid();

    switch (cmd) {
    case KD_IOC_CMD_FFT_RUN:
        if (0x00 != lwp_get_from_user_ex(&req, args, sizeof(req))) {
            LOG_D("ioctl copy from user failed");
            return -EFAULT;
        }

        LOG_D("ioctl request copied point=%u mode=%u im=%u om=%u shift=0x%x timeout=%u in=0x%lx/%u out=0x%lx/%u", req.point,
              req.mode, req.input_mode, req.output_mode, req.shift, req.timeout_ms, (unsigned long)req.input_phy_addr,
              req.input_len, (unsigned long)req.output_phy_addr, req.output_len);

        return k230_fft_run(&req);
    default:
        LOG_W("unsupported ioctl cmd=0x%x expected=0x%x", cmd, KD_IOC_CMD_FFT_RUN);
        return -RT_EINVAL;
    }
}

#ifdef RT_USING_DEVICE_OPS
static const struct rt_device_ops g_fft_ops = {
    .open    = fft_device_open,
    .close   = fft_device_close,
    .control = fft_device_control,
};
#endif

static void fft_irq_handler(int irq, void* data)
{
    fft_device_t* dev = (fft_device_t*)data;
    rt_uint32_t   intr_num;
    rt_uint32_t   event;

    (void)irq;

    intr_num = (rt_uint32_t)readq(&dev->reg->intr_num);
    writeq(1, &dev->reg->int_clr);

    LOG_D("irq intr_num=%u", intr_num);

    switch (intr_num) {
    case 0:
        event = FFT_EVENT_DONE;
        break;
    case 1:
        event = FFT_EVENT_ERR_WRITE;
        break;
    case 2:
        event = FFT_EVENT_ERR_READ;
        break;
    case 3:
        event = FFT_EVENT_ERR_TIMEOUT;
        break;
    default:
        event = FFT_EVENT_ERR_TIMEOUT;
        break;
    }

    dev->last_event = event;
    rt_event_send(&dev->event, event);
}

int k230_fft_dev_init(void)
{
    int           ret;
    fft_device_t* dev = &g_fft_dev;

    if (dev->inited) {
        return 0;
    }

    rt_memset(dev, 0, sizeof(*dev));

    ret = rt_mutex_init(&dev->lock, "fft_lock", RT_IPC_FLAG_FIFO);
    if (ret != RT_EOK) {
        return ret;
    }

    ret = rt_event_init(&dev->event, "fft_evt", RT_IPC_FLAG_FIFO);
    if (ret != RT_EOK) {
        rt_mutex_detach(&dev->lock);
        return ret;
    }

    dev->reg = rt_ioremap_nocache((void*)FFT_BASE_ADDR, FFT_IO_SIZE);
    if (dev->reg == RT_NULL) {
        rt_event_detach(&dev->event);
        rt_mutex_detach(&dev->lock);
        return -RT_ENOMEM;
    }

    LOG_D("init reg=%p base=0x%lx size=0x%lx sdma_send_transfer=%p", dev->reg, (unsigned long)FFT_BASE_ADDR,
          (unsigned long)FFT_IO_SIZE, sdma_send_transfer);

    fft_disable(dev);

    dev->parent.type      = RT_Device_Class_Char;
    dev->parent.user_data = dev;

#ifdef RT_USING_DEVICE_OPS
    dev->parent.ops = &g_fft_ops;
#else
    dev->parent.open    = fft_device_open;
    dev->parent.close   = fft_device_close;
    dev->parent.control = fft_device_control;
#endif

    ret = rt_device_register(&dev->parent, "fft", RT_DEVICE_FLAG_RDWR | RT_DEVICE_FLAG_STANDALONE);
    if (ret != RT_EOK) {
        rt_iounmap((void*)dev->reg);
        rt_event_detach(&dev->event);
        rt_mutex_detach(&dev->lock);
        return ret;
    }

#ifdef RT_USING_DEVICE_OPS
    LOG_D("registered device parent=%p ops=%p", &dev->parent, dev->parent.ops);
#else
    LOG_D("registered device parent=%p control=%p", &dev->parent, dev->parent.control);
#endif

    rt_hw_interrupt_install(FFT_IRQN, fft_irq_handler, dev, "fft");
    rt_hw_interrupt_umask(FFT_IRQN);

    dev->inited = RT_TRUE;
    return 0;
}
INIT_DEVICE_EXPORT(k230_fft_dev_init);

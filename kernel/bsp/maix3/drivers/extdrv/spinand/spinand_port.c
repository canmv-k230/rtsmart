/*
* Copyright (c) 2022-2024, ArtInChip Technology Co., Ltd
*
* SPDX-License-Identifier: Apache-2.0
*
* Authors: xuan.wen <xuan.wen@artinchip.com>
*/

#include <rthw.h>
#include <rtdevice.h>
#include <drivers/spi.h>
#include "spinand_port.h"
#include "bbt.h"

#define DBG_TAG "spi_nand_port"
#define DBG_LVL DBG_WARNING
#include <rtdbg.h>

static void qspi_messages_init(struct rt_qspi_message* qspi_messages, struct spi_nand_cmd_cfg* cfg, uint32_t addr,
                        uint8_t* sendbuff, uint8_t* recvbuff, uint32_t datacount)
{
    uint8_t inst_sz = __builtin_ffs(cfg->opcode);
    if(0x00 == inst_sz) {
        inst_sz = 0;
    } else if(8 > inst_sz) {
        inst_sz = 8;
    } else if(16 > inst_sz) {
        inst_sz = 16;
    } else {
        LOG_E("invalid inst sz\n");
        return;
    }

    /* 1-bit mode */
    qspi_messages->instruction.content      = cfg->opcode;
    qspi_messages->instruction.size         = 1 * 8;
    qspi_messages->instruction.qspi_lines   = cfg->opcode_bits;

    qspi_messages->address.content    = addr;
    qspi_messages->address.size       = cfg->addr_bytes * 8;
    qspi_messages->address.qspi_lines = cfg->addr_bits;

    if(cfg->dummy_bytes) {
        if (cfg->addr_bits) {
            qspi_messages->dummy_cycles = (cfg->dummy_bytes * 8) / qspi_messages->address.qspi_lines;
        } else {
            qspi_messages->dummy_cycles = (cfg->dummy_bytes * 8) / qspi_messages->instruction.qspi_lines;
        }
    } else {
        qspi_messages->dummy_cycles = 0;
    }

    /* 4-bit mode */
    qspi_messages->qspi_data_lines   = cfg->data_bits;
    qspi_messages->parent.cs_take    = 1;
    qspi_messages->parent.cs_release = 1;
    qspi_messages->parent.send_buf   = sendbuff;
    qspi_messages->parent.recv_buf   = recvbuff;
    qspi_messages->parent.length     = datacount;
    qspi_messages->parent.next       = NULL;

#if 0
    rt_kprintf("inst 0x%02X %d %d, addr 0x%X %d %d, dummy %d, %d, count %d, sbuf %p, rbuf %p\n", 
        qspi_messages->instruction.content, qspi_messages->instruction.size, qspi_messages->instruction.qspi_lines,
        qspi_messages->address.content, qspi_messages->address.size, qspi_messages->address.qspi_lines,
        qspi_messages->dummy_cycles, qspi_messages->qspi_data_lines, datacount, sendbuff, recvbuff
    );
#endif
}

int aic_spinand_transfer_message(struct aic_spinand *flash,
                                struct spi_nand_cmd_cfg *cfg, uint32_t addr,
                                uint8_t *sendbuff, uint8_t *recvbuff, uint32_t datacount)
{
    int result;
    struct rt_qspi_message qspi_messages = { 0 };
    struct rt_qspi_device *device = (struct rt_qspi_device *)flash->user_data;

    RT_ASSERT(flash != RT_NULL);
    RT_ASSERT(device != RT_NULL);

    qspi_messages_init(&qspi_messages, cfg, addr, sendbuff, recvbuff,
                    datacount);

    result = rt_mutex_take(&(device->parent.bus->lock), RT_WAITING_FOREVER);
    if (result != SPINAND_SUCCESS) {
        rt_set_errno(-RT_EBUSY);
        return result;
    }

    /* reset errno */
    rt_set_errno(SPINAND_SUCCESS);

    /* configure SPI bus */
    if (device->parent.bus->owner != &device->parent) {
        /* not the same owner as current, re-configure SPI bus */
        result = device->parent.bus->ops->configure(&device->parent,
                                                    &device->config.parent);
        if (result == SPINAND_SUCCESS) {
            /* set SPI bus owner */
            device->parent.bus->owner = &device->parent;
        } else {
            LOG_E("Configure SPI bus failed\n");
            rt_set_errno(-SPINAND_ERR);
            goto __exit;
        }
    }

    /* transmit each SPI message */
    if (device->parent.bus->ops->xfer(&device->parent, &qspi_messages.parent) < 0) {
        LOG_E("Xfer SPI bus failed\n");
        result = -SPINAND_ERR;
        rt_set_errno(-SPINAND_ERR);
        goto __exit;
    }

    result = SPINAND_SUCCESS;

__exit:
    /* release bus lock */
    rt_mutex_release(&(device->parent.bus->lock));

    return result;
}

rt_err_t spinand_probe(struct aic_spinand *flash, struct rt_qspi_device *device)
{
    rt_err_t ret;

    if (!flash || !device || !device->config.parent.max_hz ||
        device->config.parent.data_width != 8 ||
        (device->config.qspi_dl_width != 1 &&
         device->config.qspi_dl_width != 2 &&
         device->config.qspi_dl_width != 4))
        return -RT_EINVAL;
    if (flash->IsInited || flash->lock)
        return -RT_EBUSY;

    flash->user_data = device;
    flash->qspi_dl_width = device->config.qspi_dl_width;
    flash->lock = rt_mutex_create("spinand", RT_IPC_FLAG_PRIO);
    if (!flash->lock) {
        ret = -RT_ENOMEM;
        goto fail;
    }

    /* The core allocates the page/OOB buffer and bad-block cache once. */
    ret = spinand_flash_init(flash);
    if (ret != RT_EOK)
        goto fail;

    flash->IsInited = true;
    return RT_EOK;

fail:
    spinand_deinit(flash);
    return ret;
}

void spinand_deinit(struct aic_spinand *flash)
{
    if (!flash)
        return;
    if (flash->bbt.cache)
        nand_bbt_cleanup(flash);
    if (flash->databuf)
        rt_free_align(flash->databuf);
    if (flash->lock)
        rt_mutex_delete(flash->lock);
    rt_memset(flash, 0, sizeof(*flash));
}

/*
 * Copyright (c) 2006-2018, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author        Notes
 * 2012-01-13     weety         first version
 */

#include <drivers/mmcsd_core.h>
#include <drivers/sdio.h>
#include <drivers/sd.h>

#define DBG_TAG               "SDIO"
#ifdef RT_SDIO_DEBUG
#define DBG_LVL               DBG_LOG
#else
#define DBG_LVL               DBG_INFO
#endif /* RT_SDIO_DEBUG */
#include <rtdbg.h>

#ifndef RT_SDIO_STACK_SIZE
#define RT_SDIO_STACK_SIZE 512
#endif
#ifndef RT_SDIO_IO_RESET_SETTLE_MS
#define RT_SDIO_IO_RESET_SETTLE_MS 10
#endif
#ifndef RT_SDIO_THREAD_PRIORITY
#define RT_SDIO_THREAD_PRIORITY  0x40
#endif

static rt_list_t sdio_cards = RT_LIST_OBJECT_INIT(sdio_cards);
static rt_list_t sdio_drivers = RT_LIST_OBJECT_INIT(sdio_drivers);

struct sdio_card
{
    struct rt_mmcsd_card *card;
    rt_list_t  list;
};

struct sdio_driver
{
    struct rt_sdio_driver *drv;
    rt_list_t  list;
};

#define MIN(a, b) (a < b ? a : b)

static const rt_uint8_t speed_value[16] =
{
    0, 10, 12, 13, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60, 70, 80
};

static const rt_uint32_t speed_unit[8] =
{
    10000, 100000, 1000000, 10000000, 0, 0, 0, 0
};

rt_inline rt_int32_t sdio_match_card(struct rt_mmcsd_card           *card,
                                     const struct rt_sdio_device_id *id);
rt_inline rt_int32_t sdio_match_function(
    const struct rt_sdio_function  *function,
    const struct rt_sdio_device_id *id);
rt_inline rt_int32_t sdio_match_card_driver(
    struct rt_mmcsd_card *card, const struct rt_sdio_driver *driver);
rt_inline rt_int32_t sdio_match_function_driver(
    const struct rt_sdio_function *function,
    const struct rt_sdio_driver   *driver);


rt_int32_t sdio_io_send_op_cond(struct rt_mmcsd_host *host,
                                rt_uint32_t           ocr,
                                rt_uint32_t          *cmd5_resp)
{
    struct rt_mmcsd_cmd cmd;
    rt_int32_t i, err = 0;

    RT_ASSERT(host != RT_NULL);

    rt_memset(&cmd, 0, sizeof(struct rt_mmcsd_cmd));

    cmd.cmd_code = SD_IO_SEND_OP_COND;
    cmd.arg = ocr;
    cmd.flags = RESP_SPI_R4 | RESP_R4 | CMD_BCR;

    for (i = 100; i; i--) 
    {
        err = mmcsd_send_cmd(host, &cmd, 0);
        if (err)
            break;

        /* if we're just probing, do a single pass */
        if (ocr == 0)
            break;

        /* otherwise wait until reset completes */
        if (controller_is_spi(host)) 
        {
            /*
             * Both R1_SPI_IDLE and MMC_CARD_BUSY indicate
             * an initialized card under SPI, but some cards
             * (Marvell's) only behave when looking at this
             * one.
             */
            if (cmd.resp[1] & CARD_BUSY)
                break;
        } 
        else 
        {
            if (cmd.resp[0] & CARD_BUSY)
                break;
        }

        err = -RT_ETIMEOUT;

        mmcsd_delay_ms(10);
    }

    if (cmd5_resp)
        *cmd5_resp = cmd.resp[controller_is_spi(host) ? 1 : 0];

    return err;
}

rt_int32_t sdio_io_rw_direct(struct rt_mmcsd_card *card,
                             rt_int32_t            rw,
                             rt_uint32_t           fn,
                             rt_uint32_t           reg_addr,
                             rt_uint8_t           *pdata,
                             rt_uint8_t            raw)
{
    RT_ASSERT(card != RT_NULL);

    return sdio_io_rw_direct_host(card->host, rw, fn, reg_addr, pdata, raw);
}

/* CMD52 issued against a host that has no card bound yet.  Card enumeration
 * needs this: an SDIO card left initialized by a previous boot has to be reset
 * before it will answer CMD5, and at that point no rt_mmcsd_card exists. */
rt_int32_t sdio_io_rw_direct_host(struct rt_mmcsd_host *host,
                                  rt_int32_t            rw,
                                  rt_uint32_t           fn,
                                  rt_uint32_t           reg_addr,
                                  rt_uint8_t           *pdata,
                                  rt_uint8_t            raw)
{
    struct rt_mmcsd_cmd cmd;
    rt_int32_t err;

    RT_ASSERT(host != RT_NULL);
    RT_ASSERT(fn <= SDIO_MAX_FUNCTIONS);
    RT_ASSERT(pdata != RT_NULL);

    if (reg_addr & ~SDIO_ARG_CMD53_REG_MASK)
        return -RT_ERROR;

    rt_memset(&cmd, 0, sizeof(struct rt_mmcsd_cmd));

    cmd.cmd_code = SD_IO_RW_DIRECT;
    cmd.arg = rw ? SDIO_ARG_CMD52_WRITE : SDIO_ARG_CMD52_READ;
    cmd.arg |= fn << SDIO_ARG_CMD52_FUNC_SHIFT;
    cmd.arg |= raw ? SDIO_ARG_CMD52_RAW_FLAG : 0x00000000;
    cmd.arg |= reg_addr << SDIO_ARG_CMD52_REG_SHIFT;
    cmd.arg |= *pdata;
    /* SDIO names both forms R5; SDHCI calls the I/O Abort form R5b
     * so the controller waits for DAT[0] to leave the busy state. */
    cmd.flags = RESP_SPI_R5 |
        ((rw && fn == 0 && reg_addr == SDIO_REG_CCCR_IO_ABORT) ?
            RESP_R5B : RESP_R5) | CMD_AC;

    err = mmcsd_send_cmd(host, &cmd, 0);
    if (err)
        return err;

    if (!controller_is_spi(host)) 
    {
        if (cmd.resp[0] & R5_ERROR)
            return -RT_EIO;
        if (cmd.resp[0] & R5_FUNCTION_NUMBER)
            return -RT_ERROR;
        if (cmd.resp[0] & R5_OUT_OF_RANGE)
            return -RT_ERROR;
    }

    if (!rw || raw) 
    {
        if (controller_is_spi(host))
            *pdata = (cmd.resp[0] >> 8) & 0xFF;
        else
            *pdata = cmd.resp[0] & 0xFF;
    }

    return 0;
}

/* SDIO Simplified Specification V2.0, 4.4 "Reset for SDIO".
 *
 * CMD0 resets only the memory portion of a card; the I/O portion is reset by
 * writing the RES bit of the CCCR I/O Abort register.  Without this, a card that
 * a previous boot left initialized keeps that state across a warm reset of the
 * host and never answers the CMD5 that follows, so it is never enumerated -
 * which is why such cards appear to work "the first time only".
 *
 * Failures are expected and ignored: an SD or MMC card answers CMD52 with a
 * timeout, exactly like the CMD5 that mmcsd_detect() already sends to every
 * card before falling back to SD and MMC probing.
 */
void sdio_reset(struct rt_mmcsd_host *host)
{
    rt_uint8_t abort = 0;

    if (!host)
        return;

    /* Preserve the other bits when the card answers; assume the register reads
     * back as zero when it does not. */
    if (sdio_io_rw_direct_host(host, 0, 0, SDIO_REG_CCCR_IO_ABORT,
                               &abort, 0) != 0)
        abort = SDIO_IO_ABORT_RES;
    else
        abort |= SDIO_IO_ABORT_RES;

    (void)sdio_io_rw_direct_host(host, 1, 0, SDIO_REG_CCCR_IO_ABORT,
                                 &abort, 0);

    /* A card may be unresponsive until its reset completes. */
    rt_thread_mdelay(RT_SDIO_IO_RESET_SETTLE_MS);
}

rt_int32_t sdio_io_rw_extended(struct rt_mmcsd_card *card,
                               rt_int32_t            rw,
                               rt_uint32_t           fn,
                               rt_uint32_t           addr,
                               rt_int32_t            op_code,
                               rt_uint8_t           *buf,
                               rt_uint32_t           blocks,
                               rt_uint32_t           blksize)
{
    struct rt_mmcsd_req req;
    struct rt_mmcsd_cmd cmd;
    struct rt_mmcsd_data data;

    RT_ASSERT(card != RT_NULL);
    RT_ASSERT(fn <= SDIO_MAX_FUNCTIONS);
    RT_ASSERT(blocks != 1 || blksize <= 512);
    RT_ASSERT(blocks != 0);
    RT_ASSERT(blksize != 0);

    if (addr & ~SDIO_ARG_CMD53_REG_MASK)
        return -RT_ERROR;

    rt_memset(&req, 0, sizeof(struct rt_mmcsd_req));
    rt_memset(&cmd, 0, sizeof(struct rt_mmcsd_cmd));
    rt_memset(&data, 0, sizeof(struct rt_mmcsd_data));

    req.cmd = &cmd;
    req.data = &data;

    cmd.cmd_code = SD_IO_RW_EXTENDED;
    cmd.arg = rw ? SDIO_ARG_CMD53_WRITE : SDIO_ARG_CMD53_READ;
    cmd.arg |= fn << SDIO_ARG_CMD53_FUNC_SHIFT;
    cmd.arg |= op_code ? SDIO_ARG_CMD53_INCREMENT : 0x00000000;
    cmd.arg |= addr << SDIO_ARG_CMD53_REG_SHIFT;
    if (blocks == 1 && blksize <= 512)
        cmd.arg |= (blksize == 512) ? 0 : blksize;      /* byte mode */
    else
        cmd.arg |= SDIO_ARG_CMD53_BLOCK_MODE | blocks;  /* block mode */
    cmd.flags = RESP_SPI_R5 | RESP_R5 | CMD_ADTC;

    data.blksize = blksize;
    data.blks = blocks;
    data.flags = rw ? DATA_DIR_WRITE : DATA_DIR_READ;
    data.buf = (rt_uint32_t *)buf;

    mmcsd_set_data_timeout(&data, card);

    mmcsd_send_request(card->host, &req);

    if (cmd.err)
        return cmd.err;
    if (data.err)
        return data.err;

    if (!controller_is_spi(card->host)) 
    {
        if (cmd.resp[0] & R5_ERROR)
            return -RT_EIO;
        if (cmd.resp[0] & R5_FUNCTION_NUMBER)
            return -RT_ERROR;
        if (cmd.resp[0] & R5_OUT_OF_RANGE)
            return -RT_ERROR;
    }

    return 0;
}

rt_inline rt_uint32_t sdio_max_block_size(struct rt_sdio_function *func)
{
    rt_uint32_t size = MIN(func->card->host->max_seg_size,
                           func->card->host->max_blk_size);

    /* Some cards occasionally return an invalid zero FUNCE block size.
     * Do not let that turn a CMD53 byte transfer into a zero-length loop. */
    if (func->max_blk_size)
        size = MIN(size, func->max_blk_size);
    if (func->cur_blk_size)
        size = MIN(size, func->cur_blk_size);
    return MIN(size, 512u); /* maximum size for byte mode */
}

rt_int32_t sdio_io_rw_extended_block(struct rt_sdio_function *func,
                                            rt_int32_t               rw,
                                            rt_uint32_t              addr,
                                            rt_int32_t               op_code,
                                            rt_uint8_t              *buf,
                                            rt_uint32_t              len)
{
    rt_int32_t  ret;
    rt_uint32_t left_size;
    rt_uint32_t max_blks, blks;
    
    left_size = len;

    /* Do the bulk of the transfer using block mode (if supported). */
    if (func->card->cccr.multi_block &&
        func->cur_blk_size &&
        (len > sdio_max_block_size(func)))
    {
        max_blks = MIN(func->card->host->max_blk_count,
                       func->card->host->max_seg_size / func->cur_blk_size);
        max_blks = MIN(max_blks, 511u);

        while (max_blks && left_size > func->cur_blk_size)
        {
            blks = left_size / func->cur_blk_size;
            if (blks > max_blks)
                blks = max_blks;
            len = blks * func->cur_blk_size;

            ret = sdio_io_rw_extended(func->card, rw, func->num, 
                  addr, op_code, buf, blks, func->cur_blk_size);
            if (ret)
                return ret;

            left_size -= len;
            buf += len;
            if (op_code)
                addr += len;
        }
    }

    while (left_size > 0)
    {
        len = MIN(left_size, sdio_max_block_size(func));
        if (!len)
            return -RT_EINVAL;

        ret = sdio_io_rw_extended(func->card, rw, func->num, 
                  addr, op_code, buf, 1, len);
        if (ret)
            return ret;

        left_size -= len;
        buf += len;
        if (op_code)
            addr += len;
    }

    return 0;
}

rt_uint8_t sdio_io_readb(struct rt_sdio_function *func, 
                         rt_uint32_t              reg,
                         rt_int32_t              *err)
{
    rt_uint8_t data = 0;
    rt_int32_t ret;

    ret = sdio_io_rw_direct(func->card, 0, func->num, reg, &data, 0);

    if (err)
    {
        *err = ret;
    }

    return data;
}

rt_int32_t sdio_io_writeb(struct rt_sdio_function *func, 
                          rt_uint32_t              reg,
                          rt_uint8_t               data)
{
    return sdio_io_rw_direct(func->card, 1, func->num, reg, &data, 0);
}

rt_uint16_t sdio_io_readw(struct rt_sdio_function *func,
                          rt_uint32_t              addr,
                          rt_int32_t              *err)
{
    rt_int32_t ret;
    rt_uint32_t dmabuf;

    if (err)
        *err = 0;

    ret = sdio_io_rw_extended_block(func, 0, addr, 1, (rt_uint8_t *)&dmabuf, 2);
    if (ret) 
    {
        if (err)
            *err = ret;
    }

    return (rt_uint16_t)dmabuf;
}

rt_int32_t sdio_io_writew(struct rt_sdio_function *func,
                          rt_uint16_t              data,
                          rt_uint32_t              addr)
{
    rt_uint32_t dmabuf = data;

    return sdio_io_rw_extended_block(func, 1, addr, 1, (rt_uint8_t *)&dmabuf, 2);
}

rt_uint32_t sdio_io_readl(struct rt_sdio_function *func,
                          rt_uint32_t              addr,
                          rt_int32_t              *err)
{
    rt_int32_t ret;
    rt_uint32_t dmabuf;

    if (err)
        *err = 0;

    ret = sdio_io_rw_extended_block(func, 0, addr, 1, (rt_uint8_t *)&dmabuf, 4);
    if (ret) 
    {
        if (err)
            *err = ret;
    }

    return dmabuf;
}

rt_int32_t sdio_io_writel(struct rt_sdio_function *func,
                          rt_uint32_t              data,
                          rt_uint32_t              addr)
{
    rt_uint32_t dmabuf = data;

    return sdio_io_rw_extended_block(func, 1, addr, 1, (rt_uint8_t *)&dmabuf, 4);
}

rt_int32_t sdio_io_read_multi_fifo_b(struct rt_sdio_function *func, 
                                     rt_uint32_t              addr,
                                     rt_uint8_t              *buf,
                                     rt_uint32_t              len)
{
    return sdio_io_rw_extended_block(func, 0, addr, 0, buf, len);
}

rt_int32_t sdio_io_write_multi_fifo_b(struct rt_sdio_function *func, 
                                      rt_uint32_t              addr,
                                      rt_uint8_t              *buf,
                                      rt_uint32_t              len)
{
    return sdio_io_rw_extended_block(func, 1, addr, 0, buf, len);
}

rt_int32_t sdio_io_read_multi_incr_b(struct rt_sdio_function *func, 
                                     rt_uint32_t              addr,
                                     rt_uint8_t              *buf,
                                     rt_uint32_t              len)
{
    return sdio_io_rw_extended_block(func, 0, addr, 1, buf, len);
}

rt_int32_t sdio_io_write_multi_incr_b(struct rt_sdio_function *func, 
                                      rt_uint32_t              addr,
                                      rt_uint8_t              *buf,
                                      rt_uint32_t              len)
{
    return sdio_io_rw_extended_block(func, 1, addr, 1, buf, len);
}

static rt_int32_t sdio_read_cccr(struct rt_mmcsd_card *card)
{
    rt_int32_t ret;
    rt_int32_t cccr_version;
    rt_uint8_t data;

    rt_memset(&card->cccr, 0, sizeof(struct rt_sdio_cccr));

    data = sdio_io_readb(card->sdio_function[0], SDIO_REG_CCCR_CCCR_REV, &ret);
    if (ret)
        goto out;

    cccr_version = data & 0x0f;

    if (cccr_version > SDIO_CCCR_REV_3_00) 
    {
        LOG_E("unrecognised CCCR structure version %d", cccr_version);

        return -RT_ERROR;
    }

    card->cccr.sdio_version = (data & 0xf0) >> 4;

    data = sdio_io_readb(card->sdio_function[0], SDIO_REG_CCCR_CARD_CAPS, &ret);
    if (ret)
        goto out;

    if (data & SDIO_CCCR_CAP_SMB)
        card->cccr.multi_block = 1;
    if (data & SDIO_CCCR_CAP_LSC)
        card->cccr.low_speed = 1;
    if (data & SDIO_CCCR_CAP_4BLS)
        card->cccr.low_speed_4 = 1;
    if (data & SDIO_CCCR_CAP_4BLS)
        card->cccr.bus_width = 1;

    if (cccr_version >= SDIO_CCCR_REV_1_10) 
    {
        data = sdio_io_readb(card->sdio_function[0], SDIO_REG_CCCR_POWER_CTRL, &ret);
        if (ret)
            goto out;

        if (data & SDIO_POWER_SMPC)
            card->cccr.power_ctrl = 1;
    }

    if (cccr_version >= SDIO_CCCR_REV_1_20) 
    {
        data = sdio_io_readb(card->sdio_function[0], SDIO_REG_CCCR_SPEED, &ret);
        if (ret)
            goto out;

        if (data & SDIO_SPEED_SHS)
            card->cccr.high_speed = 1;
    }

out:
    return ret;
}

static rt_int32_t cistpl_funce_func0(struct rt_mmcsd_card *card,
                                     const rt_uint8_t     *buf,
                                     rt_uint32_t           size)
{
    if (size < 0x04 || buf[0] != 0)
        return -RT_ERROR;

    /* TPLFE_FN0_BLK_SIZE */
    card->cis.func0_blk_size = buf[1] | (buf[2] << 8);

    /* TPLFE_MAX_TRAN_SPEED */
    card->cis.max_tran_speed = speed_value[(buf[3] >> 3) & 15] *
                speed_unit[buf[3] & 7];

    return 0;
}

static rt_int32_t cistpl_funce_func(struct rt_sdio_function *func,
                                    const rt_uint8_t        *buf,
                                    rt_uint32_t              size)
{
    rt_uint32_t version;
    rt_uint32_t min_size;

    version = func->card->cccr.sdio_version;
    min_size = (version == SDIO_SDIO_REV_1_00) ? 28 : 42;

    if (size < min_size || buf[0] != 1)
        return -RT_ERROR;

    /* TPLFE_MAX_BLK_SIZE */
    func->max_blk_size = buf[12] | (buf[13] << 8);

    /* TPLFE_ENABLE_TIMEOUT_VAL, present in ver 1.1 and above */
    if (version > SDIO_SDIO_REV_1_00)
        func->enable_timeout_val = (buf[28] | (buf[29] << 8)) * 10;
    else
        func->enable_timeout_val = 1000; /* 1000ms */

    return 0;
}

static rt_int32_t sdio_read_cis(struct rt_sdio_function *func)
{
    rt_int32_t ret = RT_EOK;
    rt_int32_t tuple_ret;
    rt_bool_t keep_tuple;
    struct rt_sdio_function_tuple *curr, **prev;
    rt_uint32_t i, cisptr = 0;
    rt_uint8_t data;
    rt_uint8_t tpl_code, tpl_link;

    struct rt_mmcsd_card *card = func->card;
    struct rt_sdio_function *func0 = card->sdio_function[0];

    RT_ASSERT(func0 != RT_NULL);

    for (i = 0; i < 3; i++)
    {
        data = sdio_io_readb(func0, 
            SDIO_REG_FBR_BASE(func->num) + SDIO_REG_FBR_CIS + i, &ret);
        if (ret)
            return ret;
        cisptr |= data << (i * 8);
    }

    prev = &func->tuples;

    do {
        tpl_code = sdio_io_readb(func0, cisptr++, &ret);
        if (ret)
            break;
        if (tpl_code == CISTPL_END)
            break;
        if (tpl_code == CISTPL_NULL)
            continue;

        tpl_link = sdio_io_readb(func0, cisptr++, &ret);
        if (ret)
            break;
        if (tpl_link == 0xff)
            break;

        curr = rt_malloc(sizeof(struct rt_sdio_function_tuple) + tpl_link);
        if (!curr)
            return -RT_ENOMEM;
        curr->data = (rt_uint8_t *)curr + sizeof(struct rt_sdio_function_tuple);

        for (i = 0; i < tpl_link; i++) 
        {
            curr->data[i] = sdio_io_readb(func0, cisptr + i, &ret);
            if (ret)
                break;
        }
        if (ret) 
        {
            rt_free(curr);
            break;
        }

        keep_tuple = RT_FALSE;
        switch (tpl_code)
        {
        case CISTPL_MANFID:
            if (tpl_link < 4)
            {
                LOG_D("bad CISTPL_MANFID length");
                break;
            }
            if (func->num != 0)
            {
                func->manufacturer = curr->data[0];
                func->manufacturer |= curr->data[1] << 8;
                func->product = curr->data[2];
                func->product |= curr->data[3] << 8;
            }
            else
            {
                card->cis.manufacturer = curr->data[0];
                card->cis.manufacturer |= curr->data[1] << 8;
                card->cis.product = curr->data[2];
                card->cis.product |= curr->data[3] << 8;
            }
            break;
        case CISTPL_FUNCE:
            if (func->num != 0)
                tuple_ret = cistpl_funce_func(func, curr->data, tpl_link);
            else
                tuple_ret = cistpl_funce_func0(card, curr->data, tpl_link);

            if (tuple_ret)
            {
                LOG_D("bad CISTPL_FUNCE size %u "
                       "type %u", tpl_link, curr->data[0]);
            }

            break;
        case CISTPL_VERS_1:
            if (tpl_link < 2)
            {
                LOG_D("CISTPL_VERS_1 too short");
            }
            break;
        default: 
            /* this tuple is unknown to the core */
            curr->next = RT_NULL;
            curr->code = tpl_code;
            curr->size = tpl_link;
            *prev = curr;
            prev = &curr->next;
            keep_tuple = RT_TRUE;
            LOG_D( "function %d, CIS tuple code %#x, length %d",
                func->num, tpl_code, tpl_link);
            break;
        }

        if (!keep_tuple)
            rt_free(curr);
        cisptr += tpl_link;
    } while (1);

    /*
     * Link in all unknown tuples found in the common CIS so that
     * drivers don't have to go digging in two places.
     */
    if (func->num != 0)
        *prev = func0->tuples;

    return ret;
}


void sdio_free_cis(struct rt_sdio_function *func)
{
    struct rt_sdio_function_tuple *tuple, *tmp;
    struct rt_mmcsd_card *card = func->card;

    tuple = func->tuples;

    while (tuple && ((tuple != card->sdio_function[0]->tuples) || (!func->num))) 
    {
        tmp = tuple;
        tuple = tuple->next;
        rt_free(tmp);
    }

    func->tuples = RT_NULL;
}

static rt_int32_t sdio_read_fbr(struct rt_sdio_function *func)
{
    rt_int32_t ret;
    rt_uint8_t data;
    struct rt_sdio_function *func0 = func->card->sdio_function[0];

    data = sdio_io_readb(func0, 
        SDIO_REG_FBR_BASE(func->num) + SDIO_REG_FBR_STD_FUNC_IF, &ret);
    if (ret)
        goto err;

    data &= 0x0f;

    if (data == 0x0f) 
    {
        data = sdio_io_readb(func0, 
            SDIO_REG_FBR_BASE(func->num) + SDIO_REG_FBR_STD_IF_EXT, &ret);
        if (ret)
            goto err;
    }

    func->func_code = data;

err:
    return ret;
}

static rt_int32_t sdio_initialize_function(struct rt_mmcsd_card *card,
                                           rt_uint32_t           func_num)
{
    rt_int32_t ret;
    struct rt_sdio_function *func;

    RT_ASSERT(func_num <= SDIO_MAX_FUNCTIONS);

    func = rt_malloc(sizeof(struct rt_sdio_function));
    if (!func)
    {
        LOG_E("malloc rt_sdio_function failed");
        ret = -RT_ENOMEM;
        goto err;
    }
    rt_memset(func, 0, sizeof(struct rt_sdio_function));

    func->card = card;
    func->num = func_num;

    ret = sdio_read_fbr(func);
    if (ret)
        goto err1;

    for (rt_uint32_t attempt = 0; attempt < 3; attempt++)
    {
        ret = sdio_read_cis(func);
        if (ret == RT_EOK && func->max_blk_size)
            break;
        if (attempt == 2)
            break;

        LOG_W("function %u CIS read invalid (error %d, max block %u), retrying",
              func_num, ret, func->max_blk_size);
        sdio_free_cis(func);
        func->manufacturer = 0;
        func->product = 0;
        func->max_blk_size = 0;
        func->enable_timeout_val = 0;
        rt_thread_mdelay(1);
    }
    if (ret)
        goto err1;
    /* A function whose CIS never yielded a maximum block size cannot be driven
     * in block mode, so refuse it here instead of letting every later transfer
     * silently degrade to byte mode. */
    if (!func->max_blk_size)
    {
        LOG_E("function %u CIS reports zero max block size", func_num);
        ret = -RT_EIO;
        goto err1;
    }

    /*
     * product/manufacturer id is optional for function CIS, so
     * copy it from the card structure as needed.
     */
    if (func->product == 0)
    {
        func->manufacturer = card->cis.manufacturer;
        func->product = card->cis.product;
    }

    card->sdio_function[func_num] = func;

    return 0;

err1:
    sdio_free_cis(func);
    rt_free(func);
    card->sdio_function[func_num] = RT_NULL;
err:
    return ret;
}

static rt_int32_t sdio_set_highspeed(struct rt_mmcsd_card *card)
{
    rt_int32_t ret;
    rt_uint8_t speed;

    if (!(card->host->flags & MMCSD_SUP_HIGHSPEED))
        return 0;

    if (!card->cccr.high_speed)
        return 0;

    speed = sdio_io_readb(card->sdio_function[0], SDIO_REG_CCCR_SPEED, &ret);
    if (ret)
        return ret;

    speed |= SDIO_SPEED_EHS;

    ret = sdio_io_writeb(card->sdio_function[0], SDIO_REG_CCCR_SPEED, speed);
    if (ret)
        return ret;

    card->flags |= CARD_FLAG_HIGHSPEED;
    mmcsd_set_timing(card->host, MMCSD_TIMING_SD_HS);

    return 0;
}

static rt_int32_t sdio_set_bus_wide(struct rt_mmcsd_card *card)
{
    rt_int32_t ret;
    rt_uint8_t busif;

    if (!(card->host->flags & MMCSD_BUSWIDTH_4))
        return 0;

    if (card->cccr.low_speed && !card->cccr.bus_width)
        return 0;

    busif = sdio_io_readb(card->sdio_function[0], SDIO_REG_CCCR_BUS_IF, &ret);
    if (ret)
        return ret;

    busif |= SDIO_BUS_WIDTH_4BIT;

    ret = sdio_io_writeb(card->sdio_function[0], SDIO_REG_CCCR_BUS_IF, busif);
    if (ret)
        return ret;

    mmcsd_set_bus_width(card->host, MMCSD_BUS_WIDTH_4);

    return 0;
}

static rt_int32_t sdio_register_card(struct rt_mmcsd_card *card)
{
    struct sdio_card *sc;
    struct sdio_driver *sd;
    struct rt_sdio_function *function;
    rt_list_t *l;
    rt_uint8_t function_num;
    rt_int32_t probe_result;
    rt_bool_t driver_bound = RT_FALSE;

    sc = rt_malloc(sizeof(struct sdio_card));
    if (sc == RT_NULL)
    {
        LOG_E("malloc sdio card failed");
        return -RT_ENOMEM;
    }

    sc->card = card;
    rt_list_insert_after(&sdio_cards, &sc->list);

    LOG_D("SDIO card %04x:%04x functions=%u on %s",
          card->cis.manufacturer, card->cis.product,
          card->sdio_function_num,
          card->host ? card->host->name : "unknown");
    for (function_num = 1;
         function_num <= card->sdio_function_num;
         function_num++)
    {
        rt_bool_t id_matched = RT_FALSE;

        function = card->sdio_function[function_num];
        if (!function)
        {
            continue;
        }

        LOG_D("SDIO function %u class=0x%02x %04x:%04x",
              function->num, function->func_code,
              function->manufacturer, function->product);
        for (l = (&sdio_drivers)->next; l != &sdio_drivers; l = l->next)
        {
            sd = (struct sdio_driver *)rt_list_entry(
                l, struct sdio_driver, list);
            if (sdio_match_function_driver(function, sd->drv))
            {
                id_matched = RT_TRUE;
                break;
            }
        }
        if (!id_matched)
        {
            LOG_W("no driver for SDIO function %u on %s: "
                  "class=0x%02x id=%04x:%04x",
                  function->num,
                  card->host ? card->host->name : "unknown",
                  function->func_code,
                  function->manufacturer, function->product);
        }
    }

    if (rt_list_isempty(&sdio_drivers))
    {
        goto out;
    }

    for (l = (&sdio_drivers)->next; l != &sdio_drivers; l = l->next)
    {
        sd = (struct sdio_driver *)rt_list_entry(l, struct sdio_driver, list);
        if (sdio_match_card_driver(card, sd->drv))
        {
            probe_result = sd->drv->probe(card);
            if (probe_result != RT_EOK)
            {
                LOG_E("SDIO driver %s probe failed on %s: %d",
                      sd->drv->name,
                      card->host ? card->host->name : "unknown",
                      probe_result);

                /* A busy probe may still own live card references because its
                 * teardown could not complete. Keep the backing card alive, but
                 * propagate the failure so detection cannot report success. */
                if (probe_result == -RT_EBUSY)
                {
                    LOG_E("retaining SDIO card on %s after incomplete teardown",
                          card->host ? card->host->name : "unknown");
                    return probe_result;
                }

                /* A later probe must not invalidate drivers already bound. */
                if (driver_bound)
                {
                    LOG_E("keeping SDIO card on %s after later probe failure",
                          card->host ? card->host->name : "unknown");
                    return RT_EOK;
                }
                rt_list_remove(&sc->list);
                rt_free(sc);
                return probe_result;
            }
            driver_bound = RT_TRUE;
        }
    }

out:
    return 0;
}

static rt_int32_t sdio_init_card(struct rt_mmcsd_host *host, rt_uint32_t ocr)
{
    rt_int32_t err = 0;
    rt_int32_t i, function_num;
    rt_uint32_t  cmd5_resp;
    struct rt_mmcsd_card *card;

    err = sdio_io_send_op_cond(host, ocr, &cmd5_resp);
    if (err)
        goto err;

    if (controller_is_spi(host)) 
    {
        err = mmcsd_spi_use_crc(host, host->spi_use_crc);
        if (err)
            goto err;
    }

    function_num = (cmd5_resp & 0x70000000) >> 28;

    card = rt_malloc(sizeof(struct rt_mmcsd_card));
    if (!card) 
    {
        LOG_E("malloc card failed");
        err = -RT_ENOMEM;
        goto err;
    }
    rt_memset(card, 0, sizeof(struct rt_mmcsd_card));

    card->card_type = CARD_TYPE_SDIO;
    card->sdio_function_num = function_num;
    card->host = host;
    host->card = card;

    card->sdio_function[0] = rt_malloc(sizeof(struct rt_sdio_function));
    if (!card->sdio_function[0])
    {
        LOG_E("malloc sdio_func0 failed");
        err = -RT_ENOMEM;
        goto err1;
    }
    rt_memset(card->sdio_function[0], 0, sizeof(struct rt_sdio_function));
    card->sdio_function[0]->card = card;
    card->sdio_function[0]->num = 0;

    if (!controller_is_spi(host)) 
    {
        err = mmcsd_get_card_addr(host, &card->rca);
        if (err)
            goto err2;

        mmcsd_set_bus_mode(host, MMCSD_BUSMODE_PUSHPULL);
    }

    if (!controller_is_spi(host)) 
    {
        err = mmcsd_select_card(card);
        if (err)
            goto err2;
    }

    err = sdio_read_cccr(card);
    if (err)
        goto err2;

    err = sdio_read_cis(card->sdio_function[0]);
    if (err)
        goto err2;

    err = sdio_set_highspeed(card);
    if (err)
        goto err2;

    if (card->flags & CARD_FLAG_HIGHSPEED) 
    {
        mmcsd_set_clock(host, 50000000);
    } 
    else 
    {
        mmcsd_set_clock(host, card->cis.max_tran_speed);
    }

    err = sdio_set_bus_wide(card);
    if (err)
        goto err2;

    for (i = 1; i < function_num + 1; i++) 
    {
        err = sdio_initialize_function(card, i);
        if (err)
            goto err3;
    }


    /* Driver probes may start worker threads which access the card, or stop
     * those threads again while recovering from an initialization error.  Do
     * not keep the bus locked across probe: a worker cannot otherwise complete
     * its SDIO request and the probe deadlocks waiting for that worker. */
    mmcsd_host_unlock(host);
    err = sdio_register_card(card);
    mmcsd_host_lock(host);
    if (err == -RT_EBUSY)
    {
        return err;
    }
    if (err)
    {
        goto err3;
    }

    return 0;

err3:
    if (host->card)
    {
        for (i = 1; i < host->card->sdio_function_num + 1; i++)
        {
            if (host->card->sdio_function[i])
            {
                sdio_free_cis(host->card->sdio_function[i]);
                rt_free(host->card->sdio_function[i]);
                host->card->sdio_function[i] = RT_NULL;
            }
        }
    }
err2:
    if (host->card && host->card->sdio_function[0])
    {
        sdio_free_cis(host->card->sdio_function[0]);
        rt_free(host->card->sdio_function[0]);
        host->card->sdio_function[0] = RT_NULL;
    }
err1:
    if (host->card)
    {
        rt_free(host->card);
        host->card = RT_NULL;
    }
err:
    LOG_E("error %d while initialising SDIO card", err);
    
    return err;
}

rt_int32_t init_sdio(struct rt_mmcsd_host *host, rt_uint32_t ocr)
{
    rt_int32_t err;
    rt_uint32_t  current_ocr;

    RT_ASSERT(host != RT_NULL);

    // if (ocr & 0x7F)
    // {
    //     LOG_W("Card ocr below the defined voltage rang.");
    //     ocr &= ~0x7F;
    // }

    // if (ocr & VDD_165_195)
    // {
    //     LOG_W("Can't support the low voltage SDIO card.");
    //     ocr &= ~VDD_165_195;
    // }

    current_ocr = mmcsd_select_voltage(host, ocr);

    if (!current_ocr) 
    {
        err = -RT_ERROR;
        goto err;
    }

    err = sdio_init_card(host, current_ocr);
    if (err == -RT_EBUSY)
        goto err;
    if (err)
        goto remove_card;

    return 0;

remove_card:
    rt_free(host->card);
    host->card = RT_NULL;
err:

    LOG_E("init SDIO card failed");

    return err;
}

static void sdio_irq_thread(void *param)
{
    rt_int32_t i, ret;
    rt_uint8_t pending;
    rt_uint8_t int_en;
    struct rt_mmcsd_card *card;
    struct rt_mmcsd_host *host = (struct rt_mmcsd_host *)param;
    RT_ASSERT(host != RT_NULL);
    card = host->card;
    RT_ASSERT(card != RT_NULL);

    while (1) 
    {
        if (rt_sem_take(host->sdio_irq_sem, RT_WAITING_FOREVER) == RT_EOK)
        {
            mmcsd_host_lock(host);
            pending = sdio_io_readb(host->card->sdio_function[0], 
                        SDIO_REG_CCCR_INT_PEND, &ret);
            if (ret) 
            {
                mmcsd_dbg("error %d reading SDIO_REG_CCCR_INT_PEND\n", ret);
                goto out;
            }

            for (i = 1; i <= 7; i++)
            {
                if (pending & (1 << i))
                {
                    struct rt_sdio_function *func = card->sdio_function[i];
                    if (func && func->irq_handler)
                    {
                        func->irq_handler(func);
                        continue;
                    }
                    /* Nobody owns this function's interrupt: as long as the
                     * bit stays enabled the card re-asserts DAT[1] the moment
                     * this thread re-enables the host interrupt, spinning the
                     * thread forever.  Mask it in the CCCR; sdio_attach_irq()
                     * re-enables the bit when a handler is registered.  Say so
                     * out loud - silently retiring a card interrupt for the
                     * rest of the session is not a debug-level event. */
                    LOG_W("SDIO: masking IRQ of function %d: %s", i,
                        func ? "no handler registered" : "no such function");
                    int_en = sdio_io_readb(card->sdio_function[0],
                        SDIO_REG_CCCR_INT_EN, &ret);
                    if (ret != RT_EOK)
                    {
                        LOG_W("SDIO: failed to read INT_EN while masking "
                              "function %d: %d", i, ret);
                        continue;
                    }
                    int_en &= ~(1 << i);
                    ret = sdio_io_writeb(card->sdio_function[0],
                        SDIO_REG_CCCR_INT_EN, int_en);
                    if (ret != RT_EOK)
                    {
                        LOG_W("SDIO: failed to mask IRQ of function %d: %d",
                            i, ret);
                    }
                }
            }

        out:
            mmcsd_host_unlock(host);
            /* A concurrent defer/rearm can make both paths enable the IRQ;
             * the K230 op only sets the same controller bit, so this is
             * intentionally harmless. */
            if ((host->flags & MMCSD_SUP_SDIO_IRQ) &&
                !host->sdio_irq_deferred)
                host->ops->enable_sdio_irq(host, 1);
            continue;
        }
    }
}

static rt_int32_t sdio_irq_thread_create(struct rt_mmcsd_card *card)
{
    struct rt_mmcsd_host *host = card->host;
    rt_err_t ret;

    if (!host->sdio_irq_num)
    {
        host->sdio_irq_deferred = RT_FALSE;
        host->sdio_irq_sem = rt_sem_create("sdio_irq", 0, RT_IPC_FLAG_FIFO);
        if (host->sdio_irq_sem == RT_NULL)
        {
            return -RT_ENOMEM;
        }

        host->sdio_irq_thread = rt_thread_create(
            "sdio_irq", sdio_irq_thread, host, RT_SDIO_STACK_SIZE,
            RT_SDIO_THREAD_PRIORITY, 20);
        if (host->sdio_irq_thread == RT_NULL)
        {
            rt_sem_delete(host->sdio_irq_sem);
            host->sdio_irq_sem = RT_NULL;
            return -RT_ENOMEM;
        }
        ret = rt_thread_startup(host->sdio_irq_thread);
        if (ret != RT_EOK)
        {
            rt_thread_delete(host->sdio_irq_thread);
            rt_sem_delete(host->sdio_irq_sem);
            host->sdio_irq_thread = RT_NULL;
            host->sdio_irq_sem = RT_NULL;
            return ret;
        }
    }
    host->sdio_irq_num++;
    return RT_EOK;
}

static rt_int32_t sdio_irq_thread_delete(struct rt_mmcsd_card *card)
{
    struct rt_mmcsd_host *host = card->host;

    RT_ASSERT(host->sdio_irq_num > 0);

    host->sdio_irq_num--;
    if (!host->sdio_irq_num) 
    {
        if (host->flags & MMCSD_SUP_SDIO_IRQ)
            host->ops->enable_sdio_irq(host, 0);
        /* sdio_irq_thread() runs CMD52 with the host lock held.  Take it here
         * so the thread cannot be destroyed mid-command, which would strand
         * host->bus_lock and wedge every later request on this host.  The lock
         * is recursive, so a caller already holding it is fine.  The thread
         * must go before the semaphore it waits on, or it wakes on freed
         * memory and spins. */
        mmcsd_host_lock(host);
        rt_thread_delete(host->sdio_irq_thread);
        host->sdio_irq_thread = RT_NULL;
        rt_sem_delete(host->sdio_irq_sem);
        host->sdio_irq_sem = RT_NULL;
        host->sdio_irq_deferred = RT_FALSE;
        mmcsd_host_unlock(host);
    }

    return 0;
}

rt_int32_t sdio_attach_irq(struct rt_sdio_function *func,
                           rt_sdio_irq_handler_t   *handler)
{
    rt_int32_t ret;
    rt_int32_t restore_ret;
    rt_uint8_t reg;
    rt_uint8_t old_reg;
    struct rt_sdio_function *func0;
    struct rt_mmcsd_host *host;

    RT_ASSERT(func != RT_NULL);
    RT_ASSERT(func->card != RT_NULL);

    func0 = func->card->sdio_function[0];
    host = func->card->host;

    mmcsd_dbg("SDIO: enabling IRQ for function %d\n", func->num);

    mmcsd_host_lock(host);
    if (func->irq_handler)
    {
        mmcsd_dbg("SDIO: IRQ for already in use.\n");
        ret = -RT_EBUSY;
        goto out;
    }

    reg = sdio_io_readb(func0, SDIO_REG_CCCR_INT_EN, &ret);
    if (ret)
        goto out;
    old_reg = reg;

    reg |= 1 << func->num;
    reg |= 1; /* Master interrupt enable */

    /* Publish the handler before enabling it in the CCCR.  The host lock also
     * serializes this RMW with orphan masking in sdio_irq_thread(). */
    func->irq_handler = handler;

    ret = sdio_io_writeb(func0, SDIO_REG_CCCR_INT_EN, reg);
    if (ret)
    {
        func->irq_handler = RT_NULL;
        goto out;
    }

    ret = sdio_irq_thread_create(func->card);
    if (ret)
    {
        func->irq_handler = RT_NULL;
        restore_ret = sdio_io_writeb(func0, SDIO_REG_CCCR_INT_EN, old_reg);
        if (restore_ret != RT_EOK)
        {
            LOG_W("SDIO: failed to restore INT_EN after IRQ attach "
                  "failure: %d", restore_ret);
        }
    }

out:
    mmcsd_host_unlock(host);
    return ret;
}

rt_int32_t sdio_detach_irq(struct rt_sdio_function *func)
{
    rt_int32_t ret;
    rt_uint8_t reg;
    struct rt_sdio_function *func0;
    struct rt_mmcsd_host *host;

    RT_ASSERT(func != RT_NULL);
    RT_ASSERT(func->card != RT_NULL);

    func0 = func->card->sdio_function[0];
    host = func->card->host;

    mmcsd_dbg("SDIO: disabling IRQ for function %d\n", func->num);

    mmcsd_host_lock(host);
    /* Disable in the CCCR before dropping the handler, the mirror image of
     * sdio_attach_irq().  Holding the host lock serializes this RMW with the
     * IRQ thread and with concurrent attach/detach operations. */
    reg = sdio_io_readb(func0, SDIO_REG_CCCR_INT_EN, &ret);
    if (ret)
        goto detach;

    reg &= ~(1 << func->num);

    /* Disable master interrupt with the last function interrupt */
    if (!(reg & 0xFE))
        reg = 0;

    ret = sdio_io_writeb(func0, SDIO_REG_CCCR_INT_EN, reg);
    if (ret)
        goto detach;

detach:
    /* Once teardown begins, retaining a software callback is less safe than a
     * stale device-side enable bit. The host caller masks its controller IRQ,
     * and orphan handling masks any source seen after a partial detach. */
    if (func->irq_handler)
    {
        func->irq_handler = RT_NULL;
        sdio_irq_thread_delete(func->card);
    }

    mmcsd_host_unlock(host);
    return ret;
}

void sdio_irq_wakeup(struct rt_mmcsd_host *host)
{
    if (host->flags & MMCSD_SUP_SDIO_IRQ)
        host->ops->enable_sdio_irq(host, 0);
    if (host->sdio_irq_sem)
        rt_sem_release(host->sdio_irq_sem);
}

void sdio_irq_defer(struct rt_mmcsd_host *host)
{
    RT_ASSERT(host != RT_NULL);

    /* Called from a function IRQ handler while sdio_irq_thread owns the host
     * lock. Keep a level-triggered host IRQ masked until deferred I/O drains
     * the card source. */
    host->sdio_irq_deferred = RT_TRUE;
    if (host->flags & MMCSD_SUP_SDIO_IRQ)
        host->ops->enable_sdio_irq(host, 0);
}

void sdio_irq_rearm(struct rt_mmcsd_host *host)
{
    RT_ASSERT(host != RT_NULL);

    mmcsd_host_lock(host);
    host->sdio_irq_deferred = RT_FALSE;
    if (host->sdio_irq_num && (host->flags & MMCSD_SUP_SDIO_IRQ))
        host->ops->enable_sdio_irq(host, 1);
    mmcsd_host_unlock(host);
}

rt_int32_t sdio_enable_func(struct rt_sdio_function *func)
{
    rt_int32_t ret;
    rt_uint8_t reg;
    rt_uint32_t timeout;
    struct rt_sdio_function *func0;

    RT_ASSERT(func != RT_NULL);
    RT_ASSERT(func->card != RT_NULL);

    func0 = func->card->sdio_function[0];

    mmcsd_dbg("SDIO: enabling function %d\n", func->num);

    reg = sdio_io_readb(func0, SDIO_REG_CCCR_IO_EN, &ret);
    if (ret)
        goto err;

    reg |= 1 << func->num;

    ret = sdio_io_writeb(func0, SDIO_REG_CCCR_IO_EN, reg);
    if (ret)
        goto err;

    timeout = rt_tick_get() + func->enable_timeout_val * RT_TICK_PER_SECOND / 1000;

    while (1) 
    {
        reg = sdio_io_readb(func0, SDIO_REG_CCCR_IO_RDY, &ret);
        if (ret)
            goto err;
        if (reg & (1 << func->num))
            break;
        ret = -RT_ETIMEOUT;
        if (rt_tick_get() > timeout)
            goto err;
    }

    mmcsd_dbg("SDIO: enabled function successfull\n");

    return 0;

err:
    mmcsd_dbg("SDIO: failed to enable function %d\n", func->num);
    return ret;
}

rt_int32_t sdio_disable_func(struct rt_sdio_function *func)
{
    rt_int32_t ret;
    rt_uint8_t reg;
    struct rt_sdio_function *func0;

    RT_ASSERT(func != RT_NULL);
    RT_ASSERT(func->card != RT_NULL);

    func0 = func->card->sdio_function[0];

    mmcsd_dbg("SDIO: disabling function %d\n", func->num);

    reg =  sdio_io_readb(func0, SDIO_REG_CCCR_IO_EN, &ret);
    if (ret)
        goto err;

    reg &= ~(1 << func->num);

    ret = sdio_io_writeb(func0, SDIO_REG_CCCR_IO_EN, reg);
    if (ret)
        goto err;

    mmcsd_dbg("SDIO: disabled function successfull\n");

    return 0;

err:
    mmcsd_dbg("SDIO: failed to disable function %d\n", func->num);
    return -RT_EIO;
}

void sdio_set_drvdata(struct rt_sdio_function *func, void *data)
{
    func->priv = data;
}

void* sdio_get_drvdata(struct rt_sdio_function *func)
{
    return func->priv;
}

rt_int32_t sdio_set_block_size(struct rt_sdio_function *func,
                               rt_uint32_t              blksize)
{
    rt_int32_t ret;
    struct rt_sdio_function *func0 = func->card->sdio_function[0];

    if (blksize > func->card->host->max_blk_size)
        return -RT_ERROR;
    if (func->max_blk_size && blksize > func->max_blk_size)
        return -RT_ERROR;

    if (blksize == 0) 
    {
        if (!func->max_blk_size)
            return -RT_EINVAL;
        blksize = MIN(func->max_blk_size, func->card->host->max_blk_size);
        blksize = MIN(blksize, 512u);
    }

    ret = sdio_io_writeb(func0, SDIO_REG_FBR_BASE(func->num) + SDIO_REG_FBR_BLKSIZE, 
                 blksize & 0xff);
    if (ret)
        return ret;
    ret = sdio_io_writeb(func0, SDIO_REG_FBR_BASE(func->num) + SDIO_REG_FBR_BLKSIZE + 1, 
                 (blksize >> 8) & 0xff);
    if (ret)
        return ret;
    func->cur_blk_size = blksize;

    return 0;
}

rt_inline rt_int32_t sdio_match_function(
    const struct rt_sdio_function  *function,
    const struct rt_sdio_device_id *id)
{
    return (function != RT_NULL) &&
           ((id->manufacturer == SDIO_ANY_MAN_ID) ||
            (id->manufacturer == function->manufacturer)) &&
           ((id->func_code == SDIO_ANY_FUNC_ID) ||
            (id->func_code == function->func_code)) &&
           ((id->product == SDIO_ANY_PROD_ID) ||
            (id->product == function->product));
}

rt_inline rt_int32_t sdio_match_card(struct rt_mmcsd_card           *card,
                                     const struct rt_sdio_device_id *id)
{
    rt_uint8_t num = 1;

    while (num <= card->sdio_function_num)
    {
        struct rt_sdio_function *function = card->sdio_function[num];

        if (sdio_match_function(function, id))
            return 1;
        num++;
    }

    return 0;
}

rt_inline rt_int32_t sdio_match_function_driver(
    const struct rt_sdio_function *function,
    const struct rt_sdio_driver   *driver)
{
    rt_size_t count;
    rt_size_t index;

    if (!driver || !driver->id)
    {
        return 0;
    }
    count = driver->id_count ? driver->id_count : 1U;
    for (index = 0; index < count; index++)
    {
        if (sdio_match_function(function, &driver->id[index]))
        {
            return 1;
        }
    }
    return 0;
}

rt_inline rt_int32_t sdio_match_card_driver(
    struct rt_mmcsd_card *card, const struct rt_sdio_driver *driver)
{
    rt_size_t count;
    rt_size_t index;

    if (!driver || !driver->id)
    {
        return 0;
    }
    count = driver->id_count ? driver->id_count : 1U;
    for (index = 0; index < count; index++)
    {
        if (sdio_match_card(card, &driver->id[index]))
        {
            return 1;
        }
    }
    return 0;
}


static struct rt_mmcsd_card *sdio_match_driver(
    const struct rt_sdio_driver *driver)
{
    rt_list_t *l;
    struct sdio_card *sc;
    struct rt_mmcsd_card *card;

    for (l = (&sdio_cards)->next; l != &sdio_cards; l = l->next)
    {
        sc = (struct sdio_card *)rt_list_entry(l, struct sdio_card, list);
        card = sc->card;

        if (sdio_match_card_driver(card, driver))
        {
            return card;
        }
    }

    return RT_NULL;
}

rt_int32_t sdio_register_driver(struct rt_sdio_driver *driver)
{
    struct sdio_driver *sd;
    struct rt_mmcsd_card *card;

    sd = rt_malloc(sizeof(struct sdio_driver));
    if (sd == RT_NULL)
    {
        LOG_E("malloc sdio driver failed");

        return -RT_ENOMEM;
    }

    sd->drv = driver;
    rt_list_insert_after(&sdio_drivers, &sd->list);

    if (!rt_list_isempty(&sdio_cards))
    {
        card = sdio_match_driver(driver);
        if (card != RT_NULL)
        {
            return driver->probe(card);
        }
    }

    return -RT_EEMPTY;
}

rt_int32_t sdio_unregister_driver(struct rt_sdio_driver *driver)
{
    rt_list_t *l;
    struct sdio_driver *sd = RT_NULL;
    struct rt_mmcsd_card *card;

    for (l = (&sdio_drivers)->next; l != &sdio_drivers; l = l->next)
    {
        sd = (struct sdio_driver *)rt_list_entry(l, struct sdio_driver, list);
        if (sd->drv != driver)
        {
            sd = RT_NULL;
        }
    }

    if (sd == RT_NULL)
    {
        LOG_E("SDIO driver %s not register", driver->name);
        return -RT_ERROR;
    }

    if (!rt_list_isempty(&sdio_cards))
    {
        card = sdio_match_driver(driver);
        if (card != RT_NULL)
        {
            rt_int32_t result = driver->remove(card);

            if (result != RT_EOK)
            {
                LOG_E("SDIO driver %s removal incomplete: %d",
                      driver->name, result);
                return result;
            }
            rt_list_remove(&sd->list);
            rt_free(sd);
        }
    }

    return 0;
}

void rt_sdio_init(void)
{

}

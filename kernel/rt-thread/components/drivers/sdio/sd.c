/*
 * Copyright (c) 2006-2018, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author        Notes
 * 2011-07-25     weety     first version
 */

#include <drivers/mmcsd_core.h>
#include <drivers/sd.h>

#define DBG_TAG               "SDIO"
#ifdef RT_SDIO_DEBUG
#define DBG_LVL               DBG_LOG
#else
#define DBG_LVL               DBG_INFO
#endif /* RT_SDIO_DEBUG */
#include <rtdbg.h>

static const rt_uint32_t tran_unit[] =
{
    10000, 100000, 1000000, 10000000,
    0,     0,      0,       0
};

static const rt_uint8_t tran_value[] =
{
    0,  10, 12, 13, 15, 20, 25, 30,
    35, 40, 45, 50, 55, 60, 70, 80,
};

static const rt_uint32_t tacc_uint[] =
{
    1, 10, 100, 1000, 10000, 100000, 1000000, 10000000,
};

static const rt_uint8_t tacc_value[] =
{
    0,  10, 12, 13, 15, 20, 25, 30,
    35, 40, 45, 50, 55, 60, 70, 80,
};

static const rt_uint32_t sd_au_size_map[] =
{
    0,
    16 * 1024,
    32 * 1024,
    64 * 1024,
    128 * 1024,
    256 * 1024,
    512 * 1024,
    1024 * 1024,
    2 * 1024 * 1024,
    4 * 1024 * 1024,
    8 * 1024 * 1024,
    12 * 1024 * 1024,
    16 * 1024 * 1024,
    24 * 1024 * 1024,
    32 * 1024 * 1024,
    64 * 1024 * 1024,
};

rt_inline rt_uint32_t GET_BITS(rt_uint32_t *resp,
                               rt_uint32_t  start,
                               rt_uint32_t  size)
{                               
        const rt_int32_t __size = size;
        const rt_uint32_t __mask = (__size < 32 ? 1 << __size : 0) - 1; 
        const rt_int32_t __off = 3 - ((start) / 32);
        const rt_int32_t __shft = (start) & 31;
        rt_uint32_t __res;

        __res = resp[__off] >> __shft;
        if (__size + __shft > 32)
            __res |= resp[__off-1] << ((32 - __shft) % 32);

        return __res & __mask;
}

static rt_int32_t mmcsd_parse_csd(struct rt_mmcsd_card *card)
{
    struct rt_mmcsd_csd *csd = &card->csd;
    rt_uint32_t *resp = card->resp_csd;

    csd->csd_structure = GET_BITS(resp, 126, 2);

    switch (csd->csd_structure)
    {
    case 0:
        csd->taac = GET_BITS(resp, 112, 8);
        csd->nsac = GET_BITS(resp, 104, 8);
        csd->tran_speed = GET_BITS(resp, 96, 8);
        csd->card_cmd_class = GET_BITS(resp, 84, 12);
        csd->rd_blk_len = GET_BITS(resp, 80, 4);
        csd->rd_blk_part = GET_BITS(resp, 79, 1);
        csd->wr_blk_misalign = GET_BITS(resp, 78, 1);
        csd->rd_blk_misalign = GET_BITS(resp, 77, 1);
        csd->dsr_imp = GET_BITS(resp, 76, 1);
        csd->c_size = GET_BITS(resp, 62, 12);
        csd->c_size_mult = GET_BITS(resp, 47, 3);
        csd->r2w_factor = GET_BITS(resp, 26, 3);
        csd->wr_blk_len = GET_BITS(resp, 22, 4);
        csd->wr_blk_partial = GET_BITS(resp, 21, 1);
        csd->csd_crc = GET_BITS(resp, 1, 7);

        card->card_blksize = 1 << csd->rd_blk_len;
        card->card_capacity = (csd->c_size + 1) << (csd->c_size_mult + 2);
        card->card_capacity *= card->card_blksize;
        card->card_capacity >>= 10; /* unit:KB */
        card->tacc_clks = csd->nsac * 100;
        card->tacc_ns = (tacc_uint[csd->taac&0x07] * tacc_value[(csd->taac&0x78)>>3] + 9) / 10;
        card->max_data_rate = tran_unit[csd->tran_speed&0x07] * tran_value[(csd->tran_speed&0x78)>>3];

        break;
    case 1:
        card->flags |= CARD_FLAG_SDHC;

        /*This field is fixed to 0Eh, which indicates 1 ms. 
          The host should not use TAAC, NSAC, and R2W_FACTOR
          to calculate timeout and should uses fixed timeout
          values for read and write operations*/
        csd->taac = GET_BITS(resp, 112, 8);
        csd->nsac = GET_BITS(resp, 104, 8);
        csd->tran_speed = GET_BITS(resp, 96, 8);
        csd->card_cmd_class = GET_BITS(resp, 84, 12);
        csd->rd_blk_len = GET_BITS(resp, 80, 4);
        csd->rd_blk_part = GET_BITS(resp, 79, 1);
        csd->wr_blk_misalign = GET_BITS(resp, 78, 1);
        csd->rd_blk_misalign = GET_BITS(resp, 77, 1);
        csd->dsr_imp = GET_BITS(resp, 76, 1);
        csd->c_size = GET_BITS(resp, 48, 22);

        csd->r2w_factor = GET_BITS(resp, 26, 3);
        csd->wr_blk_len = GET_BITS(resp, 22, 4);
        csd->wr_blk_partial = GET_BITS(resp, 21, 1);
        csd->csd_crc = GET_BITS(resp, 1, 7);

        card->card_blksize = 512;       
        card->card_capacity = (csd->c_size + 1) * 512;  /* unit:KB */
        card->card_sec_cnt = card->card_capacity * 2;
        card->tacc_clks = 0;
        card->tacc_ns = 0;
        card->max_data_rate = tran_unit[csd->tran_speed&0x07] * tran_value[(csd->tran_speed&0x78)>>3];

        break;
    default:
        LOG_E("unrecognised CSD structure version %d!", csd->csd_structure);

        return -RT_ERROR;
    }
    LOG_I("SD card capacity %d KB.", card->card_capacity);

    return 0;
}

static rt_int32_t mmcsd_parse_scr(struct rt_mmcsd_card *card)
{
    struct rt_sd_scr *scr = &card->scr;
    rt_uint32_t resp[4];

    resp[3] = card->resp_scr[1];
    resp[2] = card->resp_scr[0];
    scr->sd_version = GET_BITS(resp, 56, 4);
    scr->sd_bus_widths = GET_BITS(resp, 48, 4);

    return 0;
}

static void mmcsd_parse_ssr(struct rt_mmcsd_card *card, const rt_uint8_t *ssr)
{
    rt_uint8_t au_size;
    rt_uint32_t au_bytes;

    au_size = (ssr[10] >> 4) & 0x0F;
    au_bytes = sd_au_size_map[au_size];
    if (au_bytes == 0)
    {
        return;
    }

    card->erase_size = au_bytes >> 9;
}

static rt_int32_t mmcsd_switch(struct rt_mmcsd_card *card)
{
    rt_int32_t err;
    struct rt_mmcsd_host *host = card->host;
    struct rt_mmcsd_req req;
    struct rt_mmcsd_cmd cmd;
    struct rt_mmcsd_data data;
    rt_uint8_t *buf;

    buf = (rt_uint8_t*)rt_malloc(64);
    if (!buf) 
    {
        LOG_E("alloc memory failed!");

        return -RT_ENOMEM;
    }
    
    if (card->card_type != CARD_TYPE_SD)
        goto err;
    if (card->scr.sd_version < SCR_SPEC_VER_1)
        goto err;

    rt_memset(&cmd, 0, sizeof(struct rt_mmcsd_cmd));

    cmd.cmd_code = SD_SWITCH;
    cmd.arg = 0x00FFFFF1;
    cmd.flags = RESP_R1 | CMD_ADTC;

    rt_memset(&data, 0, sizeof(struct rt_mmcsd_data));

    mmcsd_set_data_timeout(&data, card);

    data.blksize = 64;
    data.blks = 1;
    data.flags = DATA_DIR_READ;
    data.buf = (rt_uint32_t *)buf;

    rt_memset(&req, 0, sizeof(struct rt_mmcsd_req));

    req.cmd = &cmd;
    req.data = &data;

    mmcsd_send_request(host, &req);

    if (cmd.err || data.err) 
    {
        goto err1;
    }

    if ((buf[13] & 0x02) != 0x02)
    {
        LOG_I("high speed is not support by card.");
        goto err;
    }

    card->hs_max_data_rate = 50000000;

    rt_memset(&cmd, 0, sizeof(struct rt_mmcsd_cmd));

    cmd.cmd_code = SD_SWITCH;
    cmd.arg = 0x80FFFFF1;
    cmd.flags = RESP_R1 | CMD_ADTC;

    rt_memset(&data, 0, sizeof(struct rt_mmcsd_data));

    mmcsd_set_data_timeout(&data, card);

    data.blksize = 64;
    data.blks = 1;
    data.flags = DATA_DIR_READ;
    data.buf = (rt_uint32_t *)buf;

    rt_memset(&req, 0, sizeof(struct rt_mmcsd_req));

    req.cmd = &cmd;
    req.data = &data;

    mmcsd_send_request(host, &req);

    if (cmd.err || data.err) 
    {
        goto err1;
    }

    if ((buf[16] & 0xF) != 1) 
    {
        LOG_I("switching card to high speed failed!");
        goto err;
    }

    card->flags |= CARD_FLAG_HIGHSPEED;

err:
    rt_free(buf);
    return 0;

err1:
    if (cmd.err)
        err = cmd.err;
    if (data.err)
        err = data.err;

    return err;
}

static rt_err_t mmcsd_app_cmd(struct rt_mmcsd_host *host,
                              struct rt_mmcsd_card *card)
{
    rt_err_t err;
    struct rt_mmcsd_cmd cmd = {0};

    cmd.cmd_code = APP_CMD;

    if (card) 
    {
        cmd.arg = card->rca << 16;
        cmd.flags = RESP_R1 | CMD_AC;
    } 
    else 
    {
        cmd.arg = 0;
        cmd.flags = RESP_R1 | CMD_BCR;
    }

    err = mmcsd_send_cmd(host, &cmd, 0);
    if (err)
        return err;

    /* Check that card supported application commands */
    if (!controller_is_spi(host) && !(cmd.resp[0] & R1_APP_CMD))
        return -RT_ERROR;

    return RT_EOK;
}


rt_err_t mmcsd_send_app_cmd(struct rt_mmcsd_host *host,
                            struct rt_mmcsd_card *card,
                            struct rt_mmcsd_cmd  *cmd,
                            int                   retry)
{
    struct rt_mmcsd_req req;

    rt_uint32_t i; 
    rt_err_t err;

    err = -RT_ERROR;

    /*
     * We have to resend MMC_APP_CMD for each attempt so
     * we cannot use the retries field in mmc_command.
     */
    for (i = 0;i <= retry;i++) 
    {
        rt_memset(&req, 0, sizeof(struct rt_mmcsd_req));

        err = mmcsd_app_cmd(host, card);
        if (err) 
        {
            /* no point in retrying; no APP commands allowed */
            if (controller_is_spi(host)) 
            {
                if (cmd->resp[0] & R1_SPI_ILLEGAL_COMMAND)
                    break;
            }
            continue;
        }

        rt_memset(&req, 0, sizeof(struct rt_mmcsd_req));

        rt_memset(cmd->resp, 0, sizeof(cmd->resp));

        req.cmd = cmd;
        //cmd->data = NULL;

        mmcsd_send_request(host, &req);

        err = cmd->err;
        if (!cmd->err)
            break;

        /* no point in retrying illegal APP commands */
        if (controller_is_spi(host)) 
        {
            if (cmd->resp[0] & R1_SPI_ILLEGAL_COMMAND)
                break;
        }
    }

    return err;
}

rt_err_t mmcsd_app_set_bus_width(struct rt_mmcsd_card *card, rt_int32_t width)
{
    rt_err_t err;
    struct rt_mmcsd_cmd cmd;

    rt_memset(&cmd, 0, sizeof(struct rt_mmcsd_cmd));

    cmd.cmd_code = SD_APP_SET_BUS_WIDTH;
    cmd.flags = RESP_R1 | CMD_AC;

    switch (width) 
    {
    case MMCSD_BUS_WIDTH_1:
        cmd.arg = MMCSD_BUS_WIDTH_1;
        break;
    case MMCSD_BUS_WIDTH_4:
        cmd.arg = MMCSD_BUS_WIDTH_4;
        break;
    default:
        return -RT_ERROR;
    }

    err = mmcsd_send_app_cmd(card->host, card, &cmd, 3);
    if (err)
        return err;

    return RT_EOK;
}

rt_err_t mmcsd_send_app_op_cond(struct rt_mmcsd_host *host,
                                rt_uint32_t           ocr,
                                rt_uint32_t          *rocr)
{
    struct rt_mmcsd_cmd cmd;
    rt_uint32_t i;
    rt_err_t err = RT_EOK;

    rt_memset(&cmd, 0, sizeof(struct rt_mmcsd_cmd));

    cmd.cmd_code = SD_APP_OP_COND;
    if (controller_is_spi(host))
        cmd.arg = ocr & (1 << 30); /* SPI only defines one bit */
    else
        cmd.arg = ocr;
    cmd.flags = RESP_SPI_R1 | RESP_R3 | CMD_BCR;

    for (i = 500; i; i--)
    {
        err = mmcsd_send_app_cmd(host, RT_NULL, &cmd, 3);
        if (err)
            break;

        /* if we're just probing, do a single pass */
        if (ocr == 0)
            break;

        /* otherwise wait until reset completes */
        if (controller_is_spi(host)) 
        {
            if (!(cmd.resp[0] & R1_SPI_IDLE))
                break;
        } 
        else 
        {
            if (cmd.resp[0] & CARD_BUSY)
                break;
        }

        err = -RT_ETIMEOUT;

        mmcsd_delay_ms(5);
    }

    if (rocr && !controller_is_spi(host))
        *rocr = cmd.resp[0];

    return err;
}

/*
 * To support SD 2.0 cards, we must always invoke SD_SEND_IF_COND
 * before SD_APP_OP_COND. This command will harmlessly fail for
 * SD 1.0 cards.
 */
rt_err_t mmcsd_send_if_cond(struct rt_mmcsd_host *host, rt_uint32_t ocr)
{
    struct rt_mmcsd_cmd cmd;
    rt_err_t err;
    rt_uint8_t pattern;

    cmd.cmd_code = SD_SEND_IF_COND;
    cmd.arg = ((ocr & 0xFF8000) != 0) << 8 | 0xAA;
    cmd.flags = RESP_SPI_R7 | RESP_R7 | CMD_BCR;

    err = mmcsd_send_cmd(host, &cmd, 0);
    if (err)
        return err;

    if (controller_is_spi(host))
        pattern = cmd.resp[1] & 0xFF;
    else
        pattern = cmd.resp[0] & 0xFF;

    if (pattern != 0xAA)
        return -RT_ERROR;

    return RT_EOK;
}

rt_err_t mmcsd_get_card_addr(struct rt_mmcsd_host *host, rt_uint32_t *rca)
{
    rt_err_t err;
    struct rt_mmcsd_cmd cmd;

    rt_memset(&cmd, 0, sizeof(struct rt_mmcsd_cmd));

    cmd.cmd_code = SD_SEND_RELATIVE_ADDR;
    cmd.arg = 0;
    cmd.flags = RESP_R6 | CMD_BCR;

    err = mmcsd_send_cmd(host, &cmd, 3);
    if (err)
        return err;

    *rca = cmd.resp[0] >> 16;

    return RT_EOK;
}

#define be32_to_cpu(x) ((rt_uint32_t)(              \
    (((rt_uint32_t)(x) & (rt_uint32_t)0x000000ffUL) << 24) |        \
    (((rt_uint32_t)(x) & (rt_uint32_t)0x0000ff00UL) <<  8) |        \
    (((rt_uint32_t)(x) & (rt_uint32_t)0x00ff0000UL) >>  8) |        \
    (((rt_uint32_t)(x) & (rt_uint32_t)0xff000000UL) >> 24)))

static rt_int32_t mmcsd_get_ssr(struct rt_mmcsd_card *card, rt_uint8_t *ssr)
{
    rt_int32_t err;
    struct rt_mmcsd_req req;
    struct rt_mmcsd_cmd cmd;
    struct rt_mmcsd_data data;

    err = mmcsd_app_cmd(card->host, card);
    if (err)
        return err;

    rt_memset(&req, 0, sizeof(struct rt_mmcsd_req));
    rt_memset(&cmd, 0, sizeof(struct rt_mmcsd_cmd));
    rt_memset(&data, 0, sizeof(struct rt_mmcsd_data));

    req.cmd = &cmd;
    req.data = &data;

    cmd.cmd_code = SD_APP_SD_STATUS;
    cmd.arg = 0;
    cmd.flags = RESP_SPI_R1 | RESP_R1 | CMD_ADTC;

    data.blksize = 64;
    data.blks = 1;
    data.flags = DATA_DIR_READ;
    data.buf = (rt_uint32_t *)ssr;

    mmcsd_set_data_timeout(&data, card);

    mmcsd_send_request(card->host, &req);

    if (cmd.err)
        return cmd.err;
    if (data.err)
        return data.err;

    return 0;
}

rt_int32_t mmcsd_get_scr(struct rt_mmcsd_card *card, rt_uint32_t *scr)
{
    rt_int32_t err;
    struct rt_mmcsd_req req;
    struct rt_mmcsd_cmd cmd;
    struct rt_mmcsd_data data;

    err = mmcsd_app_cmd(card->host, card);
    if (err)
        return err;

    rt_memset(&req, 0, sizeof(struct rt_mmcsd_req));
    rt_memset(&cmd, 0, sizeof(struct rt_mmcsd_cmd));
    rt_memset(&data, 0, sizeof(struct rt_mmcsd_data));

    req.cmd = &cmd;
    req.data = &data;

    cmd.cmd_code = SD_APP_SEND_SCR;
    cmd.arg = 0;
    cmd.flags = RESP_SPI_R1 | RESP_R1 | CMD_ADTC;

    data.blksize = 8;
    data.blks = 1;
    data.flags = DATA_DIR_READ;
    data.buf = scr;

    mmcsd_set_data_timeout(&data, card);

    mmcsd_send_request(card->host, &req);

    if (cmd.err)
        return cmd.err;
    if (data.err)
        return data.err;

    scr[0] = be32_to_cpu(scr[0]);
    scr[1] = be32_to_cpu(scr[1]);

    return 0;
}


/* UHS-I needs 1.8 V signaling, a 4-bit bus and a host that can switch. */
static rt_bool_t mmcsd_sd_host_uhs(struct rt_mmcsd_host *host)
{
    return !controller_is_spi(host) && (host->flags & MMCSD_SUP_UHS) &&
           (host->flags & MMCSD_BUSWIDTH_4) && host->ops->switch_signal_voltage;
}

/*
 * CMD11 voltage switch sequence (SD Physical Layer 4.2.4.2). On failure the
 * card may be left between the two levels and must be power cycled.
 */
static rt_int32_t mmcsd_sd_switch_voltage(struct rt_mmcsd_host *host)
{
    struct rt_mmcsd_cmd cmd;
    rt_uint32_t clock = host->io_cfg.clock;
    rt_int32_t err;

    rt_memset(&cmd, 0, sizeof(struct rt_mmcsd_cmd));
    cmd.cmd_code = SD_SWITCH_VOLTAGE;
    cmd.arg = 0;
    cmd.flags = RESP_R1 | CMD_AC;
    err = mmcsd_send_cmd(host, &cmd, 0);
    if (err)
        return err;
    if (cmd.resp[0] & R1_ERROR)
        return -RT_EIO;

    /* The card drives CMD and DAT[3:0] low right after the response. */
    mmcsd_delay_ms(1);
    if (host->ops->card_busy && !host->ops->card_busy(host))
        return -RT_EIO;

    /* Stop the clock for at least 5 ms while the signaling changes. */
    mmcsd_set_clock(host, 0);
    err = mmcsd_set_signal_voltage(host, MMCSD_SIGNAL_VOLTAGE_180);
    if (err)
        return err;
    mmcsd_delay_ms(10);
    mmcsd_set_clock(host, clock);

    /* A card that switched releases DAT[3:0] within 1 ms. */
    mmcsd_delay_ms(1);
    if (host->ops->card_busy && host->ops->card_busy(host))
        return -RT_EIO;
    return RT_EOK;
}

/* CMD6 for function group 1 (bus speed mode), others unchanged. */
static rt_int32_t mmcsd_sd_switch_func(struct rt_mmcsd_card *card, rt_bool_t set,
                                       rt_uint8_t func, rt_uint8_t *status)
{
    struct rt_mmcsd_req req;
    struct rt_mmcsd_cmd cmd;
    struct rt_mmcsd_data data;

    rt_memset(&cmd, 0, sizeof(struct rt_mmcsd_cmd));
    cmd.cmd_code = SD_SWITCH;
    cmd.arg = (set ? 0x80000000 : 0) | 0x00FFFFF0 | (func & 0xF);
    cmd.flags = RESP_R1 | CMD_ADTC;

    rt_memset(&data, 0, sizeof(struct rt_mmcsd_data));
    mmcsd_set_data_timeout(&data, card);
    data.blksize = 64;
    data.blks = 1;
    data.flags = DATA_DIR_READ;
    data.buf = (rt_uint32_t *)status;

    rt_memset(&req, 0, sizeof(struct rt_mmcsd_req));
    req.cmd = &cmd;
    req.data = &data;
    mmcsd_send_request(card->host, &req);

    if (cmd.err)
        return cmd.err;
    return data.err;
}

static const struct
{
    rt_uint32_t host_flag;  /* 0: every UHS host */
    rt_uint8_t  func;       /* function group 1 */
    rt_uint32_t timing;
    rt_uint32_t clock;
} sd_uhs_modes[] =
{
    { MMCSD_SUP_SDR104, 3, MMCSD_TIMING_UHS_SDR104, 208000000 },
    { MMCSD_SUP_SDR50,  2, MMCSD_TIMING_UHS_SDR50,  100000000 },
    { MMCSD_SUP_DDR50,  4, MMCSD_TIMING_UHS_DDR50,   50000000 },
    { 0,                1, MMCSD_TIMING_UHS_SDR25,   50000000 },
    { 0,                0, MMCSD_TIMING_UHS_SDR12,   25000000 },
};

/*
 * Select the fastest bus speed mode both sides support, with the bus already
 * 4 bits wide. A mode whose switch or tuning fails falls back to the next.
 */
static rt_int32_t mmcsd_sd_init_uhs(struct rt_mmcsd_card *card)
{
    struct rt_mmcsd_host *host = card->host;
    rt_uint8_t *status;
    rt_uint32_t i, clock;
    rt_int32_t err;

    status = (rt_uint8_t *)rt_malloc(64);
    if (!status)
        return -RT_ENOMEM;

    err = mmcsd_sd_switch_func(card, RT_FALSE, 0xF, status);
    if (err)
        goto out;

    for (i = 0; i < sizeof(sd_uhs_modes) / sizeof(sd_uhs_modes[0]); i++)
    {
        rt_bool_t tune;

        if (sd_uhs_modes[i].host_flag && !(host->flags & sd_uhs_modes[i].host_flag))
            continue;
        /* Group 1 support bits: byte 13, bit n for function n. */
        if (!(status[13] & (1U << sd_uhs_modes[i].func)))
            continue;

        err = mmcsd_sd_switch_func(card, RT_TRUE, sd_uhs_modes[i].func, status);
        if (err)
            goto out;
        if ((status[16] & 0xF) != sd_uhs_modes[i].func)
            continue;

        clock = sd_uhs_modes[i].clock;
        if (host->freq_max && clock > host->freq_max)
            clock = host->freq_max;
        mmcsd_set_timing(host, sd_uhs_modes[i].timing);
        mmcsd_set_clock(host, clock);
        card->hs_max_data_rate = clock;

        tune = sd_uhs_modes[i].timing == MMCSD_TIMING_UHS_SDR104 ||
               (sd_uhs_modes[i].timing == MMCSD_TIMING_UHS_SDR50 &&
                (host->flags & MMCSD_SDR50_TUNING));
        if (tune && mmcsd_excute_tuning(card) != RT_EOK)
        {
            LOG_W("%s: tuning failed, trying a slower mode", host->name);
            /* Back to a clock the untuned bus handles, then re-read the
             * support bits that the switch replaced in the status. */
            mmcsd_set_timing(host, MMCSD_TIMING_UHS_SDR12);
            mmcsd_set_clock(host, 25000000);
            err = mmcsd_sd_switch_func(card, RT_FALSE, 0xF, status);
            if (err)
                goto out;
            continue;
        }
        if (sd_uhs_modes[i].func)
            card->flags |= CARD_FLAG_HIGHSPEED;
        err = RT_EOK;
        goto out;
    }
    /* SDR12 (function 0) is the default and always supported. */
    err = -RT_ERROR;

out:
    rt_free(status);
    return err;
}

static rt_int32_t mmcsd_sd_set_wide_bus(struct rt_mmcsd_card *card)
{
    struct rt_mmcsd_host *host = card->host;
    rt_int32_t err;

    if (!(host->flags & MMCSD_BUSWIDTH_4) ||
        !(card->scr.sd_bus_widths & SD_SCR_BUS_WIDTH_4))
        return RT_EOK;
    err = mmcsd_app_set_bus_width(card, MMCSD_BUS_WIDTH_4);
    if (!err)
        mmcsd_set_bus_width(host, MMCSD_BUS_WIDTH_4);
    return err;
}

static const char *mmcsd_sd_timing_name(struct rt_mmcsd_host *host)
{
    switch (host->io_cfg.timing)
    {
    case MMCSD_TIMING_UHS_SDR104: return "UHS SDR104";
    case MMCSD_TIMING_UHS_SDR50:  return "UHS SDR50";
    case MMCSD_TIMING_UHS_DDR50:  return "UHS DDR50";
    case MMCSD_TIMING_UHS_SDR25:  return "UHS SDR25";
    case MMCSD_TIMING_UHS_SDR12:  return "UHS SDR12";
    default:
        return (host->card->flags & CARD_FLAG_HIGHSPEED) ? "SD HS" : "legacy";
    }
}

static rt_int32_t mmcsd_sd_init_card(struct rt_mmcsd_host *host,
                                     rt_uint32_t           ocr)
{
    struct rt_mmcsd_card *card;
    rt_int32_t err;
    rt_uint32_t resp[4];
    rt_uint32_t max_data_rate;
    rt_uint8_t ssr[64];
    rt_uint32_t rocr = 0;
    rt_bool_t uhs = mmcsd_sd_host_uhs(host);

retry:
    mmcsd_go_idle(host);

    /*
     * If SD_SEND_IF_COND indicates an SD 2.0
     * compliant card and we should set bit 30
     * of the ocr to indicate that we can handle
     * block-addressed SDHC cards. Such a card
     * may also switch to 1.8 V signaling.
     */
    err = mmcsd_send_if_cond(host, ocr);
    if (!err)
    {
        ocr |= SD_OCR_CCS;
        if (uhs)
            ocr |= SD_OCR_S18R;
    }

    err = mmcsd_send_app_op_cond(host, ocr, &rocr);
    if (err)
        goto err;

    if ((ocr & SD_OCR_S18R) &&
        (rocr & (SD_OCR_CCS | SD_OCR_S18R)) == (SD_OCR_CCS | SD_OCR_S18R))
    {
        err = mmcsd_sd_switch_voltage(host);
        if (err)
        {
            LOG_W("%s: 1.8 V switch failed (%d), continuing at 3.3 V",
                  host->name, err);
            mmcsd_power_cycle(host);
            ocr &= ~SD_OCR_S18R;
            uhs = RT_FALSE;
            goto retry;
        }
    }

    if (controller_is_spi(host))
        err = mmcsd_get_cid(host, resp);
    else
        err = mmcsd_all_get_cid(host, resp);
    if (err)
        goto err;

    card = rt_malloc(sizeof(struct rt_mmcsd_card));
    if (!card) 
    {
        LOG_E("malloc card failed!");
        err = -RT_ENOMEM;
        goto err;
    }
    rt_memset(card, 0, sizeof(struct rt_mmcsd_card));

    card->card_type = CARD_TYPE_SD;
    card->host = host;
    rt_memcpy(card->resp_cid, resp, sizeof(card->resp_cid));

    /*
     * For native busses:  get card RCA and quit open drain mode.
     */
    if (!controller_is_spi(host)) 
    {
        err = mmcsd_get_card_addr(host, &card->rca);
        if (err)
            goto err1;

        mmcsd_set_bus_mode(host, MMCSD_BUSMODE_PUSHPULL);
    }

    err = mmcsd_get_csd(card, card->resp_csd);
    if (err)
        goto err1;

    err = mmcsd_parse_csd(card);
    if (err)
        goto err1;

    if (!controller_is_spi(host)) 
    {
        err = mmcsd_select_card(card);
        if (err)
            goto err1;
    }

    err = mmcsd_get_scr(card, card->resp_scr);
    if (err)
        goto err1;

    mmcsd_parse_scr(card);

    rt_memset(ssr, 0, sizeof(ssr));
    if (mmcsd_get_ssr(card, ssr) == RT_EOK)
    {
        mmcsd_parse_ssr(card, ssr);
    }

    if (controller_is_spi(host)) 
    {
        err = mmcsd_spi_use_crc(host, 1);
        if (err)
            goto err1;
    }

    if (host->io_cfg.signal_voltage == MMCSD_SIGNAL_VOLTAGE_180)
    {
        /* UHS-I: the bus speed mode is selected on the 4-bit bus. */
        err = mmcsd_sd_set_wide_bus(card);
        if (!err)
            err = mmcsd_sd_init_uhs(card);
        if (err)
        {
            /* The card leaves 1.8 V only by losing power. */
            LOG_W("%s: UHS-I setup failed (%d), continuing at 3.3 V",
                  host->name, err);
            rt_free(card);
            mmcsd_set_timing(host, MMCSD_TIMING_LEGACY);
            mmcsd_power_cycle(host);
            ocr &= ~SD_OCR_S18R;
            uhs = RT_FALSE;
            goto retry;
        }
        host->card = card;
        return 0;
    }

    /*
     * change SD card to high-speed, only SD2.0 spec
     */
    err = mmcsd_switch(card);
    if (err)
        goto err1;

    /* set bus speed */
    max_data_rate = (unsigned int)-1;

    if (card->flags & CARD_FLAG_HIGHSPEED) 
    {
        if (max_data_rate > card->hs_max_data_rate)
            max_data_rate = card->hs_max_data_rate;
    } 
    else if (max_data_rate > card->max_data_rate) 
    {
        max_data_rate = card->max_data_rate;
    }

    mmcsd_set_clock(host, max_data_rate);

    /*switch bus width*/
    err = mmcsd_sd_set_wide_bus(card);
    if (err)
        goto err1;

    host->card = card;

    return 0;

err1:
    rt_free(card);
err:

    return err;
}

/*
 * Starting point for SD card init.
 */
rt_int32_t init_sd(struct rt_mmcsd_host *host, rt_uint32_t ocr)
{
    rt_int32_t err;
    rt_err_t remove_err;
    rt_uint32_t  current_ocr;
    /*
     * We need to get OCR a different way for SPI.
     */
    if (controller_is_spi(host))
    {
        mmcsd_go_idle(host);

        err = mmcsd_spi_read_ocr(host, 0, &ocr);
        if (err)
            goto err;
    }

    if (ocr & VDD_165_195)
    {
        LOG_I(" SD card claims to support the "
               "incompletely defined 'low voltage range'. This "
               "will be ignored.");
        ocr &= ~VDD_165_195;
    }

    current_ocr = mmcsd_select_voltage(host, ocr);

    /*
     * Can we support the voltage(s) of the card(s)?
     */
    if (!current_ocr)
    {
        err = -RT_ERROR;
        goto err;
    }

    /*
     * Detect and init the card.
     */
    err = mmcsd_sd_init_card(host, current_ocr);
    if (err)
        goto err;

    mmcsd_host_unlock(host);

    err = rt_mmcsd_blk_probe(host->card);
    if (err)
        goto remove_card;
    mmcsd_host_lock(host);

    LOG_I("%s: selected timing: %s, %u-bit, clock=%u Hz",
          host->name,
          mmcsd_sd_timing_name(host),
          1U << host->io_cfg.bus_width, host->io_cfg.clock);

    return 0;

remove_card:
    remove_err = rt_mmcsd_blk_remove(host->card);
    mmcsd_host_lock(host);
    if (remove_err != RT_EOK)
    {
        err = remove_err;
        goto err;
    }
    rt_free(host->card);
    host->card = RT_NULL;
err:

    LOG_D("init SD card failed!");

    return err;
}

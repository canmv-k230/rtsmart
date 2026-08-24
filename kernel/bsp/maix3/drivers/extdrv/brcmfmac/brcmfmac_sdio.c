// SPDX-License-Identifier: ISC
/* SDIO backplane and SDPCM transport, derived from Linux 6.6 brcmfmac. */
#include "brcmfmac.h"

#include <dfs_posix.h>

#define DBG_TAG "brcmf.sdio"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define BRCMF_SDIO_VENDOR_CYPRESS       0x04b4U
#define BRCMF_AUTO_START_DELAY_MS        100U
#define SBSDIO_SBADDRLOW                0x1000aU
#define SBSDIO_SBADDRMID                0x1000bU
#define SBSDIO_SBADDRHIGH               0x1000cU
#define SBSDIO_CHIPCLKCSR               0x1000eU
#define SBSDIO_SDIOPULLUP               0x1000fU
#define SBSDIO_WAKEUPCTRL               0x1001eU
#define SBSDIO_SLEEPCSR                 0x1001fU
#define SBSDIO_WATERMARK                0x10008U
#define SBSDIO_WINDOW_MASK              0xffff8000U
#define SBSDIO_OFFSET_MASK              0x00007fffU
#define SBSDIO_ACCESS_32BIT             0x00008000U
#define SBSDIO_WINDOW_SIZE              0x00008000U
#define SBSDIO_FORCE_ALP                0x01U
#define SBSDIO_FORCE_HT                 0x02U
#define SBSDIO_ALP_AVAIL_REQ            0x08U
#define SBSDIO_HT_AVAIL_REQ             0x10U
#define SBSDIO_FORCE_HW_CLKREQ_OFF      0x20U
#define SBSDIO_ALP_AVAIL                0x40U
#define SBSDIO_HT_AVAIL                 0x80U
#define SBSDIO_KSO                      0x01U
#define SBSDIO_WAKE_HT                  0x02U
#define SBSDIO_DEVICE_CTL               0x10009U
#define SBSDIO_DEVICE_CTL_F2WM_ENABLE   0x10U
#define SBSDIO_FRAMECTRL                0x1000dU
#define SBSDIO_WFRAMEBCLO               0x10019U
#define SBSDIO_WFRAMEBCHI               0x1001aU
#define SBSDIO_RFRAMEBCLO               0x1001bU
#define SBSDIO_RFRAMEBCHI               0x1001cU
#define SBSDIO_MESBUSYCTRL              0x1001dU
#define SBSDIO_MESBUSYCTRL_ENABLE       0x80U
#define SBSDIO_FRAMECTRL_RF_TERM        (1U << 0)
#define SBSDIO_FRAMECTRL_WF_TERM        (1U << 1)

#define SDIO_INT_STATUS_OFFSET          0x20U
#define SDIO_INT_HOST_MASK_OFFSET       0x24U
#define SDIO_FUNCTION_INT_MASK_OFFSET   0x34U
#define SDIO_TO_SB_MAILBOX_OFFSET       0x40U
#define SDIO_TO_SB_MAILBOX_DATA_OFFSET  0x48U
#define SDIO_TO_HOST_MAILBOX_DATA_OFFSET 0x4cU
#define SDIO_SB_MAILBOX_NAK             (1U << 0)
#define SDIO_SB_MAILBOX_INT_ACK         (1U << 1)
#define SDIO_HOST_FC_STATE              (1U << 4)
#define SDIO_HOST_FC_CHANGE             (1U << 5)
#define SDIO_HOST_FRAME_INDICATION      (1U << 6)
#define SDIO_HOST_MAILBOX_INDICATION    (1U << 7)
#define SDIO_HOST_MAILBOX_MASK          0x000000f0U
#define SDIO_HOST_DATA_NAK_HANDLED      0x00000001U
#define SDIO_HOST_DATA_DEVICE_READY     0x00000002U
#define SDIO_HOST_DATA_FLOW_CONTROL     0x00000004U
#define SDIO_HOST_DATA_FIRMWARE_READY   0x00000008U
#define SDIO_HOST_DATA_FIRMWARE_HALT    0x00000010U
#define SDIO_HOST_DATA_VERSION_MASK     0x00ff0000U
#define SDIO_HOST_DATA_VERSION_SHIFT    16U
#define SDIO_HOST_DATA_FLOW_MASK        0xff000000U
#define SDIO_HOST_DATA_FLOW_SHIFT       24U
#define SDPCM_PROTOCOL_VERSION          4U
#define SDPCM_PROTOCOL_VERSION_SHIFT    16U
#define SDIO_F2_WATERMARK               8U

#define SDPCM_HEADER_SIZE               12U
#define SDPCM_HW_HEADER_SIZE             4U
#define SDPCM_HW_EXTENSION_SIZE          8U
#define SDPCM_DATA_PAD                  2U
#define SDPCM_TX_ALIGNMENT             32U
#define SDPCM_CHANNEL_MASK              0x0fU
#define SDPCM_GLOM_CHANNEL              3U
#define SDPCM_GLOM_DESCRIPTOR           0x80U
#define SDPCM_NEXT_LENGTH_UNIT          16U
#define SDPCM_FIRST_READ                64U
#define SDPCM_MAX_RX_FRAMES             64U
#define BRCMF_PATH_MAX                  256U
#define BRCMF_NVRAM_MAX                 64000U
#define BRCMF_IO_CHUNK                  2048U
#define BRCMF_TX_RECORD_CAPACITY        (sizeof(struct brcmf_bus_record) + \
                                         sizeof(struct brcmf_bcdc_header) + \
                                         BRCMF_MAX_FRAME)
#define BRCMF_TX_POOL_DEPTH             (BRCMFMAC_TX_QUEUE_DEPTH + \
                                         BRCMFMAC_TX_GLOM_FRAMES)

#ifdef RT_WLAN_MANAGE_ENABLE
static struct rt_workqueue *g_brcmf_sdio_init_workqueue;
#endif

struct brcmf_tx_record
{
    rt_uint16_t length;
    rt_uint8_t data[BRCMF_TX_RECORD_CAPACITY];
};

static const struct brcmf_firmware_mapping g_brcmf_firmware[] = {
    {0x02d0, 0xa9a6, 43430, 0x00000002U, "BCM43430A1",
     "fw_bcm43438a1.bin", BRCMFMAC_BCM43430A1_NVRAM, RT_NULL},
    {0x02d0, 0xa9a6, 43430, 0x00000001U, "BCM43430A0",
     "brcmfmac43430a0-sdio.bin", "brcmfmac43430a0-sdio.txt", RT_NULL},
    {0x02d0, 0xa9a6, 43430, 0xfffffffcU, "BCM43430B0",
     "brcmfmac43430b0-sdio.bin", "brcmfmac43430b0-sdio.txt", RT_NULL},
    {0x02d0, 0xa9af, 43439, 0xffffffffU, "BCM43439",
     "brcmfmac43439-sdio.bin", "brcmfmac43439-sdio.txt",
     "brcmfmac43439-sdio.clm_blob"},
    {0x02d0, 0x4345, 0x4345, 0x00000200U, "BCM43456",
     "brcmfmac43456-sdio.bin", "brcmfmac43456-sdio.txt",
     "brcmfmac43456-sdio.clm_blob"},
    {0x02d0, 0xa9bf, 0x4345, 0xfffffdc0U, "BCM43455",
     "brcmfmac43455-sdio.bin", "brcmfmac43455-sdio.txt",
     "brcmfmac43455-sdio.clm_blob"},
    {0x02d0, 0xa9bf, 43454, 0x00000040U, "BCM43455",
     "brcmfmac43455-sdio.bin", "brcmfmac43455-sdio.txt",
     "brcmfmac43455-sdio.clm_blob"},
    {0x02d0, 0x4335, 0x4339, 0xffffffffU, "BCM4339",
     "brcmfmac4339-sdio.bin", "brcmfmac4339-sdio.txt", RT_NULL},
    {0x02d0, 0x4339, 0x4339, 0xffffffffU, "BCM4339",
     "brcmfmac4339-sdio.bin", "brcmfmac4339-sdio.txt", RT_NULL},
    {0x02d0, 0x4354, 0x4354, 0xffffffffU, "BCM4354",
     "brcmfmac4354-sdio.bin", "brcmfmac4354-sdio.txt",
     "brcmfmac4354-sdio.clm_blob"},
    {0x02d0, 0x4356, 0x4356, 0xffffffffU, "BCM4356",
     "brcmfmac4356-sdio.bin", "brcmfmac4356-sdio.txt",
     "brcmfmac4356-sdio.clm_blob"},
    {0x02d0, 0x4359, 0x4359, 0xffffffffU, "BCM4359",
     "brcmfmac4359-sdio.bin", "brcmfmac4359-sdio.txt",
     "brcmfmac4359-sdio.clm_blob"},
    {0x02d0, 0x4373, 0x4373, 0xffffffffU, "CYW4373",
     "brcmfmac4373-sdio.bin", "brcmfmac4373-sdio.txt",
     "brcmfmac4373-sdio.clm_blob"},
    {0x02d0, 0xa804, 43012, 0xffffffffU, "CYW43012",
     "brcmfmac43012-sdio.bin", "brcmfmac43012-sdio.txt",
     "brcmfmac43012-sdio.clm_blob"},
    {0x02d0, 0xaae8, 43752, 0xffffffffU, "CYW43752",
     "brcmfmac43752-sdio.bin", "brcmfmac43752-sdio.txt",
     "brcmfmac43752-sdio.clm_blob"},
    {0x04b4, 0xbd3d, 43439, 0xffffffffU, "CYW43439",
     "brcmfmac43439-sdio.bin", "brcmfmac43439-sdio.txt",
     "brcmfmac43439-sdio.clm_blob"},
};

const struct brcmf_firmware_mapping *brcmf_firmware_find(
    rt_uint16_t vendor, rt_uint16_t device, rt_uint32_t chip,
    rt_uint32_t revision)
{
    rt_size_t index;

    for (index = 0; index < sizeof(g_brcmf_firmware) /
                                sizeof(g_brcmf_firmware[0]); index++)
    {
        const struct brcmf_firmware_mapping *mapping =
            &g_brcmf_firmware[index];

        if (mapping->vendor == vendor &&
            mapping->device == device && mapping->chip == chip &&
            revision < 32U && (mapping->revision_mask & (1UL << revision)))
        {
            return mapping;
        }
    }
    return RT_NULL;
}

static rt_err_t brcmf_sdio_result(rt_int32_t result)
{
    return result == RT_EOK ? RT_EOK : -RT_EIO;
}

static rt_uint16_t brcmf_sdio_f2_block_size(rt_uint16_t device)
{
    switch (device)
    {
    case 0xa9a6U:
    case 0x4373U:
    case 0x4359U:
    case 0x4354U:
    case 0x4356U:
        return 256U;
    case 0x4329U:
        return 128U;
    default:
        return BRCMFMAC_SDIO_F2_BLOCK_SIZE;
    }
}

static rt_err_t brcmf_sdio_set_window_locked(struct brcmf_context *context,
                                              rt_uint32_t address)
{
    rt_uint32_t window = address & SBSDIO_WINDOW_MASK;
    rt_int32_t result;

    if (context->backplane_window == window)
    {
        return RT_EOK;
    }
    result = sdio_io_writeb(context->function1, SBSDIO_SBADDRLOW,
                            (window >> 8) & 0xffU);
    if (result == RT_EOK)
    {
        result = sdio_io_writeb(context->function1, SBSDIO_SBADDRMID,
                                (window >> 16) & 0xffU);
    }
    if (result == RT_EOK)
    {
        result = sdio_io_writeb(context->function1, SBSDIO_SBADDRHIGH,
                                (window >> 24) & 0xffU);
    }
    if (result == RT_EOK)
    {
        context->backplane_window = window;
    }
    return brcmf_sdio_result(result);
}

static rt_err_t brcmf_sdio_backplane_transfer(
    struct brcmf_context *context, rt_bool_t write, rt_uint32_t address,
    void *data, rt_size_t length)
{
    rt_uint8_t *bytes = data;
    rt_err_t result;

    if (!context || !data || !length)
    {
        return -RT_EINVAL;
    }
    result = rt_mutex_take(&context->io_mutex, RT_WAITING_FOREVER);
    if (result != RT_EOK)
    {
        return result;
    }
    mmcsd_host_lock(context->card->host);
    while (length)
    {
        rt_size_t window_left = SBSDIO_WINDOW_SIZE -
                                (address & SBSDIO_OFFSET_MASK);
        rt_size_t chunk = length < window_left ? length : window_left;
        rt_uint32_t io_address;

        if (chunk > BRCMF_IO_CHUNK)
        {
            chunk = BRCMF_IO_CHUNK;
        }
        result = brcmf_sdio_set_window_locked(context, address);
        if (result != RT_EOK)
        {
            break;
        }
        io_address = (address & SBSDIO_OFFSET_MASK) | SBSDIO_ACCESS_32BIT;
        if (write)
        {
            result = brcmf_sdio_result(sdio_io_write_multi_incr_b(
                context->function1, io_address, bytes, chunk));
        }
        else
        {
            result = brcmf_sdio_result(sdio_io_read_multi_incr_b(
                context->function1, io_address, bytes, chunk));
        }
        if (result != RT_EOK)
        {
            break;
        }
        address += chunk;
        bytes += chunk;
        length -= chunk;
    }
    mmcsd_host_unlock(context->card->host);
    rt_mutex_release(&context->io_mutex);
    return result;
}

rt_err_t brcmf_sdio_backplane_read(struct brcmf_context *context,
                                   rt_uint32_t address, void *data,
                                   rt_size_t length)
{
    return brcmf_sdio_backplane_transfer(context, RT_FALSE, address,
                                          data, length);
}

rt_err_t brcmf_sdio_backplane_write(struct brcmf_context *context,
                                    rt_uint32_t address, const void *data,
                                    rt_size_t length)
{
    return brcmf_sdio_backplane_transfer(context, RT_TRUE, address,
                                          (void *)data, length);
}

rt_uint32_t brcmf_sdio_read32(struct brcmf_context *context,
                              rt_uint32_t address, rt_err_t *error)
{
    rt_uint8_t data[4];
    rt_err_t result = brcmf_sdio_backplane_read(context, address, data,
                                                 sizeof(data));

    if (error)
    {
        *error = result;
    }
    return result == RT_EOK ? brcmf_get_le32(data) : 0xffffffffU;
}

rt_err_t brcmf_sdio_write32(struct brcmf_context *context,
                            rt_uint32_t address, rt_uint32_t value)
{
    rt_uint8_t data[4];

    brcmf_put_le32(data, value);
    return brcmf_sdio_backplane_write(context, address, data, sizeof(data));
}

static rt_err_t brcmf_sdio_f1_writeb(struct brcmf_context *context,
                                     rt_uint32_t address, rt_uint8_t value)
{
    rt_err_t result;

    rt_mutex_take(&context->io_mutex, RT_WAITING_FOREVER);
    mmcsd_host_lock(context->card->host);
    result = brcmf_sdio_result(sdio_io_writeb(context->function1,
                                               address, value));
    mmcsd_host_unlock(context->card->host);
    rt_mutex_release(&context->io_mutex);
    return result;
}

static rt_uint8_t brcmf_sdio_f1_readb(struct brcmf_context *context,
                                      rt_uint32_t address, rt_err_t *error)
{
    rt_int32_t io_result;
    rt_uint8_t value;

    rt_mutex_take(&context->io_mutex, RT_WAITING_FOREVER);
    mmcsd_host_lock(context->card->host);
    value = sdio_io_readb(context->function1, address, &io_result);
    mmcsd_host_unlock(context->card->host);
    rt_mutex_release(&context->io_mutex);
    *error = brcmf_sdio_result(io_result);
    return value;
}

static rt_err_t brcmf_sdio_configure_watermark(
    struct brcmf_context *context)
{
    rt_uint16_t device = context->function1->product;
    rt_uint8_t watermark = SDIO_F2_WATERMARK;
    rt_uint8_t mesbusy = 0;
    rt_uint8_t device_control;
    rt_err_t result;

    switch (device)
    {
    case 0x4373U:
    case 0xaae8U:
        watermark = 0x40U;
        mesbusy = 0x40U | SBSDIO_MESBUSYCTRL_ENABLE;
        break;
    case 0xa804U:
    case 0xa9bfU:
        watermark = 0x60U;
        mesbusy = 0x50U | SBSDIO_MESBUSYCTRL_ENABLE;
        break;
    case 0x4329U:
    case 0x4335U:
    case 0x4339U:
        watermark = 48U;
        mesbusy = 80U | SBSDIO_MESBUSYCTRL_ENABLE;
        break;
    case 0x4354U:
    case 0x4356U:
    case 0x4359U:
        watermark = 0x40U;
        mesbusy = 0x40U | SBSDIO_MESBUSYCTRL_ENABLE;
        break;
    default:
        return brcmf_sdio_f1_writeb(
            context, SBSDIO_WATERMARK, watermark);
    }

    result = brcmf_sdio_f1_writeb(
        context, SBSDIO_WATERMARK, watermark);
    if (result != RT_EOK)
    {
        return result;
    }
    device_control = brcmf_sdio_f1_readb(
        context, SBSDIO_DEVICE_CTL, &result);
    if (result != RT_EOK)
    {
        return result;
    }
    result = brcmf_sdio_f1_writeb(
        context, SBSDIO_DEVICE_CTL,
        device_control | SBSDIO_DEVICE_CTL_F2WM_ENABLE);
    if (result != RT_EOK)
    {
        return result;
    }
    return brcmf_sdio_f1_writeb(
        context, SBSDIO_MESBUSYCTRL, mesbusy);
}

static rt_err_t brcmf_sdio_wait_clock(struct brcmf_context *context,
                                      rt_uint8_t mask, rt_uint32_t timeout_ms)
{
    rt_uint32_t count;

    for (count = 0; count < timeout_ms; count++)
    {
        rt_err_t result;
        rt_uint8_t value = brcmf_sdio_f1_readb(
            context, SBSDIO_CHIPCLKCSR, &result);

        if (result != RT_EOK)
        {
            return result;
        }
        if ((value & mask) == mask)
        {
            return RT_EOK;
        }
        rt_thread_mdelay(1);
    }
    return -RT_ETIMEOUT;
}

rt_err_t brcmf_firmware_load(const char *name, rt_uint8_t **data,
                             rt_size_t *length, rt_bool_t required)
{
    char path[BRCMF_PATH_MAX];
    off_t size;
    int descriptor;
    rt_uint8_t *buffer;
    rt_size_t offset = 0;

    rt_snprintf(path, sizeof(path), "%s/%s", BRCMFMAC_FIRMWARE_PATH, name);
    descriptor = open(path, O_RDONLY, 0);
    if (descriptor < 0)
    {
        if (required)
        {
            LOG_E("firmware not found: %s", path);
        }
        return -RT_ENOSYS;
    }
    size = lseek(descriptor, 0, SEEK_END);
    if (size <= 0 || lseek(descriptor, 0, SEEK_SET) < 0)
    {
        close(descriptor);
        return -RT_EIO;
    }
    buffer = rt_malloc(size);
    if (!buffer)
    {
        close(descriptor);
        return -RT_ENOMEM;
    }
    while (offset < (rt_size_t)size)
    {
        ssize_t count = read(descriptor, buffer + offset,
                             (rt_size_t)size - offset);

        if (count <= 0)
        {
            rt_free(buffer);
            close(descriptor);
            return -RT_EIO;
        }
        offset += count;
    }
    close(descriptor);
    *data = buffer;
    *length = size;
    return RT_EOK;
}

static rt_bool_t brcmf_nvram_valid_char(rt_uint8_t value)
{
    return value == '\t' || (value >= 0x20U && value <= 0x7eU);
}

static rt_err_t brcmf_prepare_nvram(const rt_uint8_t *source,
                                    rt_size_t source_length,
                                    rt_uint8_t **output,
                                    rt_size_t *output_length)
{
    rt_uint8_t *buffer;
    rt_size_t source_offset = 0;
    rt_size_t destination = 0;
    rt_bool_t line_start = RT_TRUE;
    rt_bool_t comment = RT_FALSE;
    rt_bool_t have_equal = RT_FALSE;
    rt_bool_t board_revision_found = RT_FALSE;
    rt_size_t line_start_offset = 0;
    rt_size_t allocation;
    rt_uint32_t words;
    rt_uint32_t token;

    if (!source || !source_length || source_length > BRCMF_NVRAM_MAX)
    {
        return -RT_EINVAL;
    }
    allocation = RT_ALIGN(source_length + 32U, 4U) + 4U;
    buffer = rt_calloc(1, allocation);
    if (!buffer)
    {
        return -RT_ENOMEM;
    }
    while (source_offset < source_length)
    {
        rt_uint8_t value = source[source_offset++];

        if (value == '\r')
        {
            continue;
        }
        if (line_start && (value == ' ' || value == '\t'))
        {
            continue;
        }
        if (line_start && value == '#')
        {
            comment = RT_TRUE;
        }
        if (value == '\n' || value == '\0')
        {
            if (!comment && have_equal && destination > line_start_offset)
            {
                while (destination > line_start_offset &&
                       (buffer[destination - 1U] == ' ' ||
                        buffer[destination - 1U] == '\t'))
                {
                    destination--;
                }
                if (destination - line_start_offset >= 9U &&
                    rt_memcmp(buffer + line_start_offset,
                              "boardrev=", 9U) == 0)
                {
                    board_revision_found = RT_TRUE;
                }
                buffer[destination++] = '\0';
            }
            else
            {
                destination = line_start_offset;
            }
            line_start_offset = destination;
            line_start = RT_TRUE;
            comment = RT_FALSE;
            have_equal = RT_FALSE;
            continue;
        }
        line_start = RT_FALSE;
        if (comment || !brcmf_nvram_valid_char(value))
        {
            continue;
        }
        if (value == '=')
        {
            have_equal = RT_TRUE;
        }
        buffer[destination++] = value;
    }
    if (!comment && have_equal && destination > line_start_offset)
    {
        if (destination - line_start_offset >= 9U &&
            rt_memcmp(buffer + line_start_offset, "boardrev=", 9U) == 0)
        {
            board_revision_found = RT_TRUE;
        }
        buffer[destination++] = '\0';
    }
    if (!destination)
    {
        rt_free(buffer);
        return -RT_EINVAL;
    }
    if (!board_revision_found)
    {
        static const char board_revision[] = "boardrev=0xff";

        rt_memcpy(buffer + destination, board_revision,
                  sizeof(board_revision));
        destination += sizeof(board_revision);
    }
    destination = RT_ALIGN(destination + 1U, 4U);
    words = destination / 4U;
    token = ((~words & 0xffffU) << 16) | (words & 0xffffU);
    brcmf_put_le32(buffer + destination, token);
    destination += 4U;
    *output = buffer;
    *output_length = destination;
    return RT_EOK;
}

static rt_err_t brcmf_sdio_download(struct brcmf_context *context)
{
    rt_uint8_t *firmware = RT_NULL;
    rt_uint8_t *nvram_text = RT_NULL;
    rt_uint8_t *nvram = RT_NULL;
    rt_size_t firmware_length = 0;
    rt_size_t nvram_text_length = 0;
    rt_size_t nvram_length = 0;
    rt_uint32_t reset_vector;
    rt_err_t result;

    result = brcmf_firmware_load(context->mapping->firmware, &firmware,
                                 &firmware_length, RT_TRUE);
    if (result != RT_EOK)
    {
        return result;
    }
    result = brcmf_firmware_load(context->mapping->nvram, &nvram_text,
                                 &nvram_text_length, RT_TRUE);
    if (result == RT_EOK)
    {
        result = brcmf_prepare_nvram(nvram_text, nvram_text_length,
                                     &nvram, &nvram_length);
    }
    if (result == RT_EOK &&
        (firmware_length < sizeof(reset_vector) ||
         firmware_length > context->chip.ramsize ||
         nvram_length > context->chip.ramsize - firmware_length))
    {
        result = -RT_EFULL;
    }
    if (result == RT_EOK)
    {
        reset_vector = brcmf_get_le32(firmware);
        result = brcmf_sdio_backplane_write(
            context, context->chip.rambase, firmware, firmware_length);
    }
#ifdef BRCMFMAC_VERIFY_FIRMWARE
    if (result == RT_EOK)
    {
        rt_uint8_t *verify = rt_malloc(BRCMF_IO_CHUNK);
        rt_size_t offset;

        if (!verify)
        {
            result = -RT_ENOMEM;
        }
        for (offset = 0; result == RT_EOK && offset < firmware_length;
             offset += BRCMF_IO_CHUNK)
        {
            rt_size_t length = firmware_length - offset;

            if (length > BRCMF_IO_CHUNK)
            {
                length = BRCMF_IO_CHUNK;
            }
            result = brcmf_sdio_backplane_read(
                context, context->chip.rambase + offset, verify, length);
            if (result == RT_EOK &&
                rt_memcmp(verify, firmware + offset, length) != 0)
            {
                result = -RT_EIO;
            }
        }
        rt_free(verify);
    }
#endif
    if (result == RT_EOK)
    {
        result = brcmf_sdio_backplane_write(
            context,
            context->chip.rambase + context->chip.ramsize - nvram_length,
            nvram, nvram_length);
    }
    if (result == RT_EOK)
    {
        result = brcmf_chip_set_active(context, reset_vector);
    }
    rt_free(nvram);
    rt_free(nvram_text);
    rt_free(firmware);
    return result;
}

static rt_size_t brcmf_sdio_transfer_length(struct brcmf_context *context,
                                             rt_size_t length)
{
    if (length > context->f2_block_size)
    {
        return RT_ALIGN(length, context->f2_block_size);
    }
    return RT_ALIGN(length, 4U);
}

static rt_err_t brcmf_sdio_parse_header(
    const rt_uint8_t *frame, rt_size_t available, rt_uint16_t *frame_length,
    rt_uint8_t *channel, rt_uint8_t *data_offset)
{
    rt_uint16_t length;
    rt_uint16_t checksum;

    if (!frame || available < SDPCM_HEADER_SIZE)
    {
        return -RT_EIO;
    }
    length = brcmf_get_le16(frame);
    checksum = brcmf_get_le16(frame + 2U);
    if (!length && !checksum)
    {
        return -RT_EEMPTY;
    }
    if ((rt_uint16_t)(length ^ checksum) != 0xffffU ||
        length < SDPCM_HEADER_SIZE || length > BRCMF_RX_BUFFER_SIZE ||
        frame[7] < SDPCM_HEADER_SIZE || frame[7] > length)
    {
        return -RT_EIO;
    }
    *frame_length = length;
    *channel = frame[5] & SDPCM_CHANNEL_MASK;
    *data_offset = frame[7];
    return RT_EOK;
}

static void brcmf_sdio_set_priority_flow_control(
    struct brcmf_context *context, rt_uint8_t flow_control)
{
    if (context->tx_flow_control != flow_control)
    {
        context->tx_flow_control = flow_control;
        context->tx_flow_control_count++;
        rt_sem_release(context->tx_sem);
    }
}

static void brcmf_sdio_set_global_flow_control(
    struct brcmf_context *context, rt_bool_t flow_control)
{
    if (context->tx_flow_control_state != flow_control)
    {
        context->tx_flow_control_state = flow_control;
        context->tx_flow_control_count++;
        rt_sem_release(context->tx_sem);
    }
}

static void brcmf_sdio_update_flow_control(struct brcmf_context *context,
                                            const rt_uint8_t *frame)
{
    rt_uint8_t tx_max = frame[9];

    rt_mutex_take(&context->io_mutex, RT_WAITING_FOREVER);
    context->rx_sequence = frame[4];
    brcmf_sdio_set_priority_flow_control(context, frame[8]);
    if ((rt_uint8_t)(tx_max - context->tx_sequence) > 0x70U)
    {
        context->tx_invalid_credit_count++;
        if (context->tx_invalid_credit_count <= 4U ||
            !(context->tx_invalid_credit_count &
              (context->tx_invalid_credit_count - 1U)))
        {
            LOG_W("invalid TX credit window seq=%u max=%u "
                  "(errors=%u)", context->tx_sequence, tx_max,
                  (unsigned int)context->tx_invalid_credit_count);
        }
        /* Ignore an implausible window exactly as the SDIO vendor driver
         * does; synthesizing credits can overrun the firmware queue. */
        tx_max = context->tx_max;
    }
    if (context->tx_max != tx_max)
    {
        context->tx_max = tx_max;
        rt_sem_release(context->tx_sem);
        if (context->control_waiting)
        {
            rt_sem_release(context->tx_sem);
        }
    }
    rt_mutex_release(&context->io_mutex);
}

static rt_err_t brcmf_sdio_save_glom_descriptor(
    struct brcmf_context *context, const rt_uint8_t *data, rt_size_t length)
{
    rt_size_t index;
    rt_size_t total = 0;
    rt_size_t count;

    context->rx_glom_count = 0;
    if (!length || (length & 1U))
    {
        LOG_W("invalid RX glom descriptor length %u",
              (unsigned int)length);
        return -RT_EIO;
    }
    count = length / sizeof(rt_uint16_t);
    if (count > BRCMF_MAX_RX_GLOM_FRAMES)
    {
        LOG_W("RX glom descriptor has %u frames",
              (unsigned int)count);
        return -RT_EFULL;
    }
    for (index = 0; index < count; index++)
    {
        rt_uint16_t subframe_length = brcmf_get_le16(
            data + index * sizeof(rt_uint16_t));

        if (subframe_length < SDPCM_HEADER_SIZE ||
            (!index && subframe_length < 2U * SDPCM_HEADER_SIZE) ||
            (subframe_length & 3U) ||
            total > BRCMF_RX_BUFFER_SIZE - subframe_length)
        {
            LOG_W("invalid RX glom subframe %u length %u",
                  (unsigned int)index, subframe_length);
            return -RT_EIO;
        }
        context->rx_glom_length[index] = subframe_length;
        total += subframe_length;
    }
    if (RT_ALIGN(total, context->f2_block_size) > BRCMF_RX_BUFFER_SIZE)
    {
        LOG_W("RX glom superframe too large: %u",
              (unsigned int)total);
        return -RT_EFULL;
    }
    context->rx_glom_count = count;
    context->rx_next_length = 0;
    return RT_EOK;
}

static rt_err_t brcmf_sdio_deliver_frame(
    struct brcmf_context *context, rt_uint8_t *frame, rt_size_t available,
    rt_bool_t update_flow)
{
    rt_uint16_t frame_length;
    rt_uint8_t channel;
    rt_uint8_t data_offset;
    rt_size_t payload_length;
    struct brcmf_bus_record *record;
    rt_err_t result = brcmf_sdio_parse_header(
        frame, available, &frame_length, &channel, &data_offset);

    if (result != RT_EOK)
    {
        return result;
    }
    if (frame_length > available)
    {
        return -RT_EIO;
    }
    if (update_flow)
    {
        rt_size_t next_length =
            (rt_size_t)frame[6] * SDPCM_NEXT_LENGTH_UNIT;

        brcmf_sdio_update_flow_control(context, frame);
        context->rx_next_length = next_length <= BRCMF_RX_BUFFER_SIZE ?
                                  next_length : 0;
    }
    payload_length = frame_length - data_offset;
    if (channel == SDPCM_GLOM_CHANNEL)
    {
        if (!(frame[5] & SDPCM_GLOM_DESCRIPTOR))
        {
            return -RT_EIO;
        }
        return brcmf_sdio_save_glom_descriptor(
            context, frame + data_offset, payload_length);
    }
    if (channel > BRCMF_BUS_CHANNEL_DATA)
    {
        return -RT_EIO;
    }
    if (!payload_length)
    {
        return RT_EOK;
    }
    record = (struct brcmf_bus_record *)(frame + data_offset -
                                         sizeof(*record));
    record->channel = channel;
    record->interface_index = 0;
    record->length = payload_length;
    return rt_wlan_offload_bus_rx(&context->bus, record,
                                  sizeof(*record) + payload_length);
}

static rt_err_t brcmf_sdio_receive_glom(
    struct brcmf_context *context, rt_uint32_t *frames_consumed,
    rt_bool_t *request_retry)
{
    rt_size_t index;
    rt_size_t total = 0;
    rt_size_t transfer_length;
    rt_size_t segment_offset = 0;
    rt_uint16_t superframe_length = 0;
    rt_uint8_t superframe_channel = 0;
    rt_uint8_t superframe_offset = 0;
    rt_uint8_t count = context->rx_glom_count;
    rt_err_t result;

    *request_retry = RT_FALSE;
    for (index = 0; index < count; index++)
    {
        total += context->rx_glom_length[index];
    }
    transfer_length = RT_ALIGN(total, context->f2_block_size);
    context->rx_glom_count = 0;
    if (!count || transfer_length > BRCMF_RX_BUFFER_SIZE)
    {
        return -RT_EIO;
    }
    rt_mutex_take(&context->io_mutex, RT_WAITING_FOREVER);
    mmcsd_host_lock(context->card->host);
    result = brcmf_sdio_result(sdio_io_read_multi_fifo_b(
        context->function2, 0, context->rx_buffer, transfer_length));
    mmcsd_host_unlock(context->card->host);
    rt_mutex_release(&context->io_mutex);
    if (result != RT_EOK)
    {
        *request_retry = RT_TRUE;
        return result;
    }
    result = brcmf_sdio_parse_header(
        context->rx_buffer, transfer_length, &superframe_length,
        &superframe_channel, &superframe_offset);
    if (result != RT_EOK || superframe_channel != SDPCM_GLOM_CHANNEL ||
        (context->rx_buffer[5] & SDPCM_GLOM_DESCRIPTOR) ||
        RT_ALIGN(superframe_length, context->f2_block_size) !=
            transfer_length ||
        superframe_offset >= context->rx_glom_length[0])
    {
        LOG_W("invalid RX glom superframe len=%u read=%u channel=%u",
              superframe_length, (unsigned int)transfer_length,
              superframe_channel);
        return -RT_EIO;
    }
    brcmf_sdio_update_flow_control(context, context->rx_buffer);
    context->rx_next_length =
        (rt_size_t)context->rx_buffer[6] * SDPCM_NEXT_LENGTH_UNIT;
    for (index = 0; index < count; index++)
    {
        rt_uint8_t *subframe;
        rt_size_t available;

        if (!index)
        {
            subframe = context->rx_buffer + superframe_offset;
            available = context->rx_glom_length[0] - superframe_offset;
        }
        else
        {
            subframe = context->rx_buffer + segment_offset;
            available = context->rx_glom_length[index];
        }
        result = brcmf_sdio_deliver_frame(
            context, subframe, available, RT_FALSE);
        if (result != RT_EOK)
        {
            LOG_W("invalid RX glom subframe %u: %d",
                  (unsigned int)index, result);
            return result;
        }
        segment_offset += context->rx_glom_length[index];
    }
    *frames_consumed = count;
    return RT_EOK;
}

static rt_err_t brcmf_sdio_receive_one(
    struct brcmf_context *context, rt_uint32_t *frames_consumed,
    rt_bool_t *request_retry)
{
    rt_uint16_t frame_length;
    rt_uint16_t predicted_length = context->rx_next_length;
    rt_uint8_t channel;
    rt_uint8_t data_offset;
    rt_size_t transfer_length;
    rt_size_t available;
    rt_err_t result;

    *frames_consumed = 1U;
    *request_retry = RT_FALSE;
    if (context->rx_glom_count)
    {
        return brcmf_sdio_receive_glom(
            context, frames_consumed, request_retry);
    }
    context->rx_next_length = 0;
    rt_mutex_take(&context->io_mutex, RT_WAITING_FOREVER);
    mmcsd_host_lock(context->card->host);
    if (predicted_length)
    {
        transfer_length = brcmf_sdio_transfer_length(
            context, predicted_length);
        if (transfer_length > BRCMF_RX_BUFFER_SIZE)
        {
            result = -RT_EFULL;
            goto unlock;
        }
        result = brcmf_sdio_result(sdio_io_read_multi_fifo_b(
            context->function2, 0, context->rx_buffer, transfer_length));
        available = transfer_length;
    }
    else
    {
        result = brcmf_sdio_result(sdio_io_read_multi_fifo_b(
            context->function2, 0, context->rx_buffer, SDPCM_FIRST_READ));
        available = SDPCM_FIRST_READ;
    }
    if (result != RT_EOK)
    {
        *request_retry = RT_TRUE;
        goto unlock;
    }
    result = brcmf_sdio_parse_header(
        context->rx_buffer, available, &frame_length, &channel, &data_offset);
    if (result != RT_EOK)
    {
        goto unlock;
    }
    if (predicted_length)
    {
        if (RT_ALIGN(frame_length, SDPCM_NEXT_LENGTH_UNIT) != predicted_length)
        {
            LOG_W("SDPCM read-ahead mismatch: expected %u, frame %u",
                  predicted_length, frame_length);
            result = -RT_EIO;
            *request_retry = RT_TRUE;
        }
        goto unlock;
    }
    if (frame_length > available)
    {
        rt_size_t remaining = frame_length - available;
        rt_size_t body_length = brcmf_sdio_transfer_length(
            context, remaining);

        if (body_length > BRCMF_RX_BUFFER_SIZE - available)
        {
            result = -RT_EFULL;
            goto unlock;
        }
        result = brcmf_sdio_result(sdio_io_read_multi_fifo_b(
            context->function2, 0, context->rx_buffer + available,
            body_length));
        if (result != RT_EOK && channel != BRCMF_BUS_CHANNEL_DATA)
        {
            *request_retry = RT_TRUE;
        }
        available += body_length;
    }

unlock:
    mmcsd_host_unlock(context->card->host);
    rt_mutex_release(&context->io_mutex);
    if (result != RT_EOK)
    {
        return result;
    }
    result = brcmf_sdio_deliver_frame(
        context, context->rx_buffer, available, RT_TRUE);
    if (result != RT_EOK && channel != BRCMF_BUS_CHANNEL_DATA)
    {
        *request_retry = RT_TRUE;
    }
    return result;
}

static rt_err_t brcmf_sdio_recover_tx_locked(struct brcmf_context *context)
{
    rt_uint8_t abort_function = context->function2->num;
    rt_uint8_t high = 0;
    rt_uint8_t low = 0;
    rt_uint16_t remaining = 0;
    rt_uint32_t retry;
    rt_int32_t io_result = RT_EOK;
    rt_err_t result;

    mmcsd_host_lock(context->card->host);
    result = brcmf_sdio_result(sdio_io_rw_direct(
        context->card, 1, 0, SDIO_REG_CCCR_IO_ABORT,
        &abort_function, 0));
    if (result == RT_EOK)
    {
        result = brcmf_sdio_result(sdio_io_writeb(
            context->function1, SBSDIO_FRAMECTRL,
            SBSDIO_FRAMECTRL_WF_TERM));
    }
    if (result == RT_EOK)
    {
        for (retry = 0; retry < 3U; retry++)
        {
            high = sdio_io_readb(context->function1,
                                 SBSDIO_WFRAMEBCHI, &io_result);
            if (io_result != RT_EOK)
            {
                result = brcmf_sdio_result(io_result);
                break;
            }
            low = sdio_io_readb(context->function1,
                                SBSDIO_WFRAMEBCLO, &io_result);
            if (io_result != RT_EOK)
            {
                result = brcmf_sdio_result(io_result);
                break;
            }
            remaining = ((rt_uint16_t)high << 8) | low;
            if (!remaining)
            {
                break;
            }
        }
        if (remaining)
        {
            result = -RT_ETIMEOUT;
        }
    }
    mmcsd_host_unlock(context->card->host);
    context->tx_recovery_count++;
    if (result != RT_EOK)
    {
        LOG_W("TX F2 recovery failed: %d (remaining=%u)", result,
              remaining);
    }
    return result;
}

static rt_err_t brcmf_sdio_send_buffer_locked(
    struct brcmf_context *context, rt_uint8_t *buffer,
    rt_size_t length, rt_uint32_t retries)
{
    rt_uint32_t attempt;
    rt_err_t result = -RT_EIO;

    for (attempt = 0; attempt <= retries; attempt++)
    {
        mmcsd_host_lock(context->card->host);
        result = brcmf_sdio_result(sdio_io_write_multi_fifo_b(
            context->function2, 0, buffer, length));
        mmcsd_host_unlock(context->card->host);
        if (result == RT_EOK)
        {
            return RT_EOK;
        }
        if (brcmf_sdio_recover_tx_locked(context) != RT_EOK ||
            attempt == retries)
        {
            break;
        }
        context->tx_retry_count++;
        LOG_W("retrying F2 control TX after SDIO error (%u/%u)",
              (unsigned int)(attempt + 1U),
              (unsigned int)retries);
    }
    return result;
}

static void brcmf_sdio_recover_rx(struct brcmf_context *context,
                                    rt_bool_t request_retry)
{
    rt_uint8_t abort_function = context->function2->num;
    rt_uint8_t high = 0;
    rt_uint8_t low = 0;
    rt_uint16_t remaining = 0;
    rt_uint32_t retry;
    rt_int32_t io_result = RT_EOK;
    rt_err_t result;

    result = rt_mutex_take(&context->io_mutex, RT_WAITING_FOREVER);
    if (result != RT_EOK)
    {
        return;
    }
    mmcsd_host_lock(context->card->host);
    result = brcmf_sdio_result(sdio_io_rw_direct(
        context->card, 1, 0, SDIO_REG_CCCR_IO_ABORT,
        &abort_function, 0));
    if (result == RT_EOK)
    {
        result = brcmf_sdio_result(sdio_io_writeb(
            context->function1, SBSDIO_FRAMECTRL,
            SBSDIO_FRAMECTRL_RF_TERM));
    }
    if (result == RT_EOK)
    {
        for (retry = 0; retry < 1000U; retry++)
        {
            high = sdio_io_readb(context->function1,
                                 SBSDIO_RFRAMEBCHI, &io_result);
            if (io_result != RT_EOK)
            {
                result = brcmf_sdio_result(io_result);
                break;
            }
            low = sdio_io_readb(context->function1,
                                SBSDIO_RFRAMEBCLO, &io_result);
            if (io_result != RT_EOK)
            {
                result = brcmf_sdio_result(io_result);
                break;
            }
            remaining = ((rt_uint16_t)high << 8) | low;
            if (!remaining)
            {
                break;
            }
        }
        if (remaining)
        {
            result = -RT_ETIMEOUT;
        }
    }
    mmcsd_host_unlock(context->card->host);
    rt_mutex_release(&context->io_mutex);

    context->rx_next_length = 0;
    context->rx_glom_count = 0;
    context->rx_pending = RT_FALSE;
    context->rx_skip = RT_FALSE;
    context->rx_recovery_count++;
    if (result == RT_EOK && request_retry)
    {
        struct brcmf_core *sdio_core = brcmf_chip_get_core(
            &context->chip, BRCMF_CORE_SDIO_DEV);

        result = sdio_core ? brcmf_sdio_write32(
            context, sdio_core->base + SDIO_TO_SB_MAILBOX_OFFSET,
            SDIO_SB_MAILBOX_NAK) : -RT_ENOSYS;
        if (result == RT_EOK)
        {
            context->rx_skip = RT_TRUE;
            context->rx_retry_count++;
        }
    }
    if (result != RT_EOK)
    {
        LOG_W("RX F2 recovery failed: %d (remaining=%u)", result,
              remaining);
    }
}

static rt_err_t brcmf_sdio_process_hostmail(
    struct brcmf_context *context, struct brcmf_core *sdio_core,
    rt_bool_t *frame_indicated)
{
    rt_uint32_t data;
    rt_uint32_t version;
    rt_uint32_t known;
    rt_err_t result;

    data = brcmf_sdio_read32(
        context, sdio_core->base + SDIO_TO_HOST_MAILBOX_DATA_OFFSET,
        &result);
    if (result != RT_EOK)
    {
        return result;
    }
    result = brcmf_sdio_write32(
        context, sdio_core->base + SDIO_TO_SB_MAILBOX_OFFSET,
        SDIO_SB_MAILBOX_INT_ACK);
    if (result != RT_EOK)
    {
        return result;
    }
    context->hostmail_count++;

    if (data & SDIO_HOST_DATA_NAK_HANDLED)
    {
        context->rx_skip = RT_FALSE;
        *frame_indicated = RT_TRUE;
    }
    if (data & (SDIO_HOST_DATA_DEVICE_READY |
                SDIO_HOST_DATA_FIRMWARE_READY))
    {
        version = (data & SDIO_HOST_DATA_VERSION_MASK) >>
                  SDIO_HOST_DATA_VERSION_SHIFT;
        if (version != SDPCM_PROTOCOL_VERSION)
        {
            LOG_W("firmware SDPCM version %u, expected %u",
                  (unsigned int)version,
                  (unsigned int)SDPCM_PROTOCOL_VERSION);
        }
    }
    if (data & SDIO_HOST_DATA_FLOW_CONTROL)
    {
        brcmf_sdio_set_priority_flow_control(
            context,
            (rt_uint8_t)((data & SDIO_HOST_DATA_FLOW_MASK) >>
                         SDIO_HOST_DATA_FLOW_SHIFT));
    }
    if (data & SDIO_HOST_DATA_FIRMWARE_HALT)
    {
        LOG_E("firmware halted (mailbox 0x%08x)", data);
        rt_wlan_offload_bus_notify(
            &context->bus, RT_WLAN_OFFLOAD_BUS_EVENT_ERROR, -RT_EIO);
    }
    known = SDIO_HOST_DATA_NAK_HANDLED |
            SDIO_HOST_DATA_DEVICE_READY |
            SDIO_HOST_DATA_FLOW_CONTROL |
            SDIO_HOST_DATA_FIRMWARE_READY |
            SDIO_HOST_DATA_FIRMWARE_HALT |
            SDIO_HOST_DATA_VERSION_MASK |
            SDIO_HOST_DATA_FLOW_MASK;
    if (data & ~known)
    {
        LOG_W("unknown host mailbox data 0x%08x", data);
    }
    return RT_EOK;
}

static rt_err_t brcmf_sdio_process_interrupt(
    struct brcmf_context *context, rt_bool_t *frame_indicated)
{
    struct brcmf_core *sdio_core = brcmf_chip_get_core(
        &context->chip, BRCMF_CORE_SDIO_DEV);
    rt_uint32_t interrupt_status;
    rt_uint32_t pending;
    rt_uint32_t new_pending;
    rt_err_t result;

    *frame_indicated = RT_FALSE;
    if (!sdio_core)
    {
        return -RT_ENOSYS;
    }
    interrupt_status = brcmf_sdio_read32(
        context, sdio_core->base + SDIO_INT_STATUS_OFFSET, &result);
    if (result != RT_EOK)
    {
        return result;
    }
    pending = interrupt_status & SDIO_HOST_MAILBOX_MASK;
    brcmf_sdio_set_global_flow_control(
        context, (pending & SDIO_HOST_FC_STATE) != 0U);
    if (pending)
    {
        result = brcmf_sdio_write32(
            context, sdio_core->base + SDIO_INT_STATUS_OFFSET, pending);
        if (result != RT_EOK)
        {
            return result;
        }
    }

    if (pending & SDIO_HOST_FC_CHANGE)
    {
        interrupt_status = brcmf_sdio_read32(
            context, sdio_core->base + SDIO_INT_STATUS_OFFSET, &result);
        if (result != RT_EOK)
        {
            return result;
        }
        new_pending = interrupt_status & SDIO_HOST_MAILBOX_MASK;
        brcmf_sdio_set_global_flow_control(
            context, (new_pending &
                      (SDIO_HOST_FC_STATE | SDIO_HOST_FC_CHANGE)) != 0U);
        if (new_pending)
        {
            result = brcmf_sdio_write32(
                context, sdio_core->base + SDIO_INT_STATUS_OFFSET,
                new_pending);
            if (result != RT_EOK)
            {
                return result;
            }
            pending |= new_pending;
        }
    }

    *frame_indicated = (pending & SDIO_HOST_FRAME_INDICATION) != 0U;
    if (pending & SDIO_HOST_MAILBOX_INDICATION)
    {
        result = brcmf_sdio_process_hostmail(
            context, sdio_core, frame_indicated);
        if (result != RT_EOK)
        {
            return result;
        }
    }
    return RT_EOK;
}

static void brcmf_sdio_rearm_deferred_irq(struct brcmf_context *context)
{
    struct rt_mmcsd_host *host;

    if (!context->irq_deferred || !context->card)
    {
        return;
    }
    host = context->card->host;
    if (!host || !host->ops || !host->ops->enable_sdio_irq)
    {
        return;
    }

    /* Keep the level-triggered card IRQ masked until F2 has been drained. */
    context->irq_deferred = RT_FALSE;
    host->flags |= MMCSD_SUP_SDIO_IRQ;
    host->ops->enable_sdio_irq(host, 1);
}

static void brcmf_sdio_worker(void *parameter)
{
    struct brcmf_context *context = parameter;

    while (context->worker_running)
    {
        rt_uint32_t count;
        rt_bool_t frame_indicated;
        rt_bool_t interrupt_frame;
        rt_err_t result;

        rt_err_t wait_result = rt_sem_take(
            context->rx_sem, context->firmware_running ?
            rt_tick_from_millisecond(BRCMFMAC_SDIO_WATCHDOG_MS) :
            RT_WAITING_FOREVER);

        if (wait_result != RT_EOK)
        {
            context->rx_watchdog_count++;
        }
        if (!context->worker_running)
        {
            break;
        }
        if (!context->firmware_running)
        {
            brcmf_sdio_rearm_deferred_irq(context);
            continue;
        }
        brcmf_wifi_watchdog(context);
        frame_indicated = context->rx_pending;
        context->rx_pending = RT_FALSE;
        /* Linux keeps SDIO interrupt polling disabled by default. A semaphore
         * wake without rx_pending is an explicit TX-credit status scan; an
         * idle watchdog tick only services software timeouts. */
        if (wait_result != RT_EOK && !frame_indicated)
        {
            brcmf_sdio_rearm_deferred_irq(context);
            continue;
        }
        result = brcmf_sdio_process_interrupt(
            context, &interrupt_frame);
        if (result != RT_EOK)
        {
            LOG_W("interrupt processing failed: %d", result);
            if (frame_indicated)
            {
                context->rx_pending = RT_TRUE;
                rt_sem_release(context->rx_sem);
            }
            brcmf_sdio_rearm_deferred_irq(context);
            continue;
        }
        frame_indicated = frame_indicated || interrupt_frame;
        if (context->rx_skip || !frame_indicated)
        {
            brcmf_sdio_rearm_deferred_irq(context);
            continue;
        }
        for (count = 0; count < SDPCM_MAX_RX_FRAMES;)
        {
            rt_uint32_t frames_consumed;
            rt_bool_t request_retry;

            result = brcmf_sdio_receive_one(
                context, &frames_consumed, &request_retry);
            if (result == -RT_EEMPTY)
            {
                if (!count && frame_indicated)
                {
                    context->rx_empty_poll_count++;
                }
                break;
            }
            if (result != RT_EOK)
            {
                LOG_W("RX failed: %d", result);
                brcmf_sdio_recover_rx(context, request_retry);
                break;
            }
            count += frames_consumed;
        }
        if (count >= SDPCM_MAX_RX_FRAMES)
        {
            context->rx_pending = RT_TRUE;
            rt_sem_release(context->rx_sem);
        }
        brcmf_sdio_rearm_deferred_irq(context);
    }
    rt_completion_done(&context->worker_stopped);
}

static void brcmf_sdio_irq(struct rt_sdio_function *function)
{
    struct brcmf_context *context = sdio_get_drvdata(function);

    if (!context)
    {
        return;
    }
    if (function->num == 1U)
    {
        context->irq_f1_count++;
    }
    else if (function->num == 2U)
    {
        context->irq_f2_count++;
    }
    else
    {
        return;
    }

    /* RT-Thread otherwise unmasks the level IRQ before the deferred worker
     * can clear the F2 source. The legacy CYW43 transport uses the same
     * deferred-mask handshake. */
    context->card->host->flags &= ~MMCSD_SUP_SDIO_IRQ;
    context->irq_deferred = RT_TRUE;
    if (context->rx_sem)
    {
        rt_sem_release(context->rx_sem);
    }
}

static rt_uint8_t brcmf_sdio_tx_credits(struct brcmf_context *context,
                                         rt_bool_t reserve_control)
{
    rt_uint8_t credits = context->tx_max - context->tx_sequence;

    if (reserve_control &&
        (context->tx_flow_control_state || context->tx_flow_control))
    {
        return 0;
    }
    if (!credits || (credits & 0x80U))
    {
        return 0;
    }

    /* SDPCM reserves one firmware credit for the control channel. This is
     * DATAOK()/DATABUFCNT() in the vendor driver and applies even when no
     * control request is currently waiting. */
    if (reserve_control)
    {
        if (credits <= 1U)
        {
            return 0;
        }
        credits--;
    }
    return credits;
}

static rt_err_t brcmf_sdio_wait_tx_credit(struct brcmf_context *context,
                                           rt_bool_t control,
                                           rt_uint8_t *credits)
{
    rt_tick_t poll_interval = rt_tick_from_millisecond(
        BRCMFMAC_TX_CREDIT_POLL_MS);
    rt_tick_t timeout = rt_tick_from_millisecond(
        control ? BRCMFMAC_CONTROL_TIMEOUT_MS :
                  BRCMFMAC_TX_CREDIT_TIMEOUT_MS);
    rt_tick_t scan_interval = rt_tick_from_millisecond(
        BRCMFMAC_SDIO_WATCHDOG_MS);
    rt_tick_t started = rt_tick_get();
    rt_tick_t last_scan = 0;
    rt_bool_t scan_requested = RT_FALSE;

    while (context->firmware_running && context->tx_worker_running)
    {
        rt_tick_t now = rt_tick_get();
        rt_bool_t request_scan = !scan_requested ||
            now - last_scan >= scan_interval;

        *credits = brcmf_sdio_tx_credits(context, !control);
        if (*credits)
        {
            return RT_EOK;
        }
        if (rt_tick_get() - started >= timeout)
        {
            context->tx_credit_stall_count++;
            if (control)
            {
                LOG_E("control TX credit timeout: seq=%u max=%u waits=%u",
                      context->tx_sequence, context->tx_max,
                      (unsigned int)context->tx_credit_wait_count);
                return -RT_ETIMEOUT;
            }

            /* Linux leaves data queued while the firmware credit window is
             * closed. A full window is normal under load and is not evidence
             * that firmware state was lost. Keep polling; actual SDIO and
             * command failures still take the transport recovery path. */
            LOG_W("data TX credit stall: seq=%u max=%u waits=%u",
                  context->tx_sequence, context->tx_max,
                  (unsigned int)context->tx_credit_wait_count);
            started = rt_tick_get();
            request_scan = RT_TRUE;
        }

        /* Firmware publishes credit changes through received SDPCM headers.
         * Keep requesting status scans while TX is blocked so a delayed or
         * lost card interrupt cannot strand the firmware credit update. */
        context->tx_credit_wait_count++;
        if (request_scan)
        {
            scan_requested = RT_TRUE;
            last_scan = now;
            rt_sem_release(context->rx_sem);
        }

        /* Discard notifications left by earlier window and flow-control
         * updates. Recheck credits after the drain so a concurrent update is
         * not lost. */
        while (rt_sem_take(context->tx_sem, 0) == RT_EOK)
        {
        }
        if (brcmf_sdio_tx_credits(context, !control))
        {
            continue;
        }
        rt_sem_take(context->tx_sem, poll_interval);
    }
    return -RT_EIO;
}

static void brcmf_sdio_finish_control_tx(struct brcmf_context *context)
{
    context->control_waiting = RT_FALSE;
    if (brcmf_sdio_tx_credits(context, RT_FALSE))
    {
        rt_sem_release(context->tx_sem);
    }
}

static rt_size_t brcmf_sdio_build_tx_frame(
    rt_uint8_t *destination, rt_size_t capacity,
    const struct brcmf_bus_record *record, rt_uint8_t sequence,
    rt_bool_t extended, rt_bool_t last, rt_uint16_t tail_padding)
{
    rt_size_t software_offset = SDPCM_HW_HEADER_SIZE +
        (extended ? SDPCM_HW_EXTENSION_SIZE : 0U);
    rt_size_t header_length = software_offset + 8U +
        (record->channel == BRCMF_BUS_CHANNEL_DATA ? SDPCM_DATA_PAD : 0U);
    rt_size_t frame_length = header_length + record->length;
    rt_size_t wire_length = frame_length + tail_padding;
    rt_size_t packet_length = frame_length;

    if (wire_length > capacity || packet_length > 0xffffU)
    {
        return 0;
    }
    /* Linux records tail padding only in the hardware extension. The
     * per-subframe length excludes it; only the first aggregate hardware
     * header is replaced with the complete transfer length. */
    brcmf_put_le16(destination, (rt_uint16_t)packet_length);
    brcmf_put_le16(destination + 2U, (rt_uint16_t)~packet_length);
    if (extended)
    {
        brcmf_put_le16(destination + SDPCM_HW_HEADER_SIZE,
                       (rt_uint16_t)(packet_length - SDPCM_HW_HEADER_SIZE));
        destination[SDPCM_HW_HEADER_SIZE + 3U] = last ? 1U : 0U;
        brcmf_put_le16(destination + SDPCM_HW_HEADER_SIZE + 6U,
                       tail_padding);
    }
    destination[software_offset] = sequence;
    destination[software_offset + 1U] = record->channel;
    destination[software_offset + 3U] = header_length;
    rt_memcpy(destination + header_length, record->payload, record->length);
    return wire_length;
}

static rt_err_t brcmf_sdio_write_tx_records(
    struct brcmf_context *context, struct brcmf_tx_record **records,
    rt_uint8_t count)
{
    rt_size_t wire_length[BRCMFMAC_TX_GLOM_FRAMES];
    rt_uint16_t tail_padding[BRCMFMAC_TX_GLOM_FRAMES];
    rt_bool_t extended = context->tx_glom;
    rt_bool_t aggregate = extended && count > 1U;
    rt_size_t total = 0;
    rt_size_t transfer_length;
    rt_uint8_t credits;
    rt_uint8_t index;
    rt_err_t result;

    if (!count || count > BRCMFMAC_TX_GLOM_FRAMES)
    {
        return -RT_EINVAL;
    }
    for (index = 0; index < count; index++)
    {
        const struct brcmf_bus_record *record =
            (const struct brcmf_bus_record *)records[index]->data;
        rt_size_t header_length = SDPCM_HEADER_SIZE +
            (extended ? SDPCM_HW_EXTENSION_SIZE : 0U) +
            SDPCM_DATA_PAD;
        rt_size_t frame_length = header_length + record->length;

        if (records[index]->length < sizeof(*record) ||
            record->length != records[index]->length - sizeof(*record) ||
            record->channel != BRCMF_BUS_CHANNEL_DATA)
        {
            return -RT_EINVAL;
        }
        tail_padding[index] = extended ?
            (rt_uint16_t)(RT_ALIGN(frame_length, SDPCM_TX_ALIGNMENT) -
                          frame_length) : 0U;
        wire_length[index] = frame_length + tail_padding[index];
        if (total > BRCMF_RX_BUFFER_SIZE - wire_length[index])
        {
            return -RT_EFULL;
        }
        total += wire_length[index];
    }
    if (extended)
    {
        rt_size_t aligned = total;

        /* bcmdhd aligns every TX-glom subframe to DHD_SDALIGN and sends a
         * multi-frame chain, or any frame larger than one F2 block, as one
         * block-aligned CMD53. The extension tail length must include both
         * the subframe and final block padding. Splitting a single SDPCM
         * frame across block- and byte-mode CMD53 writes can make firmware
         * consume stale FIFO data as another packet. */
        if (count > 1U || total > context->f2_block_size)
        {
            aligned = RT_ALIGN(total, context->f2_block_size);
        }

        if (aligned > BRCMF_RX_BUFFER_SIZE || aligned > 0xffffU)
        {
            return -RT_EFULL;
        }
        tail_padding[count - 1U] += aligned - total;
        wire_length[count - 1U] += aligned - total;
        total = aligned;
        transfer_length = total;
    }
    else
    {
        transfer_length = brcmf_sdio_transfer_length(context, total);
    }
    if (transfer_length > BRCMF_RX_BUFFER_SIZE)
    {
        return -RT_EFULL;
    }

    for (;;)
    {
        result = brcmf_sdio_wait_tx_credit(context, RT_FALSE, &credits);
        if (result != RT_EOK)
        {
            return result;
        }
        if (credits < count)
        {
            rt_thread_mdelay(1);
            continue;
        }
        result = rt_mutex_take(&context->io_mutex, RT_WAITING_FOREVER);
        if (result != RT_EOK)
        {
            return result;
        }
        credits = brcmf_sdio_tx_credits(context, RT_TRUE);
        if (context->firmware_running && context->tx_glom == extended &&
            credits >= count)
        {
            break;
        }
        rt_mutex_release(&context->io_mutex);
        if (!context->firmware_running || context->tx_glom != extended)
        {
            return -RT_EIO;
        }
    }

    rt_memset(context->tx_buffer, 0, transfer_length);
    total = 0;
    for (index = 0; index < count; index++)
    {
        const struct brcmf_bus_record *record =
            (const struct brcmf_bus_record *)records[index]->data;
        rt_uint8_t *frame = context->tx_buffer + total;
        rt_size_t prepared = brcmf_sdio_build_tx_frame(
            frame, transfer_length - total, record,
            context->tx_sequence + index, extended,
            index == count - 1U, tail_padding[index]);

        if (!prepared || prepared != wire_length[index] ||
            brcmf_get_le16(frame) != prepared - tail_padding[index] ||
            (rt_uint16_t)(brcmf_get_le16(frame) ^
                          brcmf_get_le16(frame + 2U)) != 0xffffU ||
            (extended &&
             (brcmf_get_le16(frame + SDPCM_HW_HEADER_SIZE) !=
                  prepared - tail_padding[index] -
                  SDPCM_HW_HEADER_SIZE ||
              brcmf_get_le16(frame + SDPCM_HW_HEADER_SIZE + 6U) !=
                  tail_padding[index])))
        {
            rt_mutex_release(&context->io_mutex);
            return -RT_EIO;
        }
        total += prepared;
    }
    if (extended)
    {
        brcmf_put_le16(context->tx_buffer, (rt_uint16_t)total);
        brcmf_put_le16(context->tx_buffer + 2U, (rt_uint16_t)~total);
    }
    result = brcmf_sdio_send_buffer_locked(
        context, context->tx_buffer, transfer_length, 0U);
    if (result == RT_EOK)
    {
        context->tx_sequence += count;
        context->tx_transfer_count++;
        context->tx_frame_count += count;
        if (aggregate)
        {
            context->tx_aggregate_count++;
        }
        if (count > context->tx_max_aggregate)
        {
            context->tx_max_aggregate = count;
        }
    }
    else
    {
        context->tx_drop_count += count;
        context->tx_error_count++;
        LOG_E("F2 data TX failed for %u frame(s); firmware acceptance "
              "is ambiguous, restarting transport", count);
    }
    rt_mutex_release(&context->io_mutex);
    if (result != RT_EOK)
    {
        rt_wlan_offload_bus_notify(
            &context->bus, RT_WLAN_OFFLOAD_BUS_EVENT_ERROR, result);
    }
    return result;
}

static rt_err_t brcmf_sdio_transmit_direct(
    struct brcmf_context *context, const struct brcmf_bus_record *record,
    rt_size_t length)
{
    rt_size_t header_length;
    rt_size_t frame_length;
    rt_size_t transfer_length;
    rt_uint16_t tail_padding;
    rt_uint8_t credits;
    rt_err_t result;

    (void)length;
    context->control_waiting = RT_TRUE;
    result = brcmf_sdio_wait_tx_credit(context, RT_TRUE, &credits);
    if (result != RT_EOK)
    {
        brcmf_sdio_finish_control_tx(context);
        return result;
    }
    for (;;)
    {
        result = rt_mutex_take(&context->io_mutex, RT_WAITING_FOREVER);
        if (result != RT_EOK)
        {
            brcmf_sdio_finish_control_tx(context);
            return result;
        }
        if (brcmf_sdio_tx_credits(context, RT_FALSE))
        {
            if (!context->firmware_running)
            {
                rt_mutex_release(&context->io_mutex);
                brcmf_sdio_finish_control_tx(context);
                return -RT_EIO;
            }
            break;
        }
        rt_mutex_release(&context->io_mutex);
        result = brcmf_sdio_wait_tx_credit(context, RT_TRUE, &credits);
        if (result != RT_EOK)
        {
            brcmf_sdio_finish_control_tx(context);
            return result;
        }
    }
    header_length = SDPCM_HEADER_SIZE +
                    (context->tx_glom ? SDPCM_HW_EXTENSION_SIZE : 0U) +
                    (record->channel == BRCMF_BUS_CHANNEL_DATA ?
                     SDPCM_DATA_PAD : 0U);
    frame_length = header_length + record->length;
    transfer_length = brcmf_sdio_transfer_length(context, frame_length);
    if (transfer_length > BRCMF_RX_BUFFER_SIZE)
    {
        rt_mutex_release(&context->io_mutex);
        brcmf_sdio_finish_control_tx(context);
        return -RT_EFULL;
    }
    tail_padding = context->tx_glom ? transfer_length - frame_length : 0U;
    rt_memset(context->tx_buffer, 0, transfer_length);
    if (!brcmf_sdio_build_tx_frame(
            context->tx_buffer, transfer_length, record,
            context->tx_sequence, context->tx_glom, RT_TRUE,
            tail_padding))
    {
        rt_mutex_release(&context->io_mutex);
        brcmf_sdio_finish_control_tx(context);
        return -RT_EIO;
    }
    if (context->tx_glom)
    {
        brcmf_put_le16(context->tx_buffer, (rt_uint16_t)transfer_length);
        brcmf_put_le16(context->tx_buffer + 2U,
                       (rt_uint16_t)~transfer_length);
    }
    result = brcmf_sdio_send_buffer_locked(
        context, context->tx_buffer, transfer_length, BRCMFMAC_TX_RETRIES);
    if (result == RT_EOK)
    {
        context->tx_sequence++;
        context->tx_transfer_count++;
        context->tx_frame_count++;
    }
    else
    {
        context->tx_error_count++;
        LOG_E("F2 control TX failed after recovery retries; "
              "restarting transport");
    }
    rt_mutex_release(&context->io_mutex);
    brcmf_sdio_finish_control_tx(context);
    if (result != RT_EOK)
    {
        rt_wlan_offload_bus_notify(
            &context->bus, RT_WLAN_OFFLOAD_BUS_EVENT_ERROR, result);
    }
    return result;
}

static void brcmf_sdio_flush_tx_queue(struct brcmf_context *context)
{
    struct brcmf_tx_record *record;

    if (!context->tx_queue)
    {
        return;
    }
    while (rt_mq_recv(context->tx_queue, &record, sizeof(record), 0) ==
           RT_EOK)
    {
        if (record)
        {
            rt_mp_free(record);
        }
    }
    rt_mq_control(context->tx_queue, RT_IPC_CMD_RESET, RT_NULL);
}

static void brcmf_sdio_tx_worker(void *parameter)
{
    struct brcmf_context *context = parameter;
    struct brcmf_tx_record *records[BRCMFMAC_TX_GLOM_FRAMES];

    while (context->tx_worker_running)
    {
        rt_uint8_t count = 0;
        rt_uint8_t credits = 0;
        rt_uint8_t limit;
        rt_err_t result;

        result = rt_mq_recv(context->tx_queue, &records[0],
                            sizeof(records[0]), RT_WAITING_FOREVER);
        if (!context->tx_worker_running)
        {
            if (result == RT_EOK && records[0])
            {
                rt_mp_free(records[0]);
            }
            break;
        }
        if (result != RT_EOK || !records[0])
        {
            continue;
        }
        count = 1U;
        if (!context->firmware_running)
        {
            rt_mp_free(records[0]);
            continue;
        }
        result = brcmf_sdio_wait_tx_credit(context, RT_FALSE, &credits);
        if (result != RT_EOK)
        {
            rt_mp_free(records[0]);
            continue;
        }
        limit = context->tx_glom ? credits : 1U;
        if (limit > BRCMFMAC_TX_GLOM_FRAMES)
        {
            limit = BRCMFMAC_TX_GLOM_FRAMES;
        }
        if (limit > 1U && rt_mq_recv(
                context->tx_queue, &records[count], sizeof(records[count]),
                rt_tick_from_millisecond(BRCMFMAC_TX_GLOM_WAIT_MS)) ==
                RT_EOK)
        {
            count++;
            while (count < limit && rt_mq_recv(
                       context->tx_queue, &records[count],
                       sizeof(records[count]), 0) == RT_EOK)
            {
                count++;
            }
        }
        result = brcmf_sdio_write_tx_records(context, records, count);
        for (rt_uint8_t index = 0; index < count; index++)
        {
            rt_mp_free(records[index]);
        }
        if (result != RT_EOK && context->firmware_running)
        {
            if (context->tx_error_count <= 4U ||
                !(context->tx_error_count & (context->tx_error_count - 1U)))
            {
                LOG_W("TX aggregate failed: %d (errors=%u)", result,
                      (unsigned int)context->tx_error_count);
            }
        }
    }
    rt_completion_done(&context->tx_worker_stopped);
}

static rt_err_t brcmf_sdio_quiesce_f2(struct brcmf_context *context)
{
    struct brcmf_core *sdio_core = brcmf_chip_get_core(
        &context->chip, BRCMF_CORE_SDIO_DEV);
    rt_err_t result = RT_EOK;
    rt_err_t current;

    if (sdio_core)
    {
        result = brcmf_sdio_write32(
            context, sdio_core->base + SDIO_INT_HOST_MASK_OFFSET, 0);
    }
    current = rt_mutex_take(&context->io_mutex, RT_WAITING_FOREVER);
    if (current == RT_EOK)
    {
        mmcsd_host_lock(context->card->host);
        current = brcmf_sdio_result(
            sdio_disable_func(context->function2));
        mmcsd_host_unlock(context->card->host);
        rt_mutex_release(&context->io_mutex);
    }
    if (result == RT_EOK && current != RT_EOK)
    {
        result = current;
    }
    if (sdio_core)
    {
        current = brcmf_sdio_write32(
            context, sdio_core->base + SDIO_INT_STATUS_OFFSET,
            SDIO_HOST_MAILBOX_MASK);
        if (result == RT_EOK && current != RT_EOK)
        {
            result = current;
        }
    }
    return result;
}

static rt_err_t brcmf_sdio_bus_start(struct rt_wlan_offload_bus *bus)
{
    struct brcmf_context *context = rt_wlan_offload_bus_get_driver_data(bus);
    struct brcmf_core *sdio_core;
    rt_err_t result;

    if (context->firmware_running)
    {
        return RT_EOK;
    }
    result = brcmf_chip_set_passive(context);
    if (result == RT_EOK)
    {
        result = brcmf_sdio_download(context);
    }
    if (result == RT_EOK)
    {
        result = brcmf_sdio_f1_writeb(context, SBSDIO_CHIPCLKCSR,
                                      SBSDIO_HT_AVAIL_REQ);
    }
    if (result == RT_EOK)
    {
        result = brcmf_sdio_wait_clock(context, SBSDIO_HT_AVAIL, 1000U);
    }
    if (result == RT_EOK)
    {
        result = brcmf_sdio_f1_writeb(
            context, SBSDIO_CHIPCLKCSR,
            context->chip.id == 43012U ? SBSDIO_HT_AVAIL_REQ :
                                         SBSDIO_FORCE_HT);
    }
    sdio_core = brcmf_chip_get_core(&context->chip, BRCMF_CORE_SDIO_DEV);
    if (result == RT_EOK && sdio_core)
    {
        result = brcmf_sdio_write32(
            context, sdio_core->base + SDIO_TO_SB_MAILBOX_DATA_OFFSET,
            SDPCM_PROTOCOL_VERSION << SDPCM_PROTOCOL_VERSION_SHIFT);
    }
    if (result == RT_EOK)
    {
        mmcsd_host_lock(context->card->host);
        result = brcmf_sdio_result(sdio_enable_func(context->function2));
        mmcsd_host_unlock(context->card->host);
    }
    if (result == RT_EOK && sdio_core)
    {
        result = brcmf_sdio_write32(
            context, sdio_core->base + SDIO_INT_HOST_MASK_OFFSET,
            SDIO_HOST_MAILBOX_MASK);
    }
    if (result == RT_EOK && sdio_core)
    {
        rt_uint8_t function_mask = 2U;

        result = brcmf_sdio_backplane_write(
            context, sdio_core->base + SDIO_FUNCTION_INT_MASK_OFFSET,
            &function_mask, sizeof(function_mask));
    }
    if (result == RT_EOK)
    {
        result = brcmf_sdio_configure_watermark(context);
    }
    if (result == RT_EOK)
    {
        brcmf_sdio_flush_tx_queue(context);
        context->tx_sequence = 0;
        context->tx_max = 4;
        context->rx_next_length = 0;
        context->rx_glom_count = 0;
        context->rx_pending = RT_FALSE;
        context->rx_skip = RT_FALSE;
        context->tx_glom = RT_FALSE;
        context->tx_flow_control = 0;
        context->tx_flow_control_state = RT_FALSE;
        context->control_waiting = RT_FALSE;
        while (rt_sem_take(context->tx_sem, 0) == RT_EOK)
        {
        }
        context->firmware_running = RT_TRUE;
        brcmf_sdio_rearm_deferred_irq(context);
        rt_sem_release(context->rx_sem);
        rt_wlan_offload_bus_notify(bus,
            RT_WLAN_OFFLOAD_BUS_EVENT_AVAILABLE, RT_EOK);
        LOG_I("firmware started on %s with NVRAM %s",
              context->mapping->model, context->mapping->nvram);
    }
    else
    {
        rt_err_t cleanup_result;

        context->firmware_running = RT_FALSE;
        context->card->host->flags &= ~MMCSD_SUP_SDIO_IRQ;
        context->irq_deferred = RT_TRUE;
        if (context->card->host->ops->enable_sdio_irq)
        {
            context->card->host->ops->enable_sdio_irq(
                context->card->host, 0);
        }
        cleanup_result = brcmf_sdio_quiesce_f2(context);
        if (cleanup_result != RT_EOK)
        {
            LOG_W("could not quiesce function 2 after start failure: %d",
                  cleanup_result);
        }
        cleanup_result = brcmf_chip_set_passive(context);
        if (cleanup_result != RT_EOK)
        {
            LOG_W("could not make chip passive after start failure: %d",
                  cleanup_result);
        }
    }
    return result;
}

static rt_err_t brcmf_sdio_bus_stop(struct rt_wlan_offload_bus *bus)
{
    struct brcmf_context *context = rt_wlan_offload_bus_get_driver_data(bus);
    rt_err_t passive_result;
    rt_err_t result;

    context->firmware_running = RT_FALSE;
    context->tx_glom = RT_FALSE;
    context->tx_flow_control = 0;
    context->tx_flow_control_state = RT_FALSE;
    context->control_waiting = RT_FALSE;
    context->rx_next_length = 0;
    context->rx_glom_count = 0;
    context->rx_pending = RT_FALSE;
    context->rx_skip = RT_FALSE;
    rt_sem_release(context->tx_sem);
    brcmf_sdio_flush_tx_queue(context);
    result = brcmf_sdio_quiesce_f2(context);
    if (result != RT_EOK)
    {
        LOG_W("could not fully quiesce function 2: %d", result);
    }
    brcmf_proto_stop(context);
    LOG_I("TX stopped: transfers=%u frames=%u aggregates=%u max=%u "
          "queue_high=%u drops=%u credit_waits=%u "
          "credit_stalls=%u errors=%u invalid_credits=%u "
          "irq=%u/%u hostmail=%u rx_watchdog=%u empty=%u",
          (unsigned int)context->tx_transfer_count,
          (unsigned int)context->tx_frame_count,
          (unsigned int)context->tx_aggregate_count,
          (unsigned int)context->tx_max_aggregate,
          (unsigned int)context->tx_queue_high_water,
          (unsigned int)context->tx_drop_count,
          (unsigned int)context->tx_credit_wait_count,
          (unsigned int)context->tx_credit_stall_count,
          (unsigned int)context->tx_error_count,
          (unsigned int)context->tx_invalid_credit_count,
          (unsigned int)context->irq_f1_count,
          (unsigned int)context->irq_f2_count,
          (unsigned int)context->hostmail_count,
          (unsigned int)context->rx_watchdog_count,
          (unsigned int)context->rx_empty_poll_count);
    passive_result = brcmf_chip_set_passive(context);
    if (result == RT_EOK)
    {
        result = passive_result;
    }
    return context->tearing_down ? RT_EOK : result;
}

static rt_err_t brcmf_sdio_log_tx_drop(struct brcmf_context *context,
                                        rt_err_t result)
{
    context->tx_drop_count++;
    if (context->tx_drop_count <= 4U ||
        !(context->tx_drop_count & (context->tx_drop_count - 1U)))
    {
        LOG_W("TX queue full: %d (drops=%u depth=%u high=%u seq=%u/%u "
              "transfers=%u frames=%u aggregates=%u max=%u credit_waits=%u "
              "credit_stalls=%u)",
              result,
              (unsigned int)context->tx_drop_count,
              context->tx_queue ? (unsigned int)context->tx_queue->entry : 0U,
              (unsigned int)context->tx_queue_high_water,
              context->tx_sequence, context->tx_max,
              (unsigned int)context->tx_transfer_count,
              (unsigned int)context->tx_frame_count,
              (unsigned int)context->tx_aggregate_count,
              (unsigned int)context->tx_max_aggregate,
              (unsigned int)context->tx_credit_wait_count,
              (unsigned int)context->tx_credit_stall_count);
    }
    return -RT_EFULL;
}

static rt_err_t brcmf_sdio_queue_data(
    struct brcmf_context *context,
    const struct rt_wlan_offload_bus_iovec *vectors, rt_size_t vector_count)
{
    struct brcmf_tx_record *queued;
    rt_size_t record_length = 0;
    rt_size_t offset = 0;
    rt_size_t index;
    rt_int32_t timeout;
    rt_tick_t deadline;
    rt_err_t result;

    for (index = 0; index < vector_count; index++)
    {
        if (record_length > BRCMF_TX_RECORD_CAPACITY - vectors[index].length)
        {
            return -RT_EINVAL;
        }
        record_length += vectors[index].length;
    }
    if (!context->firmware_running || !record_length ||
        !context->tx_pool || !context->tx_queue || !context->tx_worker_running)
    {
        return -RT_EINVAL;
    }

    timeout = rt_tick_from_millisecond(BRCMFMAC_TX_QUEUE_WAIT_MS);
    deadline = rt_tick_get() + timeout;
    queued = rt_mp_alloc(context->tx_pool, timeout);
    if (!queued)
    {
        return brcmf_sdio_log_tx_drop(context, -RT_EFULL);
    }

    queued->length = record_length;
    for (index = 0; index < vector_count; index++)
    {
        rt_memcpy(queued->data + offset, vectors[index].data,
                  vectors[index].length);
        offset += vectors[index].length;
    }

    timeout = (rt_int32_t)(deadline - rt_tick_get());
    if (timeout < 0)
    {
        timeout = 0;
    }
    result = rt_mq_send_wait(context->tx_queue, &queued, sizeof(queued),
                             timeout);
    if (result != RT_EOK)
    {
        rt_mp_free(queued);
        return brcmf_sdio_log_tx_drop(context, result);
    }
    if (context->tx_queue->entry > context->tx_queue_high_water)
    {
        context->tx_queue_high_water = context->tx_queue->entry;
    }
    return RT_EOK;
}

static rt_err_t brcmf_sdio_bus_transmit(struct rt_wlan_offload_bus *bus,
                                        const void *data, rt_size_t length)
{
    struct brcmf_context *context = rt_wlan_offload_bus_get_driver_data(bus);
    const struct brcmf_bus_record *record = data;
    struct rt_wlan_offload_bus_iovec vector;

    if (!context->firmware_running || !record ||
        length < sizeof(*record) || record->length != length - sizeof(*record) ||
        record->channel > BRCMF_BUS_CHANNEL_DATA)
    {
        return -RT_EINVAL;
    }
    if (record->channel != BRCMF_BUS_CHANNEL_DATA)
    {
        return brcmf_sdio_transmit_direct(context, record, length);
    }
    vector.data = data;
    vector.length = length;
    return brcmf_sdio_queue_data(context, &vector, 1);
}

static rt_err_t brcmf_sdio_bus_transmitv(
    struct rt_wlan_offload_bus *bus,
    const struct rt_wlan_offload_bus_iovec *vectors, rt_size_t vector_count)
{
    struct brcmf_context *context = rt_wlan_offload_bus_get_driver_data(bus);
    const struct brcmf_bus_record *record;

    if (!context->firmware_running || !vectors || vector_count != 2U ||
        vectors[0].length != sizeof(struct brcmf_bus_record) +
                             sizeof(struct brcmf_bcdc_header))
    {
        return -RT_EINVAL;
    }
    record = vectors[0].data;
    if (record->channel != BRCMF_BUS_CHANNEL_DATA ||
        record->length != sizeof(struct brcmf_bcdc_header) + vectors[1].length)
    {
        return -RT_EINVAL;
    }
    return brcmf_sdio_queue_data(context, vectors, vector_count);
}

static rt_err_t brcmf_sdio_bus_reset(struct rt_wlan_offload_bus *bus)
{
    struct brcmf_context *context = rt_wlan_offload_bus_get_driver_data(bus);
    rt_err_t result;

    context->firmware_running = RT_FALSE;
    context->tx_glom = RT_FALSE;
    context->tx_flow_control = 0;
    context->tx_flow_control_state = RT_FALSE;
    context->control_waiting = RT_FALSE;
    context->rx_next_length = 0;
    context->rx_glom_count = 0;
    context->rx_pending = RT_FALSE;
    context->rx_skip = RT_FALSE;
    rt_sem_release(context->tx_sem);
    brcmf_sdio_flush_tx_queue(context);
    result = brcmf_sdio_quiesce_f2(context);
    if (result != RT_EOK)
    {
        LOG_W("could not fully quiesce function 2 for reset: %d",
              result);
    }
    return brcmf_chip_set_passive(context);
}

static const struct rt_wlan_offload_bus_ops g_brcmf_bus_ops = {
    .start = brcmf_sdio_bus_start,
    .stop = brcmf_sdio_bus_stop,
    .transmit = brcmf_sdio_bus_transmit,
    .transmitv = brcmf_sdio_bus_transmitv,
    .reset = brcmf_sdio_bus_reset,
};

#ifdef RT_WLAN_MANAGE_ENABLE
static void brcmf_sdio_auto_start_work(struct rt_work *work, void *work_data)
{
    struct brcmf_context *context = work_data;

    (void)work;
    context->auto_start_work_queued = RT_FALSE;
    if (!context->tearing_down && context->radio_registered)
    {
        brcmf_wifi_auto_start(context);
    }
}
#endif

static rt_err_t brcmf_sdio_destroy_context(struct brcmf_context *context)
{
    rt_err_t detach_error = RT_EOK;
    rt_err_t result;

    if (!context)
    {
        return RT_EOK;
    }
    context->tearing_down = RT_TRUE;

#ifdef RT_WLAN_MANAGE_ENABLE
    if (context->auto_start_work_initialized)
    {
        (void)rt_workqueue_cancel_work_sync(
            g_brcmf_sdio_init_workqueue, &context->auto_start_work);
        context->auto_start_work_queued = RT_FALSE;
        context->auto_start_work_initialized = RT_FALSE;
    }
#endif

    /* External registries and IRQ dispatchers must release every reference
     * before any context-owned storage is destroyed. */
    if (context->radio_registered)
    {
        result = brcmf_wifi_detach(context);
        if (result != RT_EOK)
        {
            LOG_E("could not unregister Wi-Fi radio: %d", result);
            return -RT_EBUSY;
        }
    }
    if (context->bus_initialized)
    {
        result = rt_wlan_offload_bus_stop(&context->bus);
        if (result != RT_EOK)
        {
            LOG_E("could not stop offload bus: %d", result);
            return -RT_EBUSY;
        }
    }

    context->worker_running = RT_FALSE;
    context->tx_worker_running = RT_FALSE;
    if (context->irq_f2_attached || context->irq_f1_attached)
    {
        mmcsd_host_lock(context->card->host);
        context->card->host->flags |= MMCSD_SUP_SDIO_IRQ;
        if (context->card->host->ops->enable_sdio_irq)
        {
            context->card->host->ops->enable_sdio_irq(
                context->card->host, 0);
        }
        context->irq_deferred = RT_FALSE;
        if (context->irq_f2_attached)
        {
            result = sdio_detach_irq(context->function2);
            if (result == RT_EOK || !context->function2->irq_handler)
            {
                context->irq_f2_attached = RT_FALSE;
                if (result != RT_EOK)
                {
                    LOG_W("function 2 IRQ detached after CCCR error: %d",
                          result);
                }
            }
            else
            {
                LOG_E("could not detach function 2 IRQ: %d", result);
                detach_error = result;
            }
        }
        if (context->irq_f1_attached)
        {
            result = sdio_detach_irq(context->function1);
            if (result == RT_EOK || !context->function1->irq_handler)
            {
                context->irq_f1_attached = RT_FALSE;
                if (result != RT_EOK)
                {
                    LOG_W("function 1 IRQ detached after CCCR error: %d",
                          result);
                }
            }
            else
            {
                LOG_E("could not detach function 1 IRQ: %d", result);
                detach_error = result;
            }
        }
        mmcsd_host_unlock(context->card->host);
    }
    if (detach_error != RT_EOK)
    {
        return -RT_EBUSY;
    }

    if (context->rx_sem)
    {
        rt_sem_release(context->rx_sem);
    }
    if (context->tx_sem)
    {
        rt_sem_release(context->tx_sem);
    }
    brcmf_sdio_flush_tx_queue(context);
    if (context->worker_started)
    {
        rt_completion_wait(&context->worker_stopped, RT_WAITING_FOREVER);
        context->worker_started = RT_FALSE;
        context->rx_thread = RT_NULL;
    }
    else if (context->rx_thread)
    {
        rt_thread_delete(context->rx_thread);
        context->rx_thread = RT_NULL;
    }
    if (context->tx_worker_started)
    {
        rt_completion_wait(&context->tx_worker_stopped,
                           RT_WAITING_FOREVER);
        context->tx_worker_started = RT_FALSE;
        context->tx_thread = RT_NULL;
    }
    else if (context->tx_thread)
    {
        rt_thread_delete(context->tx_thread);
        context->tx_thread = RT_NULL;
    }
    if (context->bus_initialized)
    {
        result = rt_wlan_offload_bus_deinit(&context->bus);
        if (result != RT_EOK)
        {
            LOG_E("could not deinitialize offload bus: %d", result);
            return -RT_EBUSY;
        }
        context->bus_initialized = RT_FALSE;
    }
    if (context->function2 || context->function1)
    {
        mmcsd_host_lock(context->card->host);
        if (context->function2)
        {
            sdio_disable_func(context->function2);
        }
        if (context->function1)
        {
            sdio_disable_func(context->function1);
        }
        mmcsd_host_unlock(context->card->host);
    }
    if (context->function2)
    {
        sdio_set_drvdata(context->function2, RT_NULL);
    }
    if (context->function1)
    {
        sdio_set_drvdata(context->function1, RT_NULL);
    }
    if (context->rx_sem)
    {
        rt_sem_delete(context->rx_sem);
    }
    if (context->tx_sem)
    {
        rt_sem_delete(context->tx_sem);
    }
    if (context->tx_queue)
    {
        rt_mq_delete(context->tx_queue);
    }
    if (context->tx_pool)
    {
        rt_mp_delete(context->tx_pool);
    }
    rt_mutex_detach(&context->command_mutex);
    rt_mutex_detach(&context->io_mutex);
    rt_free(context->command_buffer);
    if (context->tx_buffer)
    {
        rt_free_align(context->tx_buffer);
    }
    if (context->rx_buffer)
    {
        rt_free_align(context->rx_buffer);
    }
    rt_free(context);
    return RT_EOK;
}

static rt_err_t brcmf_sdio_probe_cleanup(struct brcmf_context *context,
                                         rt_err_t probe_error)
{
    rt_err_t cleanup_error = brcmf_sdio_destroy_context(context);

    return cleanup_error == RT_EOK ? probe_error : cleanup_error;
}

static rt_int32_t brcmf_sdio_probe(struct rt_mmcsd_card *card)
{
    struct brcmf_context *context;
    struct rt_wlan_offload_bus_config bus_config;
    const char *stage = "validate card";
    rt_uint16_t vendor;
    rt_uint16_t product;
    rt_err_t result;

    if (!card || card->sdio_function_num < 2U ||
        !card->sdio_function[1] || !card->sdio_function[2])
    {
        LOG_E("probe rejected incomplete SDIO card");
        return -RT_EINVAL;
    }
    vendor = card->sdio_function[1]->manufacturer;
    product = card->sdio_function[1]->product;
    LOG_I("probing SDIO %04x:%04x with %u functions on %s",
          vendor, product, card->sdio_function_num,
          card->host ? card->host->name : "unknown");
    stage = "allocate context";
    context = rt_calloc(1, sizeof(*context));
    if (!context)
    {
        LOG_E("probe %04x:%04x failed at %s: %d", vendor, product,
              stage, -RT_ENOMEM);
        return -RT_ENOMEM;
    }
    context->card = card;
    context->function1 = card->sdio_function[1];
    context->function2 = card->sdio_function[2];
    context->f2_block_size = brcmf_sdio_f2_block_size(product);
    context->backplane_window = 0xffffffffU;
    result = rt_mutex_init(&context->io_mutex, "brcmf-io", RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        LOG_E("probe %04x:%04x failed at I/O mutex: %d", vendor,
              product, result);
        rt_free(context);
        return result;
    }
    result = rt_mutex_init(&context->command_mutex, "brcmf-cmd",
                           RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        LOG_E("probe %04x:%04x failed at command mutex: %d", vendor,
              product, result);
        rt_mutex_detach(&context->io_mutex);
        rt_free(context);
        return result;
    }
    rt_completion_init(&context->command_completion);
    rt_completion_init(&context->worker_stopped);
    rt_completion_init(&context->tx_worker_stopped);
    rt_completion_init(&context->ap_interface_completion);
    context->rx_sem = rt_sem_create("brcmf-rx", 0, RT_IPC_FLAG_FIFO);
    context->tx_sem = rt_sem_create("brcmf-tx", 0, RT_IPC_FLAG_FIFO);
    context->tx_pool = rt_mp_create(
        "brcmf-tp", BRCMF_TX_POOL_DEPTH,
        sizeof(struct brcmf_tx_record));
    context->tx_queue = rt_mq_create(
        "brcmf-tq", sizeof(struct brcmf_tx_record *),
        BRCMFMAC_TX_QUEUE_DEPTH, RT_IPC_FLAG_FIFO);
    context->rx_buffer = rt_malloc_align(BRCMF_RX_BUFFER_SIZE, 64U);
    context->tx_buffer = rt_malloc_align(BRCMF_RX_BUFFER_SIZE, 64U);
    context->command_buffer = rt_malloc(BRCMF_MAX_COMMAND);
    if (!context->rx_sem || !context->tx_sem || !context->tx_pool ||
        !context->tx_queue || !context->rx_buffer || !context->tx_buffer ||
        !context->command_buffer)
    {
        LOG_E("probe %04x:%04x failed at transport buffers: %d", vendor,
              product, -RT_ENOMEM);
        return brcmf_sdio_probe_cleanup(context, -RT_ENOMEM);
    }
    sdio_set_drvdata(context->function1, context);
    sdio_set_drvdata(context->function2, context);
    mmcsd_host_lock(card->host);
    stage = "enable function 1";
    result = sdio_enable_func(context->function1);
    if (result == RT_EOK)
    {
        stage = "set function 1 block size";
        result = sdio_set_block_size(context->function1,
                                     BRCMFMAC_SDIO_F1_BLOCK_SIZE);
    }
    if (result == RT_EOK)
    {
        stage = "set function 2 block size";
        result = sdio_set_block_size(context->function2,
                                     context->f2_block_size);
    }
    mmcsd_host_unlock(card->host);
    if (result != RT_EOK)
    {
        LOG_E("probe %04x:%04x failed at %s: %d", vendor, product,
              stage, result);
        return brcmf_sdio_probe_cleanup(context, result);
    }
    stage = "request ALP clock";
    result = brcmf_sdio_f1_writeb(
        context, SBSDIO_CHIPCLKCSR,
        SBSDIO_FORCE_HW_CLKREQ_OFF | SBSDIO_ALP_AVAIL_REQ);
    if (result == RT_EOK)
    {
        stage = "wait for ALP clock";
        result = brcmf_sdio_wait_clock(context, SBSDIO_ALP_AVAIL, 100U);
    }
    if (result == RT_EOK)
    {
        stage = "force ALP clock";
        result = brcmf_sdio_f1_writeb(
            context, SBSDIO_CHIPCLKCSR,
            SBSDIO_FORCE_HW_CLKREQ_OFF | SBSDIO_FORCE_ALP);
    }
    if (result == RT_EOK)
    {
        stage = "disable SDIO pull-up";
        result = brcmf_sdio_f1_writeb(context, SBSDIO_SDIOPULLUP, 0);
    }
    if (result == RT_EOK)
    {
        stage = "attach chip";
        result = brcmf_chip_attach(context);
    }
    if (result == RT_EOK)
    {
        stage = "select firmware";
        context->mapping = brcmf_firmware_find(
            vendor, product, context->chip.id, context->chip.revision);
        if (!context->mapping)
        {
            LOG_E("no firmware mapping for SDIO %04x:%04x chip %u rev %u",
                  vendor, product, context->chip.id, context->chip.revision);
            result = -RT_ENOSYS;
        }
    }
    if (result == RT_EOK)
    {
        stage = "attach function interrupts";
        /* Linux enables both CCCR function interrupts. Keep the host IRQ
         * masked in the callback until the deferred worker drains F2. */
        mmcsd_host_lock(card->host);
        result = sdio_attach_irq(context->function1, brcmf_sdio_irq);
        context->irq_f1_attached = result == RT_EOK;
        if (result == RT_EOK)
        {
            result = sdio_attach_irq(context->function2, brcmf_sdio_irq);
            context->irq_f2_attached = result == RT_EOK;
        }
        mmcsd_host_unlock(card->host);
        if (result == RT_EOK && card->host->ops->enable_sdio_irq)
        {
            card->host->ops->enable_sdio_irq(card->host, 1);
        }
    }
    rt_memset(&bus_config, 0, sizeof(bus_config));
    bus_config.type = RT_WLAN_OFFLOAD_BUS_SDIO;
    bus_config.ops = &g_brcmf_bus_ops;
    bus_config.capabilities = RT_WLAN_OFFLOAD_BUS_CAP_PACKET |
                              RT_WLAN_OFFLOAD_BUS_CAP_HOTPLUG;
    bus_config.max_tx_size = BRCMF_MAX_COMMAND;
    bus_config.max_rx_size = BRCMF_RX_BUFFER_SIZE;
    bus_config.alignment = 4U;
    bus_config.headroom = SDPCM_HEADER_SIZE + SDPCM_DATA_PAD;
    bus_config.driver_data = context;
    if (result == RT_EOK)
    {
        stage = "initialize offload bus";
        result = rt_wlan_offload_bus_init(&context->bus, &bus_config);
        context->bus_initialized = result == RT_EOK;
    }
    if (result == RT_EOK)
    {
        stage = "register Wi-Fi radio";
        result = brcmf_wifi_attach(context);
    }
    if (result == RT_EOK)
    {
        stage = "create transport workers";
        context->worker_running = RT_TRUE;
        context->tx_worker_running = RT_TRUE;
        context->rx_thread = rt_thread_create(
            "brcmf-rx", brcmf_sdio_worker, context,
            BRCMFMAC_RX_THREAD_STACK_SIZE, BRCMFMAC_RX_THREAD_PRIORITY, 10);
        context->tx_thread = rt_thread_create(
            "brcmf-tx", brcmf_sdio_tx_worker, context,
            BRCMFMAC_TX_THREAD_STACK_SIZE, BRCMFMAC_TX_THREAD_PRIORITY, 10);
        result = context->rx_thread && context->tx_thread ? RT_EOK :
                                                           -RT_ENOMEM;
    }
    if (result == RT_EOK)
    {
        stage = "start receive worker";
        result = rt_thread_startup(context->rx_thread);
        context->worker_started = result == RT_EOK;
    }
    if (result == RT_EOK)
    {
        stage = "start transmit worker";
        result = rt_thread_startup(context->tx_thread);
        context->tx_worker_started = result == RT_EOK;
    }
#ifdef RT_WLAN_MANAGE_ENABLE
    if (result == RT_EOK)
    {
        stage = "schedule default Wi-Fi interfaces";
        rt_work_init(&context->auto_start_work,
                     brcmf_sdio_auto_start_work, context);
        context->auto_start_work_initialized = RT_TRUE;
        context->auto_start_work_queued = RT_TRUE;
        result = rt_workqueue_submit_work(
            g_brcmf_sdio_init_workqueue, &context->auto_start_work,
            rt_tick_from_millisecond(BRCMF_AUTO_START_DELAY_MS));
        if (result != RT_EOK)
        {
            context->auto_start_work_queued = RT_FALSE;
            context->auto_start_work_initialized = RT_FALSE;
        }
    }
#endif
    if (result != RT_EOK)
    {
        LOG_E("probe %04x:%04x failed at %s: %d", vendor, product,
              stage, result);
        return brcmf_sdio_probe_cleanup(context, result);
    }
    LOG_I("bound SDIO %04x:%04x as %s (F1 block %u, F2 block %u)",
          vendor, product, context->mapping->model,
          BRCMFMAC_SDIO_F1_BLOCK_SIZE, context->f2_block_size);
    return RT_EOK;
}

static rt_int32_t brcmf_sdio_remove(struct rt_mmcsd_card *card)
{
    struct brcmf_context *context = RT_NULL;

    if (card && card->sdio_function_num >= 1U && card->sdio_function[1])
    {
        context = sdio_get_drvdata(card->sdio_function[1]);
    }
    return brcmf_sdio_destroy_context(context);
}

static struct rt_sdio_device_id g_brcmf_broadcom_id = {
    SDIO_ANY_FUNC_ID, BRCMF_SDIO_VENDOR_BROADCOM, SDIO_ANY_PROD_ID,
};
static struct rt_sdio_device_id g_brcmf_cypress_id = {
    SDIO_ANY_FUNC_ID, BRCMF_SDIO_VENDOR_CYPRESS, SDIO_ANY_PROD_ID,
};
static struct rt_sdio_driver g_brcmf_broadcom_driver = {
    "brcmfmac", brcmf_sdio_probe, brcmf_sdio_remove,
    &g_brcmf_broadcom_id,
};
static struct rt_sdio_driver g_brcmf_cypress_driver = {
    "brcmfmac-cypress", brcmf_sdio_probe, brcmf_sdio_remove,
    &g_brcmf_cypress_id,
};

static int brcmf_sdio_driver_init(void)
{
    rt_err_t broadcom;
    rt_err_t cypress;

#ifdef RT_WLAN_MANAGE_ENABLE
    if (!g_brcmf_sdio_init_workqueue)
    {
        g_brcmf_sdio_init_workqueue = rt_workqueue_create(
            "brcmf-init", BRCMFMAC_INIT_THREAD_STACK_SIZE,
            BRCMFMAC_INIT_THREAD_PRIORITY);
        if (!g_brcmf_sdio_init_workqueue)
        {
            return -RT_ENOMEM;
        }
    }
#endif

    broadcom = sdio_register_driver(&g_brcmf_broadcom_driver);
    cypress = sdio_register_driver(&g_brcmf_cypress_driver);

    if (broadcom != RT_EOK && broadcom != -RT_EEMPTY)
    {
        return broadcom;
    }
    return cypress == -RT_EEMPTY ? RT_EOK : cypress;
}
INIT_DEVICE_EXPORT(brcmf_sdio_driver_init);

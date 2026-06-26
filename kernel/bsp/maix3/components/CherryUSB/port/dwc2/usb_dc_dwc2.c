/*
 * Copyright (c) 2022, sakumisu
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "usbd_core.h"
#include "usb_dwc2_reg.h"

// clang-format off
#if   defined ( __CC_ARM )
#ifndef   __UNALIGNED_UINT32_WRITE
  #define __UNALIGNED_UINT32_WRITE(addr, val)    ((*((__packed uint32_t *)(addr))) = (val))
#endif
#ifndef   __UNALIGNED_UINT32_READ
  #define __UNALIGNED_UINT32_READ(addr)          (*((const __packed uint32_t *)(addr)))
#endif
#elif defined (__ARMCC_VERSION) && (__ARMCC_VERSION >= 6010050)
#ifndef   __UNALIGNED_UINT32_WRITE
  #pragma clang diagnostic push
  #pragma clang diagnostic ignored "-Wpacked"
/*lint -esym(9058, T_UINT32_WRITE)*/ /* disable MISRA 2012 Rule 2.4 for T_UINT32_WRITE */
  __PACKED_STRUCT T_UINT32_WRITE { uint32_t v; };
  #pragma clang diagnostic pop
  #define __UNALIGNED_UINT32_WRITE(addr, val)    (void)((((struct T_UINT32_WRITE *)(void *)(addr))->v) = (val))
#endif
#ifndef   __UNALIGNED_UINT32_READ
  #pragma clang diagnostic push
  #pragma clang diagnostic ignored "-Wpacked"
/*lint -esym(9058, T_UINT32_READ)*/ /* disable MISRA 2012 Rule 2.4 for T_UINT32_READ */
  __PACKED_STRUCT T_UINT32_READ { uint32_t v; };
  #pragma clang diagnostic pop
  #define __UNALIGNED_UINT32_READ(addr)          (((const struct T_UINT32_READ *)(const void *)(addr))->v)
#endif
#elif defined ( __GNUC__ )
#ifndef   __UNALIGNED_UINT32_WRITE
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wpacked"
  #pragma GCC diagnostic ignored "-Wattributes"
  __PACKED_STRUCT T_UINT32_WRITE { uint32_t v; };
  #pragma GCC diagnostic pop
  #define __UNALIGNED_UINT32_WRITE(addr, val)    (void)((((struct T_UINT32_WRITE *)(void *)(addr))->v) = (val))
#endif
#ifndef   __UNALIGNED_UINT32_READ
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wpacked"
  #pragma GCC diagnostic ignored "-Wattributes"
  __PACKED_STRUCT T_UINT32_READ { uint32_t v; };
  #pragma GCC diagnostic pop
  #define __UNALIGNED_UINT32_READ(addr)          (((const struct T_UINT32_READ *)(const void *)(addr))->v)
#endif
#endif
// clang-format on
#include "cache.h"

#ifndef CONFIG_USB_DWC2_RXALL_FIFO_SIZE
#define CONFIG_USB_DWC2_RXALL_FIFO_SIZE (320)
#endif

#ifndef CONFIG_USB_DWC2_TX0_FIFO_SIZE
#define CONFIG_USB_DWC2_TX0_FIFO_SIZE (64 / 4)
#endif

/* Relative weights used to share the spare DFIFO words across the active
 * endpoints (and the shared RX FIFO). Bulk/iso endpoints and RX gain real
 * throughput from extra depth; interrupt endpoints never need more than one
 * packet, so their default weight is 0. These are deliberately type-based so
 * the policy applies uniformly to every configuration (cdc/hid/mtp/adb/uvc,
 * single or dual). Override in usb_config.h to bias a particular workload. */
#ifndef CONFIG_USB_DWC2_FIFO_WEIGHT_RX
#define CONFIG_USB_DWC2_FIFO_WEIGHT_RX 4U
#endif
#ifndef CONFIG_USB_DWC2_FIFO_WEIGHT_BULK
#define CONFIG_USB_DWC2_FIFO_WEIGHT_BULK 4U
#endif
#ifndef CONFIG_USB_DWC2_FIFO_WEIGHT_ISO
#define CONFIG_USB_DWC2_FIFO_WEIGHT_ISO 4U
#endif
#ifndef CONFIG_USB_DWC2_FIFO_WEIGHT_INT
#define CONFIG_USB_DWC2_FIFO_WEIGHT_INT 0U
#endif

#define DWC2_DEVICE_TX_FIFO_COUNT 16U
#define DWC2_MIN_TX_FIFO_WORDS    16U

#define USBD_BASE (long)(g_usbdev_bus[0].reg_base)

#define USB_OTG_GLB      ((USB_OTG_GlobalTypeDef *)(USBD_BASE))
#define USB_OTG_DEV      ((USB_OTG_DeviceTypeDef *)(USBD_BASE + USB_OTG_DEVICE_BASE))
#define USB_OTG_PCGCCTL  *(__IO uint32_t *)((uint32_t)USBD_BASE + USB_OTG_PCGCCTL_BASE)
#define USB_OTG_INEP(i)  ((USB_OTG_INEndpointTypeDef *)(USBD_BASE + USB_OTG_IN_ENDPOINT_BASE + ((i)*USB_OTG_EP_REG_SIZE)))
#define USB_OTG_OUTEP(i) ((USB_OTG_OUTEndpointTypeDef *)(USBD_BASE + USB_OTG_OUT_ENDPOINT_BASE + ((i)*USB_OTG_EP_REG_SIZE)))
#define USB_OTG_FIFO(i)  *(__IO uint32_t *)(USBD_BASE + USB_OTG_FIFO_BASE + ((i)*USB_OTG_FIFO_SIZE))

extern uint32_t SystemCoreClock;

/* Endpoint state */
struct dwc2_ep_state {
    uint16_t ep_mps;    /* Endpoint max packet size */
    uint8_t ep_type;    /* Endpoint type */
    uint8_t ep_stalled; /* Endpoint stall flag */
    uint8_t *xfer_buf;
    uint32_t xfer_len;
    uint32_t actual_xfer_len;
};

/* Driver state */
USB_NOCACHE_RAM_SECTION struct dwc2_udc {
    USB_MEM_ALIGNX struct usb_setup_packet setup;
    USB_MEM_ALIGNX struct dwc2_ep_state in_ep[CONFIG_USBDEV_EP_NUM];  /*!< IN endpoint parameters*/
    struct dwc2_ep_state out_ep[CONFIG_USBDEV_EP_NUM]; /*!< OUT endpoint parameters */
} g_dwc2_udc;

/* Total device DFIFO depth (32-bit words), read from GHWCFG3 at init. */
static uint32_t g_dwc2_dfifo_depth;

/* TX FIFO words currently reserved for each IN endpoint (index 0 == EP0).
 * A value of 0 means "not allocated". The shared RX FIFO and EP0 TX FIFO are
 * reserved once at init; every other IN endpoint is allocated on demand when
 * it is opened and released when it is closed, so the DFIFO only ever holds
 * the endpoints of the configuration that is actually active. */
static uint16_t g_dwc2_in_fifo_words[CONFIG_USBDEV_EP_NUM];

#define __io_bw()   __asm__ __volatile__ ("fence w,o" : : : "memory");

static uint8_t ctrl_phase = 0;

#define DWC2_EP0_SETUP           1
#define DWC2_EP0_DATA_IN         2
#define DWC2_EP0_STATUS_OUT      3

void usb_dcache_invalidate(void *addr,int size)
{
#ifdef CHERRY_USB_DEVICE_USING_DMA
    rt_hw_cpu_dcache_invalidate(addr, size);
#endif
}

void usb_dcache_clean(void *addr,int size)
{
#ifdef CHERRY_USB_DEVICE_USING_DMA
    rt_hw_cpu_dcache_clean(addr, size);
#endif
}

static inline int dwc2_reset(void)
{
    volatile uint32_t count = 0U;
    uint32_t greset;
    
    USB_OTG_GLB->GRSTCTL |= USB_OTG_GRSTCTL_CSRST;
    /* Wait for CSftRstDone. */
    do {
        if (++count > 200000U) {
            USB_LOG_INFO("0USB_OTG_GLB->GRSTCTL = 0x%x\n", USB_OTG_GLB->GRSTCTL);
            return -1;
        }
    } while ((USB_OTG_GLB->GRSTCTL & (0x1 << 29)) == 0U);
    
    greset = USB_OTG_GLB->GRSTCTL;
    greset &= ~0x1;
    greset |= (0x1 << 29);
    USB_OTG_GLB->GRSTCTL = greset;

    /* Wait for AHB master IDLE state. */
    do {
        if (++count > 200000U) {
            return -1;
        }
    } while ((USB_OTG_GLB->GRSTCTL & USB_OTG_GRSTCTL_AHBIDL) == 0U);

    return 0;
}

static inline int dwc2_core_init(void)
{
    int ret;
#if defined(CONFIG_USB_HS)
    /* Init The ULPI Interface */
    USB_OTG_GLB->GUSBCFG &= ~(USB_OTG_GUSBCFG_TSDPS | USB_OTG_GUSBCFG_ULPIFSLS | USB_OTG_GUSBCFG_PHYSEL);

    /* Select vbus source */
    USB_OTG_GLB->GUSBCFG &= ~(USB_OTG_GUSBCFG_ULPIEVBUSD | USB_OTG_GUSBCFG_ULPIEVBUSI);

    /* Reset after a PHY select */
    ret = dwc2_reset();
#else
    /* Select FS Embedded PHY */
    USB_OTG_GLB->GUSBCFG |= USB_OTG_GUSBCFG_PHYSEL;

    /* Reset after a PHY select */
    ret = dwc2_reset();
#endif
    return ret;
}

static inline void dwc2_set_mode(uint8_t mode)
{
    USB_OTG_GLB->GUSBCFG &= ~(USB_OTG_GUSBCFG_FHMOD | USB_OTG_GUSBCFG_FDMOD);

    if (mode == USB_OTG_MODE_HOST) {
        USB_OTG_GLB->GUSBCFG |= USB_OTG_GUSBCFG_FHMOD;
    } else if (mode == USB_OTG_MODE_DEVICE) {
        USB_OTG_GLB->GUSBCFG |= USB_OTG_GUSBCFG_FDMOD;
    }
}

static inline int dwc2_flush_rxfifo(void)
{
    volatile uint32_t count = 0;

    USB_OTG_GLB->GRSTCTL = USB_OTG_GRSTCTL_RXFFLSH;

    do {
        if (++count > 200000U) {
            return -1;
        }
    } while ((USB_OTG_GLB->GRSTCTL & USB_OTG_GRSTCTL_RXFFLSH) == USB_OTG_GRSTCTL_RXFFLSH);

    return 0;
}

static inline int dwc2_flush_txfifo(uint32_t num)
{
    volatile uint32_t count = 0U;

    USB_OTG_GLB->GRSTCTL = (USB_OTG_GRSTCTL_TXFFLSH | (num << 6));

    do {
        if (++count > 200000U) {
            return -1;
        }
    } while ((USB_OTG_GLB->GRSTCTL & USB_OTG_GRSTCTL_TXFFLSH) == USB_OTG_GRSTCTL_TXFFLSH);

    return 0;
}

static void dwc2_set_turnaroundtime(uint32_t hclk, uint8_t speed)
{
    uint32_t UsbTrd;

    /* The USBTRD is configured according to the tables below, depending on AHB frequency
  used by application. In the low AHB frequency range it is used to stretch enough the USB response
  time to IN tokens, the USB turnaround time, so to compensate for the longer AHB read access
  latency to the Data FIFO */
    if (speed == USB_OTG_SPEED_FULL) {
        if ((hclk >= 14200000U) && (hclk < 15000000U)) {
            /* hclk Clock Range between 14.2-15 MHz */
            UsbTrd = 0xFU;
        } else if ((hclk >= 15000000U) && (hclk < 16000000U)) {
            /* hclk Clock Range between 15-16 MHz */
            UsbTrd = 0xEU;
        } else if ((hclk >= 16000000U) && (hclk < 17200000U)) {
            /* hclk Clock Range between 16-17.2 MHz */
            UsbTrd = 0xDU;
        } else if ((hclk >= 17200000U) && (hclk < 18500000U)) {
            /* hclk Clock Range between 17.2-18.5 MHz */
            UsbTrd = 0xCU;
        } else if ((hclk >= 18500000U) && (hclk < 20000000U)) {
            /* hclk Clock Range between 18.5-20 MHz */
            UsbTrd = 0xBU;
        } else if ((hclk >= 20000000U) && (hclk < 21800000U)) {
            /* hclk Clock Range between 20-21.8 MHz */
            UsbTrd = 0xAU;
        } else if ((hclk >= 21800000U) && (hclk < 24000000U)) {
            /* hclk Clock Range between 21.8-24 MHz */
            UsbTrd = 0x9U;
        } else if ((hclk >= 24000000U) && (hclk < 27700000U)) {
            /* hclk Clock Range between 24-27.7 MHz */
            UsbTrd = 0x8U;
        } else if ((hclk >= 27700000U) && (hclk < 32000000U)) {
            /* hclk Clock Range between 27.7-32 MHz */
            UsbTrd = 0x7U;
        } else /* if(hclk >= 32000000) */
        {
            /* hclk Clock Range between 32-200 MHz */
            UsbTrd = 0x6U;
        }
    } else if (speed == USB_OTG_SPEED_HIGH) {
        UsbTrd = USBD_HS_TRDT_VALUE;
    } else {
        UsbTrd = USBD_DEFAULT_TRDT_VALUE;
    }

    USB_OTG_GLB->GUSBCFG |= USB_OTG_GUSBCFG_TOCAL;

    USB_OTG_GLB->GUSBCFG &= ~USB_OTG_GUSBCFG_TRDT;
    USB_OTG_GLB->GUSBCFG |= (uint32_t)((UsbTrd << USB_OTG_GUSBCFG_TRDT_Pos) & USB_OTG_GUSBCFG_TRDT);
}

/* Words needed by one IN endpoint, derived from its transfer type and the
 * payload it must hold per (micro)frame. Bulk and isochronous endpoints are
 * double buffered so the controller can load the next packet while the current
 * one is still draining onto the bus; interrupt endpoints only ever need to
 * hold a single packet. The result is clamped to the controller's 16-word
 * minimum. mult is the transactions-per-(micro)frame count (1 for the common
 * case, 2/3 for high-bandwidth HS iso/interrupt endpoints). */
static uint16_t dwc2_required_txfifo_words(uint8_t ep_type, uint16_t mps, uint8_t mult)
{
    uint32_t words = (((uint32_t)mps * (uint32_t)mult) + 3U) / 4U;

    switch (ep_type) {
        case USB_ENDPOINT_TYPE_BULK:
        case USB_ENDPOINT_TYPE_ISOCHRONOUS:
            words *= 2U;
            break;
        default:
            break;
    }

    if (words < DWC2_MIN_TX_FIFO_WORDS) {
        words = DWC2_MIN_TX_FIFO_WORDS;
    }

    return (uint16_t)words;
}

/* Per-endpoint share of the spare DFIFO, by transfer type. Type-based so the
 * same policy fits every configuration without a per-endpoint table. */
static uint32_t dwc2_fifo_weight(uint8_t ep_type)
{
    switch (ep_type) {
        case USB_ENDPOINT_TYPE_BULK:
            return CONFIG_USB_DWC2_FIFO_WEIGHT_BULK;
        case USB_ENDPOINT_TYPE_ISOCHRONOUS:
            return CONFIG_USB_DWC2_FIFO_WEIGHT_ISO;
        case USB_ENDPOINT_TYPE_INTERRUPT:
            return CONFIG_USB_DWC2_FIFO_WEIGHT_INT;
        default:
            return 0U;
    }
}

/* Lay out the shared RX FIFO and every IN-endpoint TX FIFO. This is re-run
 * whenever an endpoint is opened or closed, so closing an endpoint reclaims its
 * space for the next one.
 *
 * g_dwc2_in_fifo_words[] holds only the *minimum* each endpoint needs. Whatever
 * DFIFO is left after the minimums is shared out by weight (see
 * dwc2_fifo_weight() and CONFIG_USB_DWC2_FIFO_WEIGHT_RX) so the controller RAM
 * is fully used: bulk/iso TX FIFOs and the shared RX FIFO grow, while interrupt
 * FIFOs (weight 0 by default) keep just their minimum. RX absorbs the rounding
 * remainder, so the layout always ends exactly at the DFIFO depth.
 *
 * Re-packing only happens on open/close (configuration changes), never during
 * steady-state transfers, and endpoints are opened in ascending index order,
 * so a FIFO that is actively in use is never relocated underneath traffic.
 *
 * Returns -1 (without committing an overflowing layout) if the minimum set
 * does not fit in the controller DFIFO. */
static int dwc2_repack_txfifos(void)
{
    uint32_t rx_base = CONFIG_USB_DWC2_RXALL_FIFO_SIZE;
    uint32_t base = rx_base + g_dwc2_in_fifo_words[0];
    uint32_t rx_weight = 0U;
    uint8_t top = 0U;

    /* The shared RX FIFO only competes for the surplus when it actually carries
     * bulk/iso OUT data; if every OUT endpoint is control/interrupt (e.g. UVC,
     * which has only the ISO IN stream) its minimum is plenty and the spare
     * words should go to the IN FIFOs instead. If nothing benefits from depth
     * at all (a pure interrupt config), rx_weight stays 0 and RX simply absorbs
     * the whole surplus below, so the DFIFO is still fully used. */
    for (uint8_t i = 1U; i < CONFIG_USBDEV_EP_NUM; i++) {
        uint8_t t = g_dwc2_udc.out_ep[i].ep_type;
        if ((t == USB_ENDPOINT_TYPE_BULK) || (t == USB_ENDPOINT_TYPE_ISOCHRONOUS)) {
            rx_weight = CONFIG_USB_DWC2_FIFO_WEIGHT_RX;
            break;
        }
    }

    uint32_t weight_total = rx_weight;

    /* Highest index in use. */
    for (uint8_t i = 1U; i < CONFIG_USBDEV_EP_NUM; i++) {
        if (g_dwc2_in_fifo_words[i] != 0U) {
            top = i;
        }
    }

    /* Sum the minimums (real endpoints + the 16-word floor reserved for any
     * unused FIFO that sits below the top one) and the distribution weights. */
    for (uint8_t i = 1U; i <= top; i++) {
        if (g_dwc2_in_fifo_words[i] == 0U) {
            base += DWC2_MIN_TX_FIFO_WORDS;
            continue;
        }
        base += g_dwc2_in_fifo_words[i];
        weight_total += dwc2_fifo_weight(g_dwc2_udc.in_ep[i].ep_type);
    }

    uint32_t surplus = (base < g_dwc2_dfifo_depth) ? (g_dwc2_dfifo_depth - base) : 0U;

    /* Pre-compute the TX share so RX can take the remainder (keeps the total
     * exactly at the DFIFO depth despite integer rounding). */
    uint32_t tx_bonus_total = 0U;
    if ((surplus != 0U) && (weight_total != 0U)) {
        for (uint8_t i = 1U; i <= top; i++) {
            if (g_dwc2_in_fifo_words[i] == 0U) {
                continue;
            }
            tx_bonus_total += (uint32_t)(((uint64_t)surplus * dwc2_fifo_weight(g_dwc2_udc.in_ep[i].ep_type)) / weight_total);
        }
    }

    uint32_t rx_size = rx_base + (surplus - tx_bonus_total);

    USB_OTG_GLB->GRXFSIZ = rx_size;

    uint32_t offset = rx_size;
    USB_OTG_GLB->DIEPTXF0_HNPTXFSIZ = ((uint32_t)g_dwc2_in_fifo_words[0] << 16) | offset;
    offset += g_dwc2_in_fifo_words[0];

    for (uint8_t i = 1U; i < CONFIG_USBDEV_EP_NUM; i++) {
        uint32_t words = g_dwc2_in_fifo_words[i];

        if ((words == 0U) && (i < top)) {
            words = DWC2_MIN_TX_FIFO_WORDS;
        }
        if (words == 0U) {
            USB_OTG_GLB->DIEPTXF[i - 1U] = 0U;
            continue;
        }

        if ((surplus != 0U) && (weight_total != 0U) && (g_dwc2_in_fifo_words[i] != 0U)) {
            words += (uint32_t)(((uint64_t)surplus * dwc2_fifo_weight(g_dwc2_udc.in_ep[i].ep_type)) / weight_total);
        }

        USB_OTG_GLB->DIEPTXF[i - 1U] = ((uint32_t)words << 16) | offset;
        offset += words;
    }

    if (offset > g_dwc2_dfifo_depth) {
        return -1;
    }

    return 0;
}

/* Dump the active TX/RX FIFO layout straight from the controller registers,
 * so the printed sizes reflect the minimum + distributed surplus actually in
 * effect. Offsets/sizes are in 32-bit words (bytes shown for convenience). */
static void dwc2_dump_fifo_config(void)
{
    uint32_t rx = USB_OTG_GLB->GRXFSIZ & 0xFFFFU;
    uint32_t reg = USB_OTG_GLB->DIEPTXF0_HNPTXFSIZ;
    uint32_t off = reg & 0xFFFFU;
    uint32_t size = (reg >> 16) & 0xFFFFU;
    uint32_t total = rx;

    USB_LOG_INFO("==== dwc2 fifo map (dfifo depth %u words) ====\r\n", g_dwc2_dfifo_depth);
    USB_LOG_INFO(" rx       off:%-5u size:%-5u (%u bytes)\r\n", 0U, rx, rx * 4U);
    USB_LOG_INFO(" ep0  in  off:%-5u size:%-5u (%u bytes)\r\n", off, size, size * 4U);
    total += size;

    for (uint8_t i = 1U; i < CONFIG_USBDEV_EP_NUM; i++) {
        reg = USB_OTG_GLB->DIEPTXF[i - 1U];
        off = reg & 0xFFFFU;
        size = (reg >> 16) & 0xFFFFU;
        if (size == 0U) {
            continue;
        }
        USB_LOG_INFO(" ep%-2u in  off:%-5u size:%-5u (%u bytes)\r\n", i, off, size, size * 4U);
        total += size;
    }

    USB_LOG_INFO("==== dwc2 fifo used %u/%u words ====\r\n", total, g_dwc2_dfifo_depth);
}

static uint8_t dwc2_get_devspeed(void)
{
    uint8_t speed;
    uint32_t DevEnumSpeed = USB_OTG_DEV->DSTS & USB_OTG_DSTS_ENUMSPD;

    if (DevEnumSpeed == DSTS_ENUMSPD_HS_PHY_30MHZ_OR_60MHZ) {
        speed = USB_OTG_SPEED_HIGH;
    } else if ((DevEnumSpeed == DSTS_ENUMSPD_FS_PHY_30MHZ_OR_60MHZ) ||
               (DevEnumSpeed == DSTS_ENUMSPD_FS_PHY_48MHZ)) {
        speed = USB_OTG_SPEED_FULL;
    } else {
        speed = 0xFU;
    }

    return speed;
}

static void dwc2_ep0_start_read_setup(uint8_t *psetup)
{
    USB_OTG_OUTEP(0U)->DOEPTSIZ = 0U;
    USB_OTG_OUTEP(0U)->DOEPTSIZ |= (USB_OTG_DOEPTSIZ_PKTCNT & (1U << 19));
    USB_OTG_OUTEP(0U)->DOEPTSIZ |= (3U * 8U);
    USB_OTG_OUTEP(0U)->DOEPTSIZ |= USB_OTG_DOEPTSIZ_STUPCNT;

#ifdef CHERRY_USB_DEVICE_USING_DMA
    ctrl_phase = DWC2_EP0_SETUP;
    usb_dcache_invalidate((void *)psetup, CONFIG_USB_ALIGN_SIZE);
    __io_bw();
    USB_OTG_OUTEP(0U)->DOEPDMA = (uint32_t)((uint64_t)psetup);
    /* EP enable */
    USB_OTG_OUTEP(0U)->DOEPCTL |= USB_OTG_DOEPCTL_EPENA | USB_OTG_DOEPCTL_USBAEP;
#endif
}

void dwc2_ep_write(uint8_t ep_idx, uint8_t *src, uint16_t len)
{
    uint32_t *pSrc = (uint32_t *)src;
    uint32_t count32b, i;

    count32b = ((uint32_t)len + 3U) / 4U;
    for (i = 0U; i < count32b; i++) {
        USB_OTG_FIFO((uint32_t)ep_idx) = __UNALIGNED_UINT32_READ(pSrc);
        pSrc++;
    }
}

void dwc2_ep_read(uint8_t *dest, uint16_t len)
{
    uint32_t *pDest = (uint32_t *)dest;
    uint32_t i;
    uint32_t count32b = ((uint32_t)len + 3U) / 4U;

    for (i = 0U; i < count32b; i++) {
        __UNALIGNED_UINT32_WRITE(pDest, USB_OTG_FIFO(0U));
        pDest++;
    }
}

static void dwc2_tx_fifo_empty_procecss(uint8_t ep_idx)
{
    uint32_t len;
    uint32_t len32b;
    uint32_t fifoemptymsk;

    len = g_dwc2_udc.in_ep[ep_idx].xfer_len - g_dwc2_udc.in_ep[ep_idx].actual_xfer_len;
    if (len > g_dwc2_udc.in_ep[ep_idx].ep_mps) {
        len = g_dwc2_udc.in_ep[ep_idx].ep_mps;
    }

    len32b = (len + 3U) / 4U;

    while (((USB_OTG_INEP(ep_idx)->DTXFSTS & USB_OTG_DTXFSTS_INEPTFSAV) >= len32b) &&
           (g_dwc2_udc.in_ep[ep_idx].actual_xfer_len < g_dwc2_udc.in_ep[ep_idx].xfer_len) && (g_dwc2_udc.in_ep[ep_idx].xfer_len != 0U)) {
        /* Write the FIFO */
        len = g_dwc2_udc.in_ep[ep_idx].xfer_len - g_dwc2_udc.in_ep[ep_idx].actual_xfer_len;
        if (len > g_dwc2_udc.in_ep[ep_idx].ep_mps) {
            len = g_dwc2_udc.in_ep[ep_idx].ep_mps;
        }

        dwc2_ep_write(ep_idx, g_dwc2_udc.in_ep[ep_idx].xfer_buf, len);
        g_dwc2_udc.in_ep[ep_idx].xfer_buf += len;
        g_dwc2_udc.in_ep[ep_idx].actual_xfer_len += len;
    }

    if (g_dwc2_udc.in_ep[ep_idx].xfer_len <= g_dwc2_udc.in_ep[ep_idx].actual_xfer_len) {
        fifoemptymsk = (uint32_t)(0x1UL << (ep_idx & 0x0f));
        USB_OTG_DEV->DIEPEMPMSK &= ~fifoemptymsk;
    }
}

/**
  * @brief  dwc2_get_glb_intstatus: return the global USB interrupt status
  * @retval status
  */
static inline uint32_t dwc2_get_glb_intstatus(void)
{
    uint32_t tmpreg;

    tmpreg = USB_OTG_GLB->GINTSTS;
    tmpreg &= USB_OTG_GLB->GINTMSK;

    return tmpreg;
}

/**
  * @brief  dwc2_get_outeps_intstatus: return the USB device OUT endpoints interrupt status
  * @retval status
  */
static inline uint32_t dwc2_get_outeps_intstatus(void)
{
    uint32_t tmpreg;

    tmpreg = USB_OTG_DEV->DAINT;
    tmpreg &= USB_OTG_DEV->DAINTMSK;

    return ((tmpreg & 0xffff0000U) >> 16);
}

/**
  * @brief  dwc2_get_ineps_intstatus: return the USB device IN endpoints interrupt status
  * @retval status
  */
static inline uint32_t dwc2_get_ineps_intstatus(void)
{
    uint32_t tmpreg;

    tmpreg = USB_OTG_DEV->DAINT;
    tmpreg &= USB_OTG_DEV->DAINTMSK;

    return ((tmpreg & 0xFFFFU));
}

/**
  * @brief  Returns Device OUT EP Interrupt register
  * @param  epnum  endpoint number
  *          This parameter can be a value from 0 to 15
  * @retval Device OUT EP Interrupt register
  */
static inline uint32_t dwc2_get_outep_intstatus(uint8_t epnum)
{
    uint32_t tmpreg;

    tmpreg = USB_OTG_OUTEP((uint32_t)epnum)->DOEPINT;
    USB_OTG_OUTEP((uint32_t)epnum)->DOEPINT = tmpreg;
    tmpreg &= USB_OTG_DEV->DOEPMSK;

    //rt_kprintf("O_%d = 0x%x 0x%x [%d]\n", epnum, tmpreg, USB_OTG_DEV->DOEPMSK, ctrl_phase);
    return tmpreg;
}

/**
  * @brief  Returns Device IN EP Interrupt register
  * @param  epnum  endpoint number
  *          This parameter can be a value from 0 to 15
  * @retval Device IN EP Interrupt register
  */
static inline uint32_t dwc2_get_inep_intstatus(uint8_t epnum)
{
    uint32_t tmpreg, msk, emp;

    msk = USB_OTG_DEV->DIEPMSK;
    emp = USB_OTG_DEV->DIEPEMPMSK;
    msk |= ((emp >> (epnum & 0x07)) & 0x1U) << 7;
    tmpreg = USB_OTG_INEP((uint32_t)epnum)->DIEPINT;
    USB_OTG_INEP((uint32_t)epnum)->DIEPINT = tmpreg;
    tmpreg = tmpreg & msk;
    //rt_kprintf("I_%d = 0x%x 0x%x [%d]\n", epnum, tmpreg, msk, ctrl_phase);

    return tmpreg;
}

__WEAK void usb_dc_low_level_init(void)
{
}

__WEAK void usb_dc_low_level_deinit(void)
{
}

int usb_dc_init(uint8_t busid)
{
    int ret;
    uint8_t fsphy_type;
    uint8_t hsphy_type;
    uint8_t dma_support;
    uint8_t endpoints;
    uint32_t dfifo_depth;

    usb_memset(&g_dwc2_udc, 0, sizeof(struct dwc2_udc));

    usb_dc_low_level_init();

    /*
        Full-Speed PHY Interface Type (FSPhyType)
        2'b00: Full-speed interface not supported
        2'b01: Dedicated full-speed interface
        2'b10: FS pins shared with UTMI+ pins
        2'b11: FS pins shared with ULPI pins

        High-Speed PHY Interface Type (HSPhyType)
        2'b00: High-Speed interface not supported
        2'b01: UTMI+
        2'b10: ULPI
        2'b11: UTMI+ and ULPI

        Architecture (OtgArch)
        2'b00: Slave-Only
        2'b01: External DMA
        2'b10: Internal DMA
        Others: Reserved
    */
    fsphy_type = ((USB_OTG_GLB->GHWCFG2 & (0x03 << 8)) >> 8);
    hsphy_type = ((USB_OTG_GLB->GHWCFG2 & (0x03 << 6)) >> 6);
    dma_support = ((USB_OTG_GLB->GHWCFG2 & (0x03 << 3)) >> 3);
    endpoints = ((USB_OTG_GLB->GHWCFG2 & (0x0f << 10)) >> 10) + 1;
    dfifo_depth = (USB_OTG_GLB->GHWCFG3 >> 16);

    USB_LOG_INFO("========== dwc2 udc params ==========\r\n");
    USB_LOG_INFO("CID:%08x\r\n", USB_OTG_GLB->CID);
    USB_LOG_INFO("GSNPSID:%08x\r\n", USB_OTG_GLB->GSNPSID);
    USB_LOG_INFO("GHWCFG1:%08x\r\n", USB_OTG_GLB->GHWCFG1);
    USB_LOG_INFO("GHWCFG2:%08x\r\n", USB_OTG_GLB->GHWCFG2);
    USB_LOG_INFO("GHWCFG3:%08x\r\n", USB_OTG_GLB->GHWCFG3);
    USB_LOG_INFO("GHWCFG4:%08x\r\n", USB_OTG_GLB->GHWCFG4);

    USB_LOG_INFO("dwc2 fsphy type:%d, hsphy type:%d, dma support:%d\r\n", fsphy_type, hsphy_type, dma_support);
    USB_LOG_INFO("dwc2 has %d endpoints and dfifo depth(32-bit words) is %d, default config: %d endpoints\r\n", endpoints, dfifo_depth, CONFIG_USBDEV_EP_NUM);
    USB_LOG_INFO("=================================\r\n");

    if (endpoints < CONFIG_USBDEV_EP_NUM) {
        USB_LOG_ERR("dwc2 has less endpoints than config, please check\r\n");
        while (1) {
        }
    }

    if (CONFIG_USBDEV_EP_NUM > DWC2_DEVICE_TX_FIFO_COUNT) {
        USB_LOG_ERR("dwc2 device fifo config supports up to %u endpoint indexes, please check\r\n", DWC2_DEVICE_TX_FIFO_COUNT);
        while (1) {
        }
    }

    USB_OTG_DEV->DCTL |= USB_OTG_DCTL_SDIS;

    USB_OTG_GLB->GAHBCFG &= ~USB_OTG_GAHBCFG_GINT;

    /* This is vendor register */
    USB_OTG_GLB->GCCFG = usbd_get_dwc2_gccfg_conf(USBD_BASE);

    ret = dwc2_core_init();

    /* Force Device Mode*/
    dwc2_set_mode(USB_OTG_MODE_DEVICE);

    /* B-peripheral session valid override enable */
    // USB_OTG_GLB->GOTGCTL |= USB_OTG_GOTGCTL_BVALOEN;
    // USB_OTG_GLB->GOTGCTL |= USB_OTG_GOTGCTL_BVALOVAL;

    for (uint8_t i = 0U; i < 15U; i++) {
        USB_OTG_GLB->DIEPTXF[i] = 0U;
    }

    /* Restart the Phy Clock */
    USB_OTG_PCGCCTL = 0U;

    /* Device speed configuration */
    USB_OTG_DEV->DCFG &= ~USB_OTG_DCFG_DSPD;
#if defined(CONFIG_USB_HS)
    USB_OTG_DEV->DCFG |= USB_OTG_SPEED_HIGH;
#else
    if (hsphy_type == 0) {
        USB_OTG_DEV->DCFG |= USB_OTG_SPEED_FULL;
    } else {
        USB_OTG_DEV->DCFG |= USB_OTG_SPEED_HIGH_IN_FULL;
    }
#endif

    /* Clear all pending Device Interrupts */
    USB_OTG_DEV->DIEPMSK = 0U;
    USB_OTG_DEV->DOEPMSK = 0U;
    USB_OTG_DEV->DAINTMSK = 0U;

    /* Disable all interrupts. */
    USB_OTG_GLB->GINTMSK = 0U;
    /* Clear any pending interrupts */
    USB_OTG_GLB->GINTSTS = 0xBFFFFFFFU;

    /* Enable interrupts matching to the Device mode ONLY */
    USB_OTG_GLB->GINTMSK = USB_OTG_GINTMSK_USBRST | USB_OTG_GINTMSK_ENUMDNEM |
                           USB_OTG_GINTMSK_OEPINT | USB_OTG_GINTMSK_IEPINT |
                           USB_OTG_GINTMSK_IISOIXFRM | USB_OTG_GINTMSK_PXFRM_IISOOXFRM;

#ifdef CHERRY_USB_DEVICE_USING_DMA
    if (((USB_OTG_GLB->GHWCFG2 & (0x3U << 3)) >> 3) != 2) {
        USB_LOG_ERR("This dwc2 version does not support dma mode, so stop working\r\n");
        while (1) {
        }
    }

    USB_OTG_DEV->DCFG &= ~USB_OTG_DCFG_DESCDMA;
    USB_OTG_GLB->GAHBCFG |= (USB_OTG_GAHBCFG_DMAEN | USB_OTG_GAHBCFG_HBSTLEN_4);
#else
    USB_OTG_GLB->GINTMSK |= USB_OTG_GINTMSK_RXFLVLM;
#endif
#if CONFIG_DWC2_VBUS_SENSING
    USB_OTG_GLB->GINTMSK |= (USB_OTG_GINTMSK_OTGINT | USB_OTG_GINTMSK_SRQIM);
#endif
#if 0
    USB_OTG_GLB->GINTMSK |= USB_OTG_GINTMSK_SOFM;
#endif

    g_dwc2_dfifo_depth = dfifo_depth;

    /* Reserve the shared RX FIFO minimum and the EP0 TX FIFO up front. Every
     * other IN-endpoint TX FIFO is allocated dynamically in usbd_ep_open() from
     * the endpoint's type/MPS, and any spare DFIFO is shared out by weight in
     * dwc2_repack_txfifos(), so the layout always matches the active config. */
    usb_memset(g_dwc2_in_fifo_words, 0, sizeof(g_dwc2_in_fifo_words));
    g_dwc2_in_fifo_words[0] = CONFIG_USB_DWC2_TX0_FIFO_SIZE;

    if ((CONFIG_USB_DWC2_RXALL_FIFO_SIZE + CONFIG_USB_DWC2_TX0_FIFO_SIZE) > dfifo_depth) {
        USB_LOG_ERR("dwc2 rx+ep0 fifo needs %u words but controller has only %u words\r\n",
                    (uint32_t)(CONFIG_USB_DWC2_RXALL_FIFO_SIZE + CONFIG_USB_DWC2_TX0_FIFO_SIZE), dfifo_depth);
        while (1) {
        }
    }

    dwc2_repack_txfifos();
    USB_LOG_INFO("dwc2 dfifo depth %u words, rx min:%u ep0-tx:%u, per-ep tx allocated on open\r\n",
                 dfifo_depth, (uint32_t)CONFIG_USB_DWC2_RXALL_FIFO_SIZE, (uint32_t)CONFIG_USB_DWC2_TX0_FIFO_SIZE);

    ret = dwc2_flush_txfifo(0x10U);
    ret = dwc2_flush_rxfifo();

    USB_OTG_GLB->GAHBCFG |= USB_OTG_GAHBCFG_GINT;
    USB_OTG_DEV->DCTL &= ~USB_OTG_DCTL_SDIS;

    return ret;
}

int usb_dc_deinit(uint8_t busid)
{
    USB_OTG_GLB->GAHBCFG &= ~USB_OTG_GAHBCFG_GINT;
    USB_OTG_DEV->DCTL |= USB_OTG_DCTL_SDIS;

    /* Disable all interrupts before clearing endpoint state. */
    USB_OTG_GLB->GINTMSK = 0U;

    /* Clear Pending interrupt */
    for (uint8_t i = 0U; i < 15U; i++) {
        USB_OTG_INEP(i)->DIEPINT = 0xFB7FU;
        USB_OTG_OUTEP(i)->DOEPINT = 0xFB7FU;
    }

    /* Clear interrupt masks */
    USB_OTG_DEV->DIEPMSK = 0U;
    USB_OTG_DEV->DOEPMSK = 0U;
    USB_OTG_DEV->DAINTMSK = 0U;

    /* Flush the FIFO */
    dwc2_flush_txfifo(0x10U);
    dwc2_flush_rxfifo();

    USB_OTG_GLB->GINTSTS = 0xBFFFFFFFU;

    usb_dc_low_level_deinit();
    return 0;
}

int usbd_set_address(uint8_t busid, const uint8_t addr)
{
    USB_OTG_DEV->DCFG &= ~(USB_OTG_DCFG_DAD);
    USB_OTG_DEV->DCFG |= ((uint32_t)addr << 4) & USB_OTG_DCFG_DAD;
    return 0;
}

uint8_t usbd_get_port_speed(uint8_t busid, const uint8_t port)
{
    uint8_t speed;
    uint32_t DevEnumSpeed = USB_OTG_DEV->DSTS & USB_OTG_DSTS_ENUMSPD;

    if (DevEnumSpeed == DSTS_ENUMSPD_HS_PHY_30MHZ_OR_60MHZ) {
        speed = USB_SPEED_HIGH;
    } else if ((DevEnumSpeed == DSTS_ENUMSPD_FS_PHY_30MHZ_OR_60MHZ) ||
               (DevEnumSpeed == DSTS_ENUMSPD_FS_PHY_48MHZ)) {
        speed = USB_SPEED_FULL;
    } else {
        speed = USB_SPEED_FULL;
    }

    return speed;
}

int usbd_ep_open(uint8_t busid, const struct usb_endpoint_descriptor *ep)
{
    uint8_t ep_idx = USB_EP_GET_IDX(ep->bEndpointAddress);

    if (ep_idx > (CONFIG_USBDEV_EP_NUM - 1)) {
        USB_LOG_ERR("Ep addr %02x overflow\r\n", ep->bEndpointAddress);
        return -1;
    }

    if (USB_EP_DIR_IS_OUT(ep->bEndpointAddress)) {
        g_dwc2_udc.out_ep[ep_idx].ep_mps = USB_GET_MAXPACKETSIZE(ep->wMaxPacketSize);
        g_dwc2_udc.out_ep[ep_idx].ep_type = USB_GET_ENDPOINT_TYPE(ep->bmAttributes);

        USB_OTG_DEV->DAINTMSK |= USB_OTG_DAINTMSK_OEPM & (uint32_t)(1UL << (16 + ep_idx));

        if ((USB_OTG_OUTEP(ep_idx)->DOEPCTL & USB_OTG_DOEPCTL_USBAEP) == 0) {
            USB_OTG_OUTEP(ep_idx)->DOEPCTL |= (USB_GET_MAXPACKETSIZE(ep->wMaxPacketSize) & USB_OTG_DOEPCTL_MPSIZ) |
                                              ((uint32_t)USB_GET_ENDPOINT_TYPE(ep->bmAttributes) << 18) |
                                              USB_OTG_DIEPCTL_SD0PID_SEVNFRM |
                                              USB_OTG_DOEPCTL_USBAEP;
        }

        /* A bulk/iso OUT endpoint changes how much of the surplus RX deserves,
         * so re-pack here too (regardless of whether OUT or IN opened first). */
        if (ep_idx != 0) {
            (void)dwc2_repack_txfifos();
            dwc2_dump_fifo_config();
        }
    } else {
        uint16_t mps = USB_GET_MAXPACKETSIZE(ep->wMaxPacketSize);
        uint8_t ep_type = USB_GET_ENDPOINT_TYPE(ep->bmAttributes);

        /* Record type/mps first: dwc2_repack_txfifos() weights the spare DFIFO
         * by endpoint type, so it must see this endpoint's type before it runs. */
        g_dwc2_udc.in_ep[ep_idx].ep_mps = mps;
        g_dwc2_udc.in_ep[ep_idx].ep_type = ep_type;

        if (ep_idx != 0) {
            /* bits[12:11] of wMaxPacketSize carry the additional transactions
             * per microframe for high-bandwidth HS iso/interrupt endpoints. */
            uint8_t mult = 1U + (uint8_t)((ep->wMaxPacketSize >> 11) & 0x03U);

            g_dwc2_in_fifo_words[ep_idx] = dwc2_required_txfifo_words(ep_type, mps, mult);
            if (dwc2_repack_txfifos() != 0) {
                g_dwc2_in_fifo_words[ep_idx] = 0U;
                (void)dwc2_repack_txfifos();
                USB_LOG_ERR("Ep addr %02x tx fifo alloc overflow\r\n", ep->bEndpointAddress);
                return -2;
            }
            dwc2_dump_fifo_config();
        }

        USB_OTG_DEV->DAINTMSK |= USB_OTG_DAINTMSK_IEPM & (uint32_t)(1UL << ep_idx);

        if ((USB_OTG_INEP(ep_idx)->DIEPCTL & USB_OTG_DIEPCTL_USBAEP) == 0) {
            USB_OTG_INEP(ep_idx)->DIEPCTL |= (USB_GET_MAXPACKETSIZE(ep->wMaxPacketSize) & USB_OTG_DIEPCTL_MPSIZ) |
                                             ((uint32_t)USB_GET_ENDPOINT_TYPE(ep->bmAttributes) << 18) | (ep_idx << 22) |
                                             USB_OTG_DIEPCTL_SD0PID_SEVNFRM |
                                             USB_OTG_DIEPCTL_USBAEP;
        }
        dwc2_flush_txfifo(ep_idx);
    }
    return 0;
}

int usbd_ep_close(uint8_t busid, const uint8_t ep)
{
    uint8_t ep_idx = USB_EP_GET_IDX(ep);
    volatile uint32_t count = 0U;

    if (USB_EP_DIR_IS_OUT(ep)) {
        if (USB_OTG_OUTEP(ep_idx)->DOEPCTL & USB_OTG_DOEPCTL_EPENA) {
            USB_OTG_OUTEP(ep_idx)->DOEPCTL |= USB_OTG_DOEPCTL_SNAK;
            USB_OTG_OUTEP(ep_idx)->DOEPCTL |= USB_OTG_DOEPCTL_EPDIS;

            /* Wait for endpoint disabled interrupt */
            count = 0;
            do {
                if (++count > 50000) {
                    break;
                }
            } while ((USB_OTG_OUTEP(ep_idx)->DOEPINT & USB_OTG_DOEPINT_EPDISD) != USB_OTG_DOEPINT_EPDISD);

            /* Clear and unmask endpoint disabled interrupt */
            USB_OTG_OUTEP(ep_idx)->DOEPINT |= USB_OTG_DOEPINT_EPDISD;
        }

        USB_OTG_DEV->DEACHMSK &= ~(USB_OTG_DAINTMSK_OEPM & ((uint32_t)(1UL << (ep_idx & 0x07)) << 16));
        USB_OTG_DEV->DAINTMSK &= ~(USB_OTG_DAINTMSK_OEPM & ((uint32_t)(1UL << (ep_idx & 0x07)) << 16));
        USB_OTG_OUTEP(ep_idx)->DOEPCTL = 0;

        /* Drop this OUT endpoint's contribution to the RX weight and re-pack. */
        if (ep_idx != 0U) {
            g_dwc2_udc.out_ep[ep_idx].ep_type = USB_ENDPOINT_TYPE_CONTROL;
            (void)dwc2_repack_txfifos();
        }
    } else {
        if (USB_OTG_INEP(ep_idx)->DIEPCTL & USB_OTG_DIEPCTL_EPENA) {
            USB_OTG_INEP(ep_idx)->DIEPCTL |= USB_OTG_DIEPCTL_SNAK;
            USB_OTG_INEP(ep_idx)->DIEPCTL |= USB_OTG_DIEPCTL_EPDIS;

            /* Wait for endpoint disabled interrupt */
            count = 0;
            do {
                if (++count > 50000) {
                    break;
                }
            } while ((USB_OTG_INEP(ep_idx)->DIEPINT & USB_OTG_DIEPINT_EPDISD) != USB_OTG_DIEPINT_EPDISD);

            /* Clear and unmask endpoint disabled interrupt */
            USB_OTG_INEP(ep_idx)->DIEPINT |= USB_OTG_DIEPINT_EPDISD;
        }

        USB_OTG_DEV->DEACHMSK &= ~(USB_OTG_DAINTMSK_IEPM & (uint32_t)(1UL << (ep_idx & 0x07)));
        USB_OTG_DEV->DAINTMSK &= ~(USB_OTG_DAINTMSK_IEPM & (uint32_t)(1UL << (ep_idx & 0x07)));
        USB_OTG_INEP(ep_idx)->DIEPCTL = 0;

        /* Release this endpoint's TX FIFO so the space can be reused. */
        if (ep_idx != 0U) {
            g_dwc2_in_fifo_words[ep_idx] = 0U;
            (void)dwc2_repack_txfifos();
        }
    }
    return 0;
}

int usbd_ep_set_stall(uint8_t busid, const uint8_t ep)
{
    uint8_t ep_idx = USB_EP_GET_IDX(ep);

    if (USB_EP_DIR_IS_OUT(ep)) {
        if (((USB_OTG_OUTEP(ep_idx)->DOEPCTL & USB_OTG_DOEPCTL_EPENA) == 0U) && (ep_idx != 0U)) {
            USB_OTG_OUTEP(ep_idx)->DOEPCTL &= ~(USB_OTG_DOEPCTL_EPDIS);
        }
        USB_OTG_OUTEP(ep_idx)->DOEPCTL |= USB_OTG_DOEPCTL_STALL;
    } else {
        if (((USB_OTG_INEP(ep_idx)->DIEPCTL & USB_OTG_DIEPCTL_EPENA) == 0U) && (ep_idx != 0U)) {
            USB_OTG_INEP(ep_idx)->DIEPCTL &= ~(USB_OTG_DIEPCTL_EPDIS);
        }
        USB_OTG_INEP(ep_idx)->DIEPCTL |= USB_OTG_DIEPCTL_STALL;
    }
#ifdef CHERRY_USB_DEVICE_USING_DMA
    if (ep_idx == 0) {
        dwc2_ep0_start_read_setup((uint8_t *)&g_dwc2_udc.setup);
    }
#endif
    return 0;
}

int usbd_ep_clear_stall(uint8_t busid, const uint8_t ep)
{
    uint8_t ep_idx = USB_EP_GET_IDX(ep);

    if (USB_EP_DIR_IS_OUT(ep)) {
        USB_OTG_OUTEP(ep_idx)->DOEPCTL &= ~USB_OTG_DOEPCTL_STALL;
        if ((g_dwc2_udc.out_ep[ep_idx].ep_type == USB_ENDPOINT_TYPE_INTERRUPT) ||
            (g_dwc2_udc.out_ep[ep_idx].ep_type == USB_ENDPOINT_TYPE_BULK)) {
            USB_OTG_OUTEP(ep_idx)->DOEPCTL |= USB_OTG_DOEPCTL_SD0PID_SEVNFRM; /* DATA0 */
        }
    } else {
        USB_OTG_INEP(ep_idx)->DIEPCTL &= ~USB_OTG_DIEPCTL_STALL;
        if ((g_dwc2_udc.in_ep[ep_idx].ep_type == USB_ENDPOINT_TYPE_INTERRUPT) ||
            (g_dwc2_udc.in_ep[ep_idx].ep_type == USB_ENDPOINT_TYPE_BULK)) {
            USB_OTG_INEP(ep_idx)->DIEPCTL |= USB_OTG_DIEPCTL_SD0PID_SEVNFRM; /* DATA0 */
        }
    }
    return 0;
}

int usbd_ep_is_stalled(uint8_t busid, const uint8_t ep, uint8_t *stalled)
{
    if (USB_EP_DIR_IS_OUT(ep)) {
    } else {
    }
    return 0;
}

int usbd_ep_start_write(uint8_t busid, const uint8_t ep, const uint8_t *data, uint32_t data_len)
{
    uint8_t ep_idx = USB_EP_GET_IDX(ep);
    uint32_t pktcnt = 0;

    if (!data && data_len) {
        return -1;
    }
#if 0 /* some chips have confused with this, so disable as default */
    if (USB_OTG_INEP(ep_idx)->DIEPCTL & USB_OTG_DIEPCTL_EPENA) {
        return -2;
    }
#endif
    if (ep_idx && !(USB_OTG_INEP(ep_idx)->DIEPCTL & USB_OTG_DIEPCTL_MPSIZ)) {
        USB_LOG_ERR("ep 0x%x wrong mps\n", ep);
        return -3;
    }
    if ((uint32_t)(long)data & 0x03) {
        return -4;
    }

    g_dwc2_udc.in_ep[ep_idx].xfer_buf = (uint8_t *)data;
    g_dwc2_udc.in_ep[ep_idx].xfer_len = data_len;
    g_dwc2_udc.in_ep[ep_idx].actual_xfer_len = 0;

#ifdef CHERRY_USB_DEVICE_USING_DMA
    if (((unsigned long)data % CONFIG_USB_ALIGN_SIZE) != 0) {
        USB_LOG_ERR("write buffer not align to cache line\n");
    }
#endif

    USB_OTG_INEP(ep_idx)->DIEPTSIZ &= ~(USB_OTG_DIEPTSIZ_PKTCNT);
    USB_OTG_INEP(ep_idx)->DIEPTSIZ &= ~(USB_OTG_DIEPTSIZ_XFRSIZ);

    if (data_len == 0) {
        USB_OTG_INEP(ep_idx)->DIEPTSIZ |= (USB_OTG_DIEPTSIZ_PKTCNT & (1U << 19));
        USB_OTG_INEP(ep_idx)->DIEPCTL |= (USB_OTG_DIEPCTL_CNAK | USB_OTG_DIEPCTL_EPENA);
        return 0;
    }

    if (ep_idx == 0) {
        if (data_len > g_dwc2_udc.in_ep[ep_idx].ep_mps) {
            data_len = g_dwc2_udc.in_ep[ep_idx].ep_mps;
        }
        g_dwc2_udc.in_ep[ep_idx].xfer_len = data_len;
        USB_OTG_INEP(ep_idx)->DIEPTSIZ |= (USB_OTG_DIEPTSIZ_PKTCNT & (1U << 19));
        USB_OTG_INEP(ep_idx)->DIEPTSIZ |= (USB_OTG_DIEPTSIZ_XFRSIZ & data_len);
    } else {
        pktcnt = (uint16_t)((data_len + g_dwc2_udc.in_ep[ep_idx].ep_mps - 1U) / g_dwc2_udc.in_ep[ep_idx].ep_mps);

        USB_OTG_INEP(ep_idx)->DIEPTSIZ |= (USB_OTG_DIEPTSIZ_PKTCNT & (pktcnt << 19));
        USB_OTG_INEP(ep_idx)->DIEPTSIZ |= (USB_OTG_DIEPTSIZ_XFRSIZ & data_len);
    }

    if (g_dwc2_udc.in_ep[ep_idx].ep_type == USB_ENDPOINT_TYPE_ISOCHRONOUS) {
        if ((USB_OTG_DEV->DSTS & (1U << 8)) == 0U) {
            USB_OTG_INEP(ep_idx)->DIEPCTL &= ~USB_OTG_DIEPCTL_SD0PID_SEVNFRM;
            USB_OTG_INEP(ep_idx)->DIEPCTL |= USB_OTG_DIEPCTL_SODDFRM;
        } else {
            USB_OTG_INEP(ep_idx)->DIEPCTL &= ~USB_OTG_DIEPCTL_SODDFRM;
            USB_OTG_INEP(ep_idx)->DIEPCTL |= USB_OTG_DIEPCTL_SD0PID_SEVNFRM;
        }
        USB_OTG_INEP(ep_idx)->DIEPTSIZ &= ~(USB_OTG_DIEPTSIZ_MULCNT);
        USB_OTG_INEP(ep_idx)->DIEPTSIZ |= (USB_OTG_DIEPTSIZ_MULCNT & (1U << 29));
    }

#ifdef CHERRY_USB_DEVICE_USING_DMA
    usb_dcache_clean((void *)data, USB_ALIGN_UP(data_len, CONFIG_USB_ALIGN_SIZE));
    __io_bw();
    USB_OTG_INEP(ep_idx)->DIEPDMA = (uint32_t)((uint64_t)data);

    USB_OTG_INEP(ep_idx)->DIEPCTL |= (USB_OTG_DIEPCTL_CNAK | USB_OTG_DIEPCTL_EPENA);
#else
    USB_OTG_INEP(ep_idx)->DIEPCTL |= (USB_OTG_DIEPCTL_CNAK | USB_OTG_DIEPCTL_EPENA);
    /* Enable the Tx FIFO Empty Interrupt for this EP */
    if (data_len > 0U) {
        USB_OTG_DEV->DIEPEMPMSK |= 1UL << (ep_idx & 0x0f);
    }
#endif
    return 0;
}

int usbd_ep_start_read(uint8_t busid, const uint8_t ep, uint8_t *data, uint32_t data_len)
{
    uint8_t ep_idx = USB_EP_GET_IDX(ep);
    uint32_t pktcnt = 0;

    if (!data && data_len) {
        return -1;
    }
#if 0 /* some chips have confused with this, so disable as default */
    if (USB_OTG_OUTEP(ep_idx)->DOEPCTL & USB_OTG_DOEPCTL_EPENA) {
        return -2;
    }
#endif

    if (ep_idx && !(USB_OTG_OUTEP(ep_idx)->DOEPCTL & USB_OTG_DOEPCTL_MPSIZ)) {
        USB_LOG_ERR("ep 0x%x  wrong mps\n", ep);
        return -3;
    }
    if (((uint32_t)(long)data) & 0x03) {
        return -4;
    }

    g_dwc2_udc.out_ep[ep_idx].xfer_buf = (uint8_t *)data;
    g_dwc2_udc.out_ep[ep_idx].xfer_len = data_len;
    g_dwc2_udc.out_ep[ep_idx].actual_xfer_len = 0;
#ifdef CHERRY_USB_DEVICE_USING_DMA
    if (((unsigned long)data % CONFIG_USB_ALIGN_SIZE) != 0) {
        USB_LOG_ERR("read buffer not align to cache line\n");
    }
#endif

    USB_OTG_OUTEP(ep_idx)->DOEPTSIZ &= ~(USB_OTG_DOEPTSIZ_PKTCNT);
    USB_OTG_OUTEP(ep_idx)->DOEPTSIZ &= ~(USB_OTG_DOEPTSIZ_XFRSIZ);
    if (data_len == 0) {
        USB_OTG_OUTEP(ep_idx)->DOEPTSIZ |= (USB_OTG_DOEPTSIZ_PKTCNT & (1 << 19));
        USB_OTG_OUTEP(ep_idx)->DOEPTSIZ |= (USB_OTG_DOEPTSIZ_XFRSIZ & g_dwc2_udc.out_ep[ep_idx].ep_mps);
        USB_OTG_OUTEP(ep_idx)->DOEPCTL |= (USB_OTG_DOEPCTL_CNAK | USB_OTG_DOEPCTL_EPENA);
        return 0;
    }

    if (ep_idx == 0) {
        if (data_len > g_dwc2_udc.out_ep[ep_idx].ep_mps) {
            data_len = g_dwc2_udc.out_ep[ep_idx].ep_mps;
        }
        g_dwc2_udc.out_ep[ep_idx].xfer_len = data_len;
        USB_OTG_OUTEP(ep_idx)->DOEPTSIZ |= (USB_OTG_DOEPTSIZ_PKTCNT & (1U << 19));
        USB_OTG_OUTEP(ep_idx)->DOEPTSIZ |= (USB_OTG_DOEPTSIZ_XFRSIZ & data_len);
    } else {
        pktcnt = (uint16_t)((data_len + g_dwc2_udc.out_ep[ep_idx].ep_mps - 1U) / g_dwc2_udc.out_ep[ep_idx].ep_mps);

        USB_OTG_OUTEP(ep_idx)->DOEPTSIZ |= (USB_OTG_DOEPTSIZ_PKTCNT & (pktcnt << 19));
        USB_OTG_OUTEP(ep_idx)->DOEPTSIZ |= (USB_OTG_DOEPTSIZ_XFRSIZ & data_len);
    }

#ifdef CHERRY_USB_DEVICE_USING_DMA
    usb_dcache_invalidate((void *)data, USB_ALIGN_UP(data_len, CONFIG_USB_ALIGN_SIZE));
    __io_bw();
    USB_OTG_OUTEP(ep_idx)->DOEPDMA = (uint32_t)((uint64_t)data);
#endif
    if (g_dwc2_udc.out_ep[ep_idx].ep_type == USB_ENDPOINT_TYPE_ISOCHRONOUS) {
        if ((USB_OTG_DEV->DSTS & (1U << 8)) == 0U) {
            USB_OTG_OUTEP(ep_idx)->DOEPCTL &= ~USB_OTG_DOEPCTL_SD0PID_SEVNFRM;
            USB_OTG_OUTEP(ep_idx)->DOEPCTL |= USB_OTG_DOEPCTL_SODDFRM;
        } else {
            USB_OTG_OUTEP(ep_idx)->DOEPCTL &= ~USB_OTG_DOEPCTL_SODDFRM;
            USB_OTG_OUTEP(ep_idx)->DOEPCTL |= USB_OTG_DOEPCTL_SD0PID_SEVNFRM;
        }
    }
    USB_OTG_OUTEP(ep_idx)->DOEPCTL |= (USB_OTG_DOEPCTL_CNAK | USB_OTG_DOEPCTL_EPENA);
    return 0;
}

void USBD_IRQHandler(uint8_t busid)
{
    uint32_t gint_status, temp, ep_idx, ep_intr, epint, read_count, daintmask;
    gint_status = dwc2_get_glb_intstatus();

    if ((USB_OTG_GLB->GINTSTS & 0x1U) == USB_OTG_MODE_DEVICE) {
        /* Avoid spurious interrupt */
        if (gint_status == 0) {
            return;
        }

#ifndef CHERRY_USB_DEVICE_USING_DMA
        /* Handle RxQLevel Interrupt */
        if (gint_status & USB_OTG_GINTSTS_RXFLVL) {
            USB_MASK_INTERRUPT(USB_OTG_GLB, USB_OTG_GINTSTS_RXFLVL);

            temp = USB_OTG_GLB->GRXSTSP;
            ep_idx = temp & USB_OTG_GRXSTSP_EPNUM;

            if (((temp & USB_OTG_GRXSTSP_PKTSTS) >> USB_OTG_GRXSTSP_PKTSTS_Pos) == STS_DATA_UPDT) {
                read_count = (temp & USB_OTG_GRXSTSP_BCNT) >> 4;
                if (read_count != 0) {
                    dwc2_ep_read(g_dwc2_udc.out_ep[ep_idx].xfer_buf, read_count);
                    g_dwc2_udc.out_ep[ep_idx].xfer_buf += read_count;
                }
            } else if (((temp & USB_OTG_GRXSTSP_PKTSTS) >> USB_OTG_GRXSTSP_PKTSTS_Pos) == STS_SETUP_UPDT) {
                read_count = (temp & USB_OTG_GRXSTSP_BCNT) >> 4;
                dwc2_ep_read((uint8_t *)&g_dwc2_udc.setup, read_count);
            } else {
                /* ... */
            }
            USB_UNMASK_INTERRUPT(USB_OTG_GLB, USB_OTG_GINTSTS_RXFLVL);
        }
#endif
        if (gint_status & USB_OTG_GINTSTS_OEPINT) {
            ep_idx = 0;
            ep_intr = dwc2_get_outeps_intstatus();
            while (ep_intr != 0U) {
                if ((ep_intr & 0x1U) != 0U) {
                    epint = dwc2_get_outep_intstatus(ep_idx);

                    if ((epint & USB_OTG_DOEPINT_XFRC) == USB_OTG_DOEPINT_XFRC) {
                        if (ep_idx == 0) {

                            if (ctrl_phase == DWC2_EP0_SETUP) {
                                goto check_setup;
                            }

                            if (g_dwc2_udc.out_ep[ep_idx].xfer_len == 0) {
                                /* Out status, start reading setup */
                                dwc2_ep0_start_read_setup((uint8_t *)&g_dwc2_udc.setup);
                            } else {
                                g_dwc2_udc.out_ep[ep_idx].actual_xfer_len = g_dwc2_udc.out_ep[ep_idx].xfer_len - ((USB_OTG_OUTEP(ep_idx)->DOEPTSIZ) & USB_OTG_DOEPTSIZ_XFRSIZ);
                                g_dwc2_udc.out_ep[ep_idx].xfer_len = 0;
                                usb_dcache_invalidate((void *)g_dwc2_udc.out_ep[ep_idx].xfer_buf, USB_ALIGN_UP(g_dwc2_udc.out_ep[ep_idx].actual_xfer_len, CONFIG_USB_ALIGN_SIZE));
                                usbd_event_ep_out_complete_handler(0, 0x00, g_dwc2_udc.out_ep[ep_idx].actual_xfer_len);
                            }
                        } else {
                            g_dwc2_udc.out_ep[ep_idx].actual_xfer_len = g_dwc2_udc.out_ep[ep_idx].xfer_len - ((USB_OTG_OUTEP(ep_idx)->DOEPTSIZ) & USB_OTG_DOEPTSIZ_XFRSIZ);
                            g_dwc2_udc.out_ep[ep_idx].xfer_len = 0;
                            usb_dcache_invalidate((void *)g_dwc2_udc.out_ep[ep_idx].xfer_buf, USB_ALIGN_UP(g_dwc2_udc.out_ep[ep_idx].actual_xfer_len, CONFIG_USB_ALIGN_SIZE));
                            usbd_event_ep_out_complete_handler(0, ep_idx, g_dwc2_udc.out_ep[ep_idx].actual_xfer_len);
                        }
                    }

check_setup:
                    if ((epint & USB_OTG_DOEPINT_STUP) == USB_OTG_DOEPINT_STUP) {
                        if (ctrl_phase == DWC2_EP0_SETUP) {
                            ctrl_phase = 0;
                        }
                        if (ep_idx != 0) {
                            USB_LOG_DBG("setup done in ep%d\n", ep_idx);
                        }
                        usb_dcache_invalidate((void *)&g_dwc2_udc.setup, USB_ALIGN_UP(8, CONFIG_USB_ALIGN_SIZE));
                        usbd_event_ep0_setup_complete_handler(0, (uint8_t *)&g_dwc2_udc.setup);
                    }
                }
                ep_intr >>= 1U;
                ep_idx++;
            }
        }
        if (gint_status & USB_OTG_GINTSTS_IEPINT) {
            ep_idx = 0U;
            ep_intr = dwc2_get_ineps_intstatus();
            while (ep_intr != 0U) {
                if ((ep_intr & 0x1U) != 0U) {
                    epint = dwc2_get_inep_intstatus(ep_idx);

                    if ((epint & USB_OTG_DIEPINT_XFRC) == USB_OTG_DIEPINT_XFRC) {
                        if (ep_idx == 0) {
                            g_dwc2_udc.in_ep[ep_idx].actual_xfer_len = g_dwc2_udc.in_ep[ep_idx].xfer_len - ((USB_OTG_INEP(ep_idx)->DIEPTSIZ) & USB_OTG_DIEPTSIZ_XFRSIZ);
                            g_dwc2_udc.in_ep[ep_idx].xfer_len = 0;
                            usbd_event_ep_in_complete_handler(0, 0x80, g_dwc2_udc.in_ep[ep_idx].actual_xfer_len);

                            if (g_dwc2_udc.setup.wLength && ((g_dwc2_udc.setup.bmRequestType & USB_REQUEST_DIR_MASK) == USB_REQUEST_DIR_OUT)) {
                                /* In status, start reading setup */
                                dwc2_ep0_start_read_setup((uint8_t *)&g_dwc2_udc.setup);
                            } else if (g_dwc2_udc.setup.wLength == 0) {
                                /* In status, start reading setup */
                                dwc2_ep0_start_read_setup((uint8_t *)&g_dwc2_udc.setup);
                            }
                        } else {
                            g_dwc2_udc.in_ep[ep_idx].actual_xfer_len = g_dwc2_udc.in_ep[ep_idx].xfer_len - ((USB_OTG_INEP(ep_idx)->DIEPTSIZ) & USB_OTG_DIEPTSIZ_XFRSIZ);
                            g_dwc2_udc.in_ep[ep_idx].xfer_len = 0;
                            usbd_event_ep_in_complete_handler(0, ep_idx | 0x80, g_dwc2_udc.in_ep[ep_idx].actual_xfer_len);
                        }
                    }
#ifndef CHERRY_USB_DEVICE_USING_DMA
                    if ((epint & USB_OTG_DIEPINT_TXFE) == USB_OTG_DIEPINT_TXFE) {
                        dwc2_tx_fifo_empty_procecss(ep_idx);
                    }
#endif
                }
                ep_intr >>= 1U;
                ep_idx++;
            }
        }
        if (gint_status & USB_OTG_GINTSTS_USBRST) {
            USB_OTG_GLB->GINTSTS |= USB_OTG_GINTSTS_USBRST;
            USB_OTG_DEV->DCTL &= ~USB_OTG_DCTL_RWUSIG;

            dwc2_flush_txfifo(0x10U);
            dwc2_flush_rxfifo();

            for (uint8_t i = 0U; i < CONFIG_USBDEV_EP_NUM; i++) {
                if (i == 0U) {
                    USB_OTG_INEP(i)->DIEPCTL = USB_OTG_DIEPCTL_SNAK;
                    USB_OTG_OUTEP(i)->DOEPCTL = USB_OTG_DOEPCTL_SNAK;
                } else {
                    if (USB_OTG_INEP(i)->DIEPCTL & USB_OTG_DIEPCTL_EPENA) {
                        USB_OTG_INEP(i)->DIEPCTL = (USB_OTG_DIEPCTL_EPDIS | USB_OTG_DIEPCTL_SNAK);
                    } else {
                        USB_OTG_INEP(i)->DIEPCTL = 0;
                    }
                    if (USB_OTG_OUTEP(i)->DOEPCTL & USB_OTG_DOEPCTL_EPENA) {
                        USB_OTG_OUTEP(i)->DOEPCTL = (USB_OTG_DOEPCTL_EPDIS | USB_OTG_DOEPCTL_SNAK);
                    } else {
                        USB_OTG_OUTEP(i)->DOEPCTL = 0;
                    }
                }
                USB_OTG_INEP(i)->DIEPTSIZ = 0U;
                USB_OTG_INEP(i)->DIEPINT = 0xFBFFU;
                USB_OTG_OUTEP(i)->DOEPTSIZ = 0U;
                USB_OTG_OUTEP(i)->DOEPINT = 0xFBFFU;
            }

            USB_OTG_DEV->DAINTMSK |= 0x10001U;

            USB_OTG_DEV->DOEPMSK = USB_OTG_DOEPMSK_STUPM |
                                   USB_OTG_DOEPMSK_XFRCM;

            USB_OTG_DEV->DIEPMSK = USB_OTG_DIEPMSK_XFRCM;

            usb_memset(&g_dwc2_udc, 0, sizeof(struct dwc2_udc));

            /* Drop all per-endpoint TX FIFO reservations (keep RX + EP0); they
             * are re-allocated as the host re-opens endpoints on enumeration. */
            for (uint8_t i = 1U; i < CONFIG_USBDEV_EP_NUM; i++) {
                g_dwc2_in_fifo_words[i] = 0U;
            }
            (void)dwc2_repack_txfifos();

            usbd_event_reset_handler(0);
            /* Start reading setup */
            dwc2_ep0_start_read_setup((uint8_t *)&g_dwc2_udc.setup);
        }
        if (gint_status & USB_OTG_GINTSTS_ENUMDNE) {
            USB_OTG_GLB->GINTSTS |= USB_OTG_GINTSTS_ENUMDNE;
            dwc2_set_turnaroundtime(SystemCoreClock, dwc2_get_devspeed());

            USB_OTG_DEV->DCTL |= USB_OTG_DCTL_CGINAK;
        }
        if (gint_status & USB_OTG_GINTSTS_PXFR_INCOMPISOOUT) {
            daintmask = USB_OTG_DEV->DAINTMSK;
            daintmask >>= 16;

            for (ep_idx = 1; ep_idx < CONFIG_USBDEV_EP_NUM; ep_idx++) {
                if ((BIT(ep_idx) & ~daintmask) || (g_dwc2_udc.out_ep[ep_idx].ep_type != USB_ENDPOINT_TYPE_ISOCHRONOUS))
                    continue;
                if (!(USB_OTG_OUTEP(ep_idx)->DOEPCTL & USB_OTG_DOEPCTL_USBAEP))
                    continue;

                if ((USB_OTG_DEV->DSTS & (1U << 8)) != 0U) {
                    USB_OTG_OUTEP(ep_idx)->DOEPCTL |= USB_OTG_DOEPCTL_SD0PID_SEVNFRM;
                    USB_OTG_OUTEP(ep_idx)->DOEPCTL &= ~USB_OTG_DOEPCTL_SODDFRM;
                } else {
                    USB_OTG_OUTEP(ep_idx)->DOEPCTL &= ~USB_OTG_DOEPCTL_SD0PID_SEVNFRM;
                    USB_OTG_OUTEP(ep_idx)->DOEPCTL |= USB_OTG_DOEPCTL_SODDFRM;
                }
            }

            USB_OTG_GLB->GINTSTS |= USB_OTG_GINTSTS_PXFR_INCOMPISOOUT;
        }

        if (gint_status & USB_OTG_GINTSTS_IISOIXFR) {
            daintmask = USB_OTG_DEV->DAINTMSK;
            daintmask >>= 16;

            for (ep_idx = 1; ep_idx < CONFIG_USBDEV_EP_NUM; ep_idx++) {
                if (((BIT(ep_idx) & ~daintmask)) || (g_dwc2_udc.in_ep[ep_idx].ep_type != USB_ENDPOINT_TYPE_ISOCHRONOUS))
                    continue;

                if (!(USB_OTG_INEP(ep_idx)->DIEPCTL & USB_OTG_DIEPCTL_USBAEP))
                    continue;

                if ((USB_OTG_DEV->DSTS & (1U << 8)) != 0U) {
                    USB_OTG_INEP(ep_idx)->DIEPCTL |= USB_OTG_DIEPCTL_SD0PID_SEVNFRM;
                    USB_OTG_INEP(ep_idx)->DIEPCTL &= ~USB_OTG_DIEPCTL_SODDFRM;
                } else {
                    USB_OTG_INEP(ep_idx)->DIEPCTL &= ~USB_OTG_DIEPCTL_SD0PID_SEVNFRM;
                    USB_OTG_INEP(ep_idx)->DIEPCTL |= USB_OTG_DIEPCTL_SODDFRM;
                }
            }
            USB_OTG_GLB->GINTSTS |= USB_OTG_GINTSTS_IISOIXFR;
        }

        if (gint_status & USB_OTG_GINTSTS_SOF) {
            USB_OTG_GLB->GINTSTS |= USB_OTG_GINTSTS_SOF;
        }
        if (gint_status & USB_OTG_GINTSTS_USBSUSP) {
            USB_OTG_GLB->GINTSTS |= USB_OTG_GINTSTS_USBSUSP;
        }
        if (gint_status & USB_OTG_GINTSTS_WKUINT) {
            USB_OTG_GLB->GINTSTS |= USB_OTG_GINTSTS_WKUINT;
        }
        if (gint_status & USB_OTG_GINTSTS_OTGINT) {
            temp = USB_OTG_GLB->GOTGINT;
            if ((temp & USB_OTG_GOTGINT_SEDET) == USB_OTG_GOTGINT_SEDET) {
            } else {
            }
            USB_OTG_GLB->GOTGINT |= temp;
        }
    }
}

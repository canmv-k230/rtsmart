#ifndef __DWC2_DEBUG_H__
#define __DWC2_DEBUG_H__

#define CONFIG_USB_DWC2_DEBUG_PERIODIC
//#define USE_CHERRY_ROOTHUB_CONTROL
//#define DWC2_SCH_DBG
//#define DWC2_DEBUG
//#define DEBUG
//#define VERBOSE_DEBUG
//#define DEBUG_SOF

#define INTERRUPT_MALLOC (1)
/* relate  UVC_MAX_PACKETS in usbh_uvc_thr.c */
#define MAX_ISO_PACKET (32)
#define DWC2_MAX_URB (128)
#define DWC2_MAX_ISO_URB (64)
#define DWC2_MAX_ENDPOINT (64)
#define DWC2_MAX_CHILD (10)

#define DWC2_BH_HANDLE (1)

#ifdef DWC2_DEBUG

#define dev_dbg(dev, format, arg...)        \
    ({                                      \
     if (1)                                 \
     USB_LOG_DBG(format, ##arg);            \
     })

#else

#define dev_dbg(dev, format, arg...)

#endif

#define dev_vdbg dev_dbg

#define dev_info(dev, format, arg...)       \
    ({                                      \
     if (1)                                 \
     USB_LOG_INFO(format, ##arg);           \
     })

#define dev_warn(dev, format, arg...)       \
    ({                                      \
     if (1)                                 \
     USB_LOG_WRN(format, ##arg);            \
     })

#define dev_err(dev, format, arg...)        \
    ({                                      \
     if (1)                                 \
     USB_LOG_ERR(format, ##arg);            \
     })

#ifdef CANAAN

#define DDD(...) rt_kprintf(__VA_ARGS__)

#else

#define DDD(...)

#endif

#endif

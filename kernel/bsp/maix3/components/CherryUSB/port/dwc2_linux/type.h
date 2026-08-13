#ifndef __DWC2_TYPE_H__
#define __DWC2_TYPE_H__

#include <stdint.h>
#include <stdbool.h>

/* TODO:
 * 1. dma_sync_single_for_cpu (void * + 1)
 * 4. reset , phy , vbus
 * 6. tt (dwc2_hc_handle_tt_clear, Transaction err)
 * 10. roothub_c change to Linux version(reset via usb1.0 udisk)
 * 11. iso
 * 12. WARN_ON is assert, memset
 * 13. usb_hcd_unlink_urb_from_ep(urb->hcpriv will all lock)(urb->hport == NULL)
 *
 * */

#define NULL RT_NULL
#endif /* __DWC2_HW_H__ */

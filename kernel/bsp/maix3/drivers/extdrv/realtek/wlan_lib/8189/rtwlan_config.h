#include "rtthread.h"

#define CONFIG_PLATFOMR_CUSTOMER_RTOS
#define CONFIG_HARDWARE_8188F              1
#define DRV_NAME "RTL8189FTV"
#define DRIVERVERSION "8e1b53f30ff1168d10b2557f640f7cabeff0e6b7"

/* Index 0 is 100% of the calibrated per-rate transmit power. */
#define CONFIG_TX_POWER_PERCENTAGE_INDEX   0

#define CONFIG_DEBUG                    0
#define WLAN_INTF_DBG                       0

#if defined (REALTEK_ENABLE_DEBUG)
    #undef CONFIG_DEBUG
    #define CONFIG_DEBUG                    1
#endif

#define CONFIG_MP_INCLUDED                  1
#define CONFIG_MP_NORMAL_IWPRIV_SUPPORT     1

#define CONFIG_AP_MODE                      1
#define CONFIG_CONCURRENT_MODE              1

#define CONFIG_WLAN                         1

#define CONFIG_NO_REFERENCE_FOR_COMPILER    1

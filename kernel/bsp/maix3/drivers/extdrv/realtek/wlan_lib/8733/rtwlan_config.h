#include "rtthread.h"

#define CONFIG_PLATFOMR_CUSTOMER_RTOS
#define DRV_NAME "RTL8733BS_v1.2"
#define DRIVERVERSION "65278d88f6e5337d46c41f92a92e31afbc1dcd01"

#define CONFIG_DEBUG                    0
#define WLAN_INTF_DBG                       0

#if defined (REALTEK_ENABLE_DEBUG)
    #undef CONFIG_DEBUG
    #define CONFIG_DEBUG                    1
#endif

#define CONFIG_FA_DBG                       0
#define WLAN_INTF_DBG                       0

#define CONFIG_MP_INCLUDED                  1
#define CONFIG_MP_NORMAL_IWPRIV_SUPPORT     1

#define CONFIG_AP_MODE                      1
#define CONFIG_CONCURRENT_MODE              1

#define CONFIG_WLAN                         1

#define CONFIG_BT_COEXIST                   1

#define CONFIG_PROMISC                      1

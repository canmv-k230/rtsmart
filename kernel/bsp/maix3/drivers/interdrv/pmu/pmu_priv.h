#ifndef __PMU_PRIV_H__
#define __PMU_PRIV_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include <rtdevice.h>
#include <rthw.h>
#include <rtthread.h>

#include "drv_pmu.h"

#ifndef _IOW
#define _IOC(a, b, c, d) (((a) << 30) | ((b) << 8) | (c) | ((d) << 16))
#define _IOC_NONE  0U
#define _IOC_WRITE 1U
#define _IOC_READ  2U
#define _IO(a, b) _IOC(_IOC_NONE, (a), (b), 0)
#define _IOW(a, b, c) _IOC(_IOC_WRITE, (a), (b), sizeof(c))
#define _IOR(a, b, c) _IOC(_IOC_READ, (a), (b), sizeof(c))
#define _IOWR(a, b, c) _IOC(_IOC_READ | _IOC_WRITE, (a), (b), sizeof(c))
#endif

#ifndef BIT
#define BIT(n) (1U << (n))
#endif

#define K230_PMU_IRQN                           175U

#define PMU_STATUS                              0x3c
#define PMU_INT0_TO_CTL_REGISTER                0x40
#define PMU_INT1_TO_CTL_REGISTER                0x44
#define PMU_INT0_TO_CPU_REGISTER                0x48
#define PMU_INT_DETECT_EN                       0x4c
#define PMU_INT_DETECT_TYP                      0x50
#define PMU_INT_DETECT_CLR                      0x54
#define PMU_INT0_LONG_PRESS_TRIGGER_VAL         0x58
#define PMU_INT0_LEVEL_DEBOUNCE_VAL             0x64
#define PMU_SYSCTRL_REG                         0x78
#define PMU_OUT_EVENT_CTRL                      0xa4
#define PMU_OUT_LOGIC_CTRL                      0xa8
#define PMU_INT_STATE_REG                       0xac
#define PMU_PWR_ISO_CTRL_REG                    0x158

#define PMU_SYSCTRL_TO_CTRL_0_REG_O             BIT(0)
#define PMU_SYSCTRL_TO_CTRL_1_REG_O             BIT(1)
#define PMU_OUT_EVENT_TO_CTRL_0_INT_LOGIC       BIT(2)
#define PMU_OUT_EVENT_TO_CTRL_0_INT8            BIT(3)
#define PMU_OUT_EVENT_TO_CTRL_0_SW_REG          BIT(4)
#define PMU_OUT_LOGIC_TO_CTRL_1_INT_LOGIC       BIT(1)
#define PMU_OUT_LOGIC_TO_CTRL_1_INT8            BIT(2)
#define PMU_OUT_LOGIC_TO_CTRL_1_LOGIC_AND       BIT(0)
#define PMU_OUT_LOGIC_TO_CTRL_1_SW_REG          BIT(3)
#define PMU_ISO_ACCESS_MASK                     BIT(5)

#define PMU_CPU_IRQ_MASK                        0x0fffU
#define PMU_DET_SOURCE_MASK                     0x1fffU

#define PMU_IRQ_KEY_LONG                        BIT(11)
#define PMU_IRQ_KEY_SHORT                       BIT(10)
#define PMU_IRQ_KEY_EDGE                        BIT(9)
#define PMU_IRQ_INT1_0                          BIT(8)
#define PMU_IRQ_INT1_1                          BIT(7)
#define PMU_IRQ_INT2                            BIT(6)
#define PMU_IRQ_INT3                            BIT(5)
#define PMU_IRQ_INT4                            BIT(4)
#define PMU_IRQ_INT5                            BIT(3)
#define PMU_IRQ_RTC_ALARM                       BIT(2)
#define PMU_IRQ_RTC_TICK                        BIT(1)
#define PMU_IRQ_KEY_SHUTDOWN                    BIT(0)
#define PMU_IRQ_RTC_MASK                        \
    (PMU_IRQ_RTC_ALARM | PMU_IRQ_RTC_TICK)

#define PMU_DET_KEY_SHUTDOWN                    BIT(12)
#define PMU_DET_KEY_LONG                        BIT(11)
#define PMU_DET_KEY_SHORT                       BIT(10)
#define PMU_DET_KEY_EDGE                        BIT(9)
#define PMU_DET_RTC_ALARM                       BIT(2)
#define PMU_DET_RTC_TICK                        BIT(1)

#define PMU_CLR_KEY_LONG                        0x0200U
#define PMU_CLR_KEY_SHUTDOWN                    0x0100U
#define PMU_CLR_KEY_SHORT                       0x0080U
#define PMU_CLR_KEY_EDGE                        0x0040U
#define PMU_CLR_INT1_0                          0x0020U
#define PMU_CLR_INT1_1                          0x0010U
#define PMU_CLR_INT2                            0x0008U
#define PMU_CLR_INT3                            0x0004U
#define PMU_CLR_INT4                            0x0002U
#define PMU_CLR_INT5                            0x0001U
#define PMU_CLR_ALL                             0x03ffU

#define PMU_INT_TRIGGER_MASK                    0x7U
#define PMU_INT_TRIGGER_TYPE_MASK               0x1U
#define PMU_INT_TRIGGER_EDGE_MASK               0x2U
#define PMU_KEY_EDGE_OFFSET                     16U

#define PMU_INT_STATE_RTC_ALARM_INPUT_MASK      BIT(13)
#define PMU_RTC_TICK_EN_BIT                     BIT(8)

#define PMU_PWRKEY_LONG_PRESS_TICKS             96000U
#define PMU_PWRKEY_DEBOUNCE_TICKS               256U
#if defined(RT_PMU_SOFT_SHUTDOWN_SECONDS)
#define PMU_SOFT_SHUTDOWN_MS                    \
    ((RT_PMU_SOFT_SHUTDOWN_SECONDS) * 1000U)
#else
#define PMU_SOFT_SHUTDOWN_MS                    5000U
#endif
#define PMU_WORKER_STACK_SIZE                   2048U
#define PMU_WORKER_PRIORITY                     12U
#define PMU_WORKER_TIMESLICE                    10U
#define PMU_STATUS_NORMAL_PD                    2U
#define PMU_POWER_CYCLE_MIN_DELAY_S             2U

#define PMU_USERDEV_NAME                        "pmu_pwrkey"
#define PMU_NOTIFY_DEFAULT_SIGNO                10

#define PMU_EVENT_LONG_PRESS                    0x00000001U
#define PMU_EVENT_KEY_RELEASE                   0x00000002U

#define PMU_WORK_KEY_LONG                       BIT(0)
#define PMU_WORK_KEY_RELEASE                    BIT(1)
#define PMU_WORK_CYCLE_SHUTDOWN                 BIT(2)

#define PMU_RTC_ALARM_FLAG_MASK                 \
    (RTC_INT_ALARM_YEAR | RTC_INT_ALARM_MONTH | RTC_INT_ALARM_DAY | \
     RTC_INT_ALARM_WEEK | RTC_INT_ALARM_HOUR | RTC_INT_ALARM_MINUTE | \
     RTC_INT_ALARM_SECOND)
#define PMU_RTC_ALARM_ALL_FLAGS                 \
    (RTC_INT_ALARM_YEAR | RTC_INT_ALARM_MONTH | RTC_INT_ALARM_DAY | \
     RTC_INT_ALARM_HOUR | RTC_INT_ALARM_MINUTE | RTC_INT_ALARM_SECOND)

typedef struct {
    uint32_t timer_w_en : 1;
    uint32_t timer_r_en : 1;
    uint32_t reserved0 : 6;
    uint32_t tick_en : 1;
    uint32_t tick_sel : 4;
    uint32_t reserved1 : 3;
    uint32_t alarm_en : 1;
    uint32_t alarm_clr : 1;
    uint32_t reserved2 : 6;
    uint32_t second_cmp : 1;
    uint32_t minute_cmp : 1;
    uint32_t hour_cmp : 1;
    uint32_t week_cmp : 1;
    uint32_t day_cmp : 1;
    uint32_t month_cmp : 1;
    uint32_t year_cmp : 1;
    uint32_t reserved3 : 1;
} __attribute__((packed, aligned(4))) k230_rtc_int_ctrl_t;

typedef struct {
    uint32_t curr_count : 15;
    uint32_t reserved0 : 1;
    uint32_t sum_count : 15;
    uint32_t reserved1 : 1;
} __attribute__((packed, aligned(4))) k230_rtc_count_t;

typedef struct {
    uint32_t alarm_second : 6;
    uint32_t reserved0 : 2;
    uint32_t alarm_minute : 6;
    uint32_t reserved1 : 2;
    uint32_t alarm_hour : 5;
    uint32_t reserved2 : 3;
    uint32_t alarm_week : 3;
    uint32_t reserved3 : 5;
} __attribute__((packed, aligned(4))) k230_rtc_alarm_time_t;

typedef struct {
    uint32_t alarm_day : 5;
    uint32_t reserved0 : 3;
    uint32_t alarm_month : 4;
    uint32_t reserved1 : 4;
    uint32_t alarm_year_l : 7;
    uint32_t reserved2 : 1;
    uint32_t alarm_year_h : 7;
    uint32_t reserved3 : 1;
} __attribute__((packed, aligned(4))) k230_rtc_alarm_date_t;

typedef struct {
    uint32_t second : 6;
    uint32_t reserved0 : 2;
    uint32_t minute : 6;
    uint32_t reserved1 : 2;
    uint32_t hour : 5;
    uint32_t reserved2 : 3;
    uint32_t week : 3;
    uint32_t reserved3 : 5;
} __attribute__((packed, aligned(4))) k230_rtc_time_t;

typedef struct {
    uint32_t day : 5;
    uint32_t reserved0 : 3;
    uint32_t month : 4;
    uint32_t reserved1 : 4;
    uint32_t year_l : 7;
    uint32_t leap_year : 1;
    uint32_t year_h : 7;
    uint32_t reserved2 : 1;
} __attribute__((packed, aligned(4))) k230_rtc_date_t;

typedef struct {
    k230_rtc_date_t date;
    k230_rtc_time_t time;
    k230_rtc_alarm_date_t alarm_date;
    k230_rtc_alarm_time_t alarm_time;
    k230_rtc_count_t count;
    k230_rtc_int_ctrl_t int_ctrl;
} __attribute__((packed, aligned(4))) k230_rtc_regs_t;

struct pmu_notify_cfg {
    int32_t pid;
    int32_t signo;
};

struct pmu_event {
    uint32_t events;
    uint32_t reserved;
};

struct pmu_power_cycle_cfg {
    uint32_t shutdown_after_s;
    uint32_t poweron_after_s;
    uint32_t flags;
    uint32_t reserved;
};

#define PMU_IOCTL_REGISTER_NOTIFY \
    _IOW('P', 0x00, struct pmu_notify_cfg)
#define PMU_IOCTL_UNREGISTER_NOTIFY \
    _IOW('P', 0x01, struct pmu_notify_cfg)
#define PMU_IOCTL_GET_EVENT \
    _IOR('P', 0x02, struct pmu_event)
#define PMU_IOCTL_SHUTDOWN_ACK \
    _IOW('P', 0x03, struct pmu_event)
#define PMU_IOCTL_SCHEDULE_POWER_CYCLE \
    _IOW('P', 0x04, struct pmu_power_cycle_cfg)
#define PMU_IOCTL_CANCEL_POWER_CYCLE \
    _IO('P', 0x05)

struct pmu_worker_state {
    rt_thread_t thread;
    struct rt_event event;
    bool initialized;
};

struct pmu_rtc_state {
    struct rt_device device;
    volatile rt_uint8_t *base;
    void (*callback)(void);
    bool device_registered;
    bool initialized;
};

struct pmu_notify_state {
    struct rt_device device;
    rt_int32_t pid;
    rt_int32_t signo;
    rt_uint32_t pending_events;
    bool registered;
    bool ack_pending;
    bool initialized;
};

struct pmu_pwrkey_state {
    rt_timer_t timer;
    bool pressed;
    bool long_press_seen;
    bool release_seen;
    bool user_notified;
    bool initialized;
};

struct pmu_cycle_state {
    bool active;
    bool shutting_down;
    bool saved_routes_valid;
    uint32_t saved_cpu_route;
    uint32_t saved_out0_route;
    uint32_t saved_out1_route;
    uint32_t poweron_after_s;
    time_t shutdown_time;
    time_t poweron_time;
};

struct pmu_dev {
    volatile rt_uint8_t *base;
    volatile rt_uint8_t *pwr_base;
    const char *shutdown_source;
    bool irq_installed;
    bool initialized;
    struct pmu_worker_state worker;
    struct pmu_rtc_state rtc;
    struct pmu_notify_state notify;
    struct pmu_pwrkey_state pwrkey;
    struct pmu_cycle_state cycle;
};

extern struct pmu_dev g_pmu_dev;

struct pmu_dev *pmu_get_dev(void);
int pmu_init(struct pmu_dev *pmu);
int pmu_ensure_access(struct pmu_dev *pmu);
int pmu_current_pid(void);
void pmu_do_shutdown(const char *source);

volatile rt_uint8_t *pmu_get_base(struct pmu_dev *pmu);
volatile rt_uint8_t *pmu_get_pwr_base(struct pmu_dev *pmu);
volatile rt_uint8_t *pmu_get_rtc_base(struct pmu_dev *pmu);
uint32_t pmu_readl(struct pmu_dev *pmu, uint32_t reg);
void pmu_writel(struct pmu_dev *pmu, uint32_t value, uint32_t reg);
void pmu_update_bits_locked(struct pmu_dev *pmu, uint32_t reg, uint32_t mask,
                bool enable);
void pmu_set_detect_en_locked(struct pmu_dev *pmu, uint32_t mask, bool enable);

int pmu_init_worker(struct pmu_dev *pmu);
void pmu_post_work(struct pmu_dev *pmu, uint32_t work);

int pmu_init_userdev(struct pmu_dev *pmu);
void pmu_notify_unregister_pid(struct pmu_dev *pmu, rt_int32_t pid);
rt_err_t pmu_notify_send(struct pmu_dev *pmu, rt_uint32_t events,
             bool wait_ack);

int pmu_init_pwrkey(struct pmu_dev *pmu);
void pmu_pwrkey_irq(struct pmu_dev *pmu, uint32_t status);
void pmu_pwrkey_handle_long_work(struct pmu_dev *pmu);
void pmu_pwrkey_handle_release_work(struct pmu_dev *pmu);

int pmu_init_rtc(struct pmu_dev *pmu);
void pmu_rtc_handle_irq(struct pmu_dev *pmu, uint32_t status);
bool pmu_cycle_owns_alarm(struct pmu_dev *pmu);
rt_err_t pmu_schedule_power_cycle(
    struct pmu_dev *pmu,
    const struct pmu_power_cycle_cfg *cfg);
rt_err_t pmu_cancel_power_cycle(struct pmu_dev *pmu);
void pmu_cycle_handle_shutdown_work(struct pmu_dev *pmu);
int pmu_cycle_prepare_wakeup_alarm(struct pmu_dev *pmu);

#endif

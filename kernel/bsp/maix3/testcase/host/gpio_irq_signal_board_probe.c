/* Test-only notification gate. Link this with the GPIO driver and call
 * gpio_irq_signal_probe_before_notify() immediately before lwp_kill_ext().
 * The normal firmware must not include this gate. */
#include <rtthread.h>
#include <lwp.h>
#include <lwp_pid.h>
#include "drv_gpio.h"

#define SIGNAL_COOKIE 0x47504900UL
#define SIGNAL_PIN 26

static volatile int peer_entered, peer_done;

static void signal_peer_detach(void *arg)
{
    (void)arg;
    peer_entered = 1;
    RT_ASSERT(kd_pin_detach_irq(SIGNAL_PIN) == RT_EOK);
    peer_done = 1;
    rt_kprintf("GPIO_SIGNAL peer-detach complete\n");
}

void gpio_irq_signal_probe_before_notify(int pid, void *sigval)
{
    unsigned long test = (unsigned long)sigval - SIGNAL_COOKIE;
    if (test < 1 || test > 3)
        return;

    struct rt_lwp *lwp = lwp_from_pid(pid);
    RT_ASSERT(lwp != RT_NULL);
    rt_thread_t target = rt_list_entry(lwp->t_grp.prev, struct rt_thread, sibling);
    RT_ASSERT(target->current_priority == 10);
    RT_ASSERT((target->stat & RT_THREAD_STAT_MASK) == RT_THREAD_SUSPEND_INTERRUPTIBLE);

    if (test == 3) {
        /* This thread owns teardown; the user will take the concurrent
         * detacher path. Both have higher priority than the signal worker. */
        peer_entered = peer_done = 0;
        rt_thread_t peer = rt_thread_create("gpio_peer", signal_peer_detach,
                                           RT_NULL, 4096, 11, 10);
        RT_ASSERT(peer != RT_NULL);
        RT_ASSERT(rt_thread_startup(peer) == RT_EOK);
        rt_schedule();
        RT_ASSERT(peer_entered && !peer_done);
    }

    rt_kprintf("GPIO_SIGNAL gate case=%lu\n", test);
    /* Wake the user's usleep without posting a signal. It now calls detach
     * or close and blocks on this in-flight notification. */
    target->error = -RT_EINTR;
    RT_ASSERT(rt_thread_wakeup(target) == RT_EOK);
    rt_schedule();

    /* The old driver sleeps interruptibly here; lwp_kill_ext() then wakes
     * it with a pending signal and it spins forever above our priority. */
    rt_kprintf("GPIO_SIGNAL teardown-wait case=%lu state=%u\n",
               test, target->stat & RT_THREAD_STAT_MASK);
    RT_ASSERT((target->stat & RT_THREAD_STAT_MASK) == RT_THREAD_SUSPEND_UNINTERRUPTIBLE);
}

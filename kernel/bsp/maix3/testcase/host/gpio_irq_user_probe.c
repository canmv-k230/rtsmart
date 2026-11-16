#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

typedef struct { uint16_t pin, value; } gpio_cfg_t;
typedef struct { uint16_t pin, mode, debounce_ms, signo; void *sigval; } gpio_irqcfg_t;
#define GPIO_SET_MODE _IOW('G', 0, gpio_cfg_t*)
#define GPIO_GET_MODE _IOR('G', 1, gpio_cfg_t*)
#define GPIO_SET_IRQ _IOW('G', 2, gpio_irqcfg_t*)
#define GPIO_CTRL_IRQ _IOWR('G', 4, gpio_cfg_t*)
static volatile sig_atomic_t signals, late, phase;
static int failures, checks;
static void handler(int sig, siginfo_t *info, void *ctx)
{
    (void)sig; (void)info; (void)ctx;
    signals++;
    if (phase) late++;
}
static void check(int ok, const char *name, int value)
{
    checks++;
    printf("GPIO_USER %s %s value=%d\n",ok?"PASS":"FAIL",name,value);
    if (!ok) failures++;
}
static int mode(int fd, int high)
{
    gpio_cfg_t c={26, high?2:3};
    return ioctl(fd,GPIO_SET_MODE,&c);
}
static int control(int fd, int value)
{
    gpio_cfg_t c={26,value};
    return ioctl(fd,GPIO_CTRL_IRQ,&c);
}
static int attach(int fd, int irq_mode, int debounce)
{
    gpio_irqcfg_t c={26,irq_mode,debounce,SIGUSR1,(void*)0x12345678};
    return ioctl(fd,GPIO_SET_IRQ,&c);
}
static int priority(int prio)
{
    struct sched_param p={.sched_priority=prio};
    return sched_setparam(0,&p);
}
static int signal_wait_cases(void)
{
    int fd = open("/dev/gpio", O_RDWR);
    gpio_cfg_t saved = {26, 0};
    check(fd >= 0, "signal-wait-open", fd);
    if (fd < 0) return 2;
    int saved_ok = ioctl(fd, GPIO_GET_MODE, &saved) == 0;
    check(saved_ok, "signal-wait-save-mode", saved.value);
    check(priority(10) == 0, "signal-wait-priority", errno);

    for (int test = 1; test <= 3; test++) {
        gpio_irqcfg_t cfg = {26, 0, 0, SIGUSR1, (void *)(uintptr_t)(0x47504900UL + test)};
        phase = signals = late = 0;
        check(mode(fd, 0) == 0, "signal-wait-pulldown", test);
        check(ioctl(fd, GPIO_SET_IRQ, &cfg) == 0, "signal-wait-attach", test);
        check(control(fd, 1) == 0, "signal-wait-enable", test);
        int edge_rc = mode(fd, 1);
        /* The test-only gate wakes this sleep after claiming notification. */
        usleep(1000000);
        int in_flight = signals == 0;
        int rc = test == 2 ? close(fd) : control(fd, 128);
        check(edge_rc == 0, "signal-wait-edge", test);
        check(in_flight, "signal-wait-notification-in-flight", test);
        check(rc == 0, test == 2 ? "signal-wait-close" : "signal-wait-detach", test);
        check(signals == 1, "signal-wait-delivered", signals);
        printf("GPIO_USER signal-wait case=%d complete signals=%d\n", test, signals);
        if (test == 2) {
            fd = open("/dev/gpio", O_RDWR);
            check(fd >= 0, "signal-wait-reopen", fd);
        }
        usleep(10000);
    }
    priority(23);
    if (saved_ok) ioctl(fd, GPIO_SET_MODE, &saved);
    close(fd);
    printf("GPIO_USER SIGNAL_END checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    struct sigaction sa;
    memset(&sa,0,sizeof(sa));
    sa.sa_sigaction=handler; sa.sa_flags=SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    check(sigaction(SIGUSR1,&sa,0)==0,"sigaction",errno);
    if (argc == 2 && strcmp(argv[1], "--signal-wait") == 0)
        return signal_wait_cases();
    int fd=open("/dev/gpio",O_RDWR);
    check(fd>=0,"open",fd);
    if(fd<0) return 2;
    gpio_cfg_t saved={26,0};
    int saved_ok=ioctl(fd,GPIO_GET_MODE,&saved)==0;
    check(saved_ok,"save-mode",saved.value);
    mode(fd,0);
    check(attach(fd,0,0)==0,"attach-normal",errno);
    check(control(fd,1)==0,"enable-normal",errno);
    phase=signals=late=0;
    mode(fd,1);
    usleep(50000);
    check(signals==1,"normal-notification",signals);
    check(control(fd,128)==0,"detach-normal",errno);
    usleep(50000);

    int stress_failures=0;
    for(int i=0;i<1000;i++) {
        phase=signals=late=0;
        if(mode(fd,0) || attach(fd,0,0) || control(fd,1)) stress_failures++;
        mode(fd,1);
        usleep(1000);
        if(signals!=1) stress_failures++;
        if(control(fd,128)) stress_failures++;
        phase=1;
        mode(fd,0); mode(fd,1);
        usleep(1000);
        if(signals!=1) stress_failures++;
    }
    check(stress_failures==0,"1000-attach-interrupt-detach-cycles",stress_failures);
    for(int close_case=0;close_case<2;close_case++) {
        mode(fd,0);
        check(attach(fd,0,0)==0,"attach-late-check",errno);
        check(control(fd,1)==0,"enable-late-check",errno);
        int rc=priority(10);
        check(rc==0,"raise-user-priority",rc);
        if(rc) { control(fd,128); continue; }
        phase=signals=late=0;
        mode(fd,1);
        rc=close_case?close(fd):control(fd,128);
        phase=1;
        priority(23);
        usleep(100000);
        check(rc==0,close_case?"close":"detach",rc);
        check(late==0,close_case?"no-signal-after-close":"no-signal-after-detach",late);
        printf("GPIO_USER notification-total=%d after-%s=%d\n",signals,close_case?"close":"detach",late);
        if(close_case) fd=open("/dev/gpio",O_RDWR);
        usleep(100000);
    }
    phase=0;
    mode(fd,0);
    check(attach(fd,3,50)==0,"level-attach",errno);
    check(control(fd,1)==0,"level-enable",errno);
    mode(fd,1);
    check(control(fd,128)==0,"detach-with-pending-debounce",errno);
    int reuse=attach(fd,0,0);
    check(reuse==0,"immediate-reuse-after-debounce-cancel",reuse);
    int before=signals;
    mode(fd,0); mode(fd,1);
    usleep(100000);
    check(signals==before,"canceled-debounce-keeps-replacement-disabled",signals-before);
    control(fd,128);
    if(saved_ok) ioctl(fd,GPIO_SET_MODE,&saved);
    close(fd);
    printf("GPIO_USER END checks=%d failures=%d\n",checks,failures);
    return failures?1:0;
}

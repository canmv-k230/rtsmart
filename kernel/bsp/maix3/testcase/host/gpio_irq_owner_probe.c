/* GPIO26 must be free of attached peripherals. Uses internal pulls, no wires. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include "hal_syscall.h"

typedef struct { uint16_t pin, value; } gpio_cfg_t;
typedef struct { uint16_t pin, mode, debounce_ms, signo; void *sigval; } gpio_irqcfg_t;
#define GPIO_SET_MODE _IOW('G', 0, gpio_cfg_t *)
#define GPIO_GET_MODE _IOR('G', 1, gpio_cfg_t *)
#define GPIO_SET_IRQ _IOW('G', 2, gpio_irqcfg_t *)
#define GPIO_CTRL_IRQ _IOWR('G', 4, gpio_cfg_t *)
#define OWNER_PIN 26
#define ITERATIONS 20

static volatile sig_atomic_t signals;
static unsigned checks, failures;

static void handler(int sig) { (void)sig; signals++; }
static void check(int ok, const char *name)
{
    checks++;
    if (!ok) {
        failures++;
        printf("GPIO_OWNER FAIL %s errno=%d\n", name, errno);
    }
}
static int control(int fd, int value)
{
    gpio_cfg_t cfg = {OWNER_PIN, value};
    return ioctl(fd, GPIO_CTRL_IRQ, &cfg);
}
static int mode(int fd, int high)
{
    gpio_cfg_t cfg = {OWNER_PIN, high ? 2 : 3};
    int rc = ioctl(fd, GPIO_SET_MODE, &cfg);
    usleep(1000);
    return rc;
}
static int attach(int fd)
{
    gpio_irqcfg_t cfg = {OWNER_PIN, 0, 0, SIGUSR1, (void *)0x47504f57};
    return ioctl(fd, GPIO_SET_IRQ, &cfg);
}
static int await_signal(int count)
{
    for (int i = 0; i < 1000 && signals < count; i++) usleep(1000);
    return signals == count;
}
static pid_t reap(pid_t pid, int *status)
{
    long rc;
    do {
        rc = syscall(_NRSYS_waitpid, (long)pid, (long)status, 0L);
    } while ((rc == -1 && errno == EINTR) || rc == -EINTR);
    return (pid_t)rc;
}
static int attacker(pid_t owner)
{
    pid_t caller = (pid_t)syscall(_NRSYS_getpid);
    int fd = open("/dev/gpio", O_RDWR);
    int failed = fd < 0 || caller <= 0 || caller == owner;
    if (fd >= 0) {
        if (control(fd, 128) != 0) failed = 1;
        /* A foreign registration must still prevent attaching a replacement. */
        if (attach(fd) == 0) {
            failed = 1;
            control(fd, 128);
        }
        if (close(fd) != 0) failed = 1;
    }
    printf("GPIO_OWNER attacker owner=%d caller=%d failures=%d\n", (int)owner, (int)caller, failed);
    fflush(stdout);
    return failed;
}

int main(int argc, char **argv)
{
    struct sigaction sa = {0};
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    check(sigaction(SIGUSR1, &sa, NULL) == 0, "sigaction");
    if (argc == 2 && !strcmp(argv[1], "--kernel")) {
        /* Run between gpio_irq_owner_kernel start and check/stop. */
        check(attacker(0) == 0, "kernel-registration-preserved");
        int fd = open("/dev/gpio", O_RDWR);
        check(fd >= 0, "kernel-trigger-open");
        if (fd >= 0) {
            check(mode(fd, 0) == 0 && mode(fd, 1) == 0, "kernel-trigger-edge");
            check(close(fd) == 0, "kernel-trigger-close");
        }
        printf("GPIO_OWNER KERNEL_END checks=%u failures=%u\n", checks, failures);
        return failures ? 1 : 0;
    }

    int fd = open("/dev/gpio", O_RDWR);
    gpio_cfg_t saved = {OWNER_PIN, 0};
    check(fd >= 0, "open");
    if (fd < 0) return 2;
    int saved_ok = ioctl(fd, GPIO_GET_MODE, &saved) == 0;
    check(saved_ok, "save-mode");
    pid_t owner = (pid_t)syscall(_NRSYS_getpid);
    unsigned completed = 0;
    for (int i = 0; i < ITERATIONS; i++) {
        unsigned before = failures;
        signals = 0;
        check(mode(fd, 0) == 0, "pulldown");
        int attached = attach(fd) == 0;
        check(attached, "owner-attach");
        if (!attached) break;
        check(control(fd, 1) == 0, "owner-enable");
        check(mode(fd, 1) == 0 && await_signal(1), "baseline-irq");
        check(mode(fd, 0) == 0, "prepare-second-edge");
        fflush(NULL);
        pid_t child = (pid_t)syscall(_NRSYS_fork);
        if (child == 0) _Exit(attacker(owner));
        check(child > 0, "fork-attacker");
        if (child > 0) {
            int status = 0;
            check(reap(child, &status) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                  "attacker-exit");
        }
        check(mode(fd, 1) == 0 && await_signal(2), "irq-after-foreign-detach-and-close");
        check(control(fd, 128) == 0, "owner-detach");
        check(mode(fd, 0) == 0 && mode(fd, 1) == 0, "edge-after-owner-detach");
        usleep(10000);
        check(signals == 2, "no-signal-after-owner-detach");
        completed++;
        printf("GPIO_OWNER iteration=%d %s\n", i + 1, failures == before ? "PASS" : "FAIL");
        if (failures != before) break;
    }
    if (saved_ok) check(ioctl(fd, GPIO_SET_MODE, &saved) == 0, "restore-mode");
    check(close(fd) == 0, "close");
    check(completed == ITERATIONS, "completed-all-iterations");
    printf("GPIO_OWNER END iterations=%u checks=%u failures=%u\n", completed, checks, failures);
    return failures ? 1 : 0;
}

#include <rthw.h>
#include <rtthread.h>
#include <tty.h>

#include "lwp.h"
//#include "lwp_tid.h"
#include "lwp_pid.h"

int setsid(void)
{
    int err = -EPERM;
    struct rt_lwp *current_lwp = (struct rt_lwp *)rt_thread_self()->lwp;

    if (current_lwp->__pgrp == current_lwp->pid)
    {
        return err;
    }

    tty_release_lwp(current_lwp);
    current_lwp->session = current_lwp->pid;
    current_lwp->__pgrp = current_lwp->pid;
    current_lwp->leader = 1;

    err = current_lwp->session;
    return err;
}

/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2021.12.07     linzhenxing      first version
 */
#include <dfs_file.h>
#include <dfs_fs.h>
#include <devfs.h>
#include <lwp.h>
#include <lwp_pid.h>
#include <rtdevice.h>
#include <rtthread.h>
#include <tty.h>
#include <tty_ldisc.h>

#if defined(RT_USING_POSIX)
#include <posix_termios.h>
#endif

#define DBG_TAG               "TTY"
#ifdef RT_TTY_DEBUG
#define DBG_LVL               DBG_LOG
#else
#define DBG_LVL               DBG_INFO
#endif /* RT_TTY_DEBUG */
#include <rtdbg.h>

struct termios tty_std_termios = {  /* for the benefit of tty drivers  */
    .c_iflag = IMAXBEL | IUCLC | INLCR | ICRNL | IGNPAR,
    .c_oflag = OPOST,
    .c_cflag = B38400 | CS8 | CREAD | HUPCL,
    .c_lflag = ISIG | ECHOE  | TOSTOP | NOFLSH,
    RT_NULL,/* .c_line = N_TTY, */
    .c_cc = INIT_C_CC,
    .__c_ispeed = 38400,
    .__c_ospeed = 38400
};

void tty_initstack(struct tty_node *node)
{
    node->lwp = RT_NULL;
    node->next = node;
}

static struct tty_node *_tty_node_alloc(void)
{
    struct tty_node *node = rt_calloc(1, sizeof(struct tty_node));

    return node;
}

static void _tty_node_free(struct tty_node *node)
{
    rt_free(node);
}

struct tty_node *tty_foreground_alloc(void)
{
    return _tty_node_alloc();
}

void tty_foreground_free(struct tty_node *node)
{
    if (node)
        _tty_node_free(node);
}

static void _tty_stack_free(struct tty_node *node)
{
    while (node)
    {
        struct tty_node *next = node->next;

        _tty_node_free(node);
        if (next == node)
            break;
        node = next;
    }
}

int tty_push(struct tty_node **head, struct rt_lwp *lwp)
{
    struct tty_node *node = _tty_node_alloc();

    if (!node)
    {
        return -1;
    }

    node->lwp = lwp;
    node->next = *head;
    *head = node;

    return 0;
}

struct rt_lwp *tty_pop(struct tty_node **head, struct rt_lwp *target_lwp)
{
    struct tty_node **link = head;

    if (!link)
        return RT_NULL;
    while (*link)
    {
        struct tty_node *node = *link;
        /* tty_initstack() supplies a permanent, self-linked sentinel. */
        if (node->next == node)
            break;
        if (!target_lwp || node->lwp == target_lwp)
        {
            struct rt_lwp *lwp = node->lwp;
            *link = node->next;
            _tty_node_free(node);
            return lwp;
        }
        link = &node->next;
    }
    return RT_NULL;
}

static void _tty_detach_session_locked(struct tty_struct *tty,
                                       struct tty_node **head,
                                       struct tty_node **peer_head)
{
    struct tty_struct *peer = tty->other_struct;
    struct lwp_avl_struct *pids = lwp_get_pid_ary();
    int i;

    for (i = 0; i < RT_LWP_MAX_NR; i++)
    {
        struct rt_lwp *lwp = pids[i].data;

        if (lwp && (lwp->tty == tty || (peer && lwp->tty == peer)))
        {
            lwp->tty = RT_NULL;
            lwp->tty_old_pgrp = 0;
        }
    }

    tty->session = -1;
    tty->pgrp = -1;
    tty->foreground = RT_NULL;
    *head = tty->head;
    tty->head = RT_NULL;
    *peer_head = RT_NULL;
    if (peer && peer != tty)
    {
        peer->session = -1;
        peer->pgrp = -1;
        peer->foreground = RT_NULL;
        *peer_head = peer->head;
        peer->head = RT_NULL;
    }
}

/* Detach every member before a PTY slot is recycled. This is also used when
 * the controlling session leader leaves while other files keep the pair open. */
void tty_detach_session(struct tty_struct *tty)
{
    struct tty_node *head = RT_NULL;
    struct tty_node *peer_head = RT_NULL;
    rt_base_t level;

    if (!tty || tty->type != TTY_DRIVER_TYPE_PTY)
        return;
    level = rt_hw_interrupt_disable();
    _tty_detach_session_locked(tty, &head, &peer_head);
    rt_hw_interrupt_enable(level);
    _tty_stack_free(head);
    _tty_stack_free(peer_head);
}

/* fork/exec put the previous foreground process on this stack. Leaving a
 * session or exiting out of order must remove only the departing process;
 * otherwise UART input can wake a freed process's wait queue. A session
 * member that started several foreground children is on the stack once per
 * child, so drop all of its entries before choosing the next foreground. */
void tty_release_lwp(struct rt_lwp *lwp)
{
    struct tty_struct *tty;
    struct tty_node *head = RT_NULL;
    struct tty_node *peer_head = RT_NULL;
    rt_base_t level = rt_hw_interrupt_disable();

    tty = lwp->tty;
    if (tty)
    {
        if (tty->type == TTY_DRIVER_TYPE_PTY && lwp->leader &&
            tty->session == lwp->session)
        {
            _tty_detach_session_locked(tty, &head, &peer_head);
            lwp->tty_old_pgrp = 0;
            rt_hw_interrupt_enable(level);
            _tty_stack_free(head);
            _tty_stack_free(peer_head);
            return;
        }
        rt_spin_lock(&tty->spinlock);
        while (tty_pop(&tty->head, lwp))
            ;
        if (tty->foreground == lwp)
        {
            tty->foreground = tty_pop(&tty->head, RT_NULL);
            /* The terminal's group follows its foreground: a stale group
             * made __tty_check_change() signal the restored foreground
             * for its own reads and writes. */
            tty->pgrp = tty->foreground ? tty->foreground->__pgrp : -1;
        }
        if (tty->type == TTY_DRIVER_TYPE_PTY && tty->other_struct &&
            tty->other_struct->foreground == lwp)
            tty->other_struct->foreground = tty->foreground;
        rt_spin_unlock(&tty->spinlock);
        lwp->tty = RT_NULL;
    }
    lwp->tty_old_pgrp = 0;
    rt_hw_interrupt_enable(level);
}

/* Whether @parent may hand @tty's foreground to a new child: it holds the
 * foreground, or handed it to an earlier child that is still running (it is
 * on the stack: a shell starting a pipeline). A background job's children
 * used to take the terminal, and ^C, from the foreground job. NULL @parent:
 * the console handoff from the kernel shell, always allowed. */
static int tty_may_take_foreground_locked(struct tty_struct *tty, struct rt_lwp *parent)
{
    struct tty_node *n;

    if (!parent || !tty->foreground || tty->foreground == parent)
        return 1;
    if (parent->session == tty->foreground->session &&
        parent->__pgrp == tty->foreground->__pgrp)
        return 1;
    for (n = tty->head; n && n->next != n; n = n->next)
    {
        if (n->lwp == parent)
            return 1;
    }
    return 0;
}

/* A session member starting a foreground child hands it the terminal. The
 * parent may have none (after setsid(), or after pty_detach_session() released
 * its PTY), so test and link under the interrupt lock that pair release uses.
 * If the stack node cannot be allocated, the child still shares the terminal
 * but does not take the foreground; nor does it when @parent may not hand
 * it on. */
/* The caller holds the interrupt lock used by PTY release. Return whether the
 * preallocated node was linked into the terminal stack. */
int tty_foreground_commit_locked(struct tty_struct *tty, struct rt_lwp *previous,
                                 struct rt_lwp *child, struct tty_node *node,
                                 struct rt_lwp *parent)
{
    child->tty = tty;
    if (!tty || !node)
        return 0;

    rt_spin_lock(&tty->spinlock);
    if (!tty_may_take_foreground_locked(tty, parent))
    {
        /* The child shares the terminal in the background. */
        rt_spin_unlock(&tty->spinlock);
        return 0;
    }
    /* A forked foreground member can launch a job on behalf of the
     * group. Return to its actual foreground owner, not to the alias that
     * happened to call exec (which may exit before the owner). */
    if (parent && tty->foreground && parent != tty->foreground &&
        parent->__pgrp == tty->foreground->__pgrp &&
        parent->session == tty->foreground->session)
        previous = tty->foreground;
    node->lwp = previous;
    node->next = tty->head;
    tty->head = node;
    tty->pgrp = child->__pgrp;
    tty->session = child->session;
    tty->foreground = child;
    rt_spin_unlock(&tty->spinlock);
    return 1;
}

int tty_push_foreground(struct rt_lwp *parent, struct rt_lwp *child)
{
    struct tty_node *node = tty_foreground_alloc();
    struct tty_struct *tty;
    rt_base_t level;
    int ret = 0;

    level = rt_hw_interrupt_disable();
    tty = parent ? parent->tty : RT_NULL;
    if (tty_foreground_commit_locked(tty, parent, child, node, parent))
        node = RT_NULL;
    else if (tty && !node)
        ret = -ENOMEM;
    rt_hw_interrupt_enable(level);
    tty_foreground_free(node);
    return ret;
}

/* Console foreground handoff has no parent terminal to re-check. Allocation
 * happens before the interrupt-disabled commit, so callers can unwind a new
 * process and thread without partially linking either one. */
int tty_push_foreground_tty(struct tty_struct *tty, struct rt_lwp *child)
{
    struct tty_node *node = tty_foreground_alloc();
    rt_base_t level;

    if (!node)
        return -ENOMEM;
    level = rt_hw_interrupt_disable();
    RT_ASSERT(tty_foreground_commit_locked(tty, tty->foreground, child, node, RT_NULL));
    rt_hw_interrupt_enable(level);
    return 0;
}

/* PTY readers and pollers share one endpoint queue, so input must wake all of
 * them. rt_wqueue_wakeup_all() leaves a resumed reader queued until it runs;
 * a second input burst before then would resume a runnable thread again and
 * fail ("Thread resume failed"). Run every callback, which records poll
 * readiness, but resume only threads that are still suspended. A reader that
 * is already runnable removes its own entry when it runs. */
void tty_wakeup_all(struct rt_wqueue *queue, void *key)
{
    struct rt_list_node *node;
    int need_schedule = 0;
    rt_base_t level = rt_hw_interrupt_disable();

    queue->flag = RT_WQ_FLAG_WAKEUP;
    for (node = queue->waiting_list.next; node != &queue->waiting_list; node = node->next)
    {
        struct rt_wqueue_node *entry = rt_list_entry(node, struct rt_wqueue_node, list);
        rt_thread_t thread = entry->polling_thread;

        if (entry->wakeup(entry, key) == 0 &&
            (thread->stat & RT_THREAD_SUSPEND_MASK) == RT_THREAD_SUSPEND_MASK &&
            rt_thread_resume(thread) == RT_EOK)
            need_schedule = 1;
    }
    rt_hw_interrupt_enable(level);
    if (need_schedule)
        rt_schedule();
}

rt_inline int tty_sigismember(lwp_sigset_t *set, int _sig)
{
    unsigned long sig = _sig - 1;

    if (_LWP_NSIG_WORDS == 1)
    {
        return 1 & (set->sig[0] >> sig);
    }
    else
    {
        return 1 & (set->sig[sig / _LWP_NSIG_BPW] >> (sig % _LWP_NSIG_BPW));
    }
}

static int is_ignored(int sig)
{
    return (tty_sigismember(&current->signal_mask, sig) ||
        current->signal_handler[sig-1] == SIG_IGN);
}

/**
 *  tty_check_change    -   check for POSIX terminal changes
 *  @tty: tty to check
 *
 *  If we try to write to, or set the state of, a terminal and we're
 *  not in the foreground, send a SIGTTOU.  If the signal is blocked or
 *  ignored, go ahead and perform the operation.  (POSIX 7.2)
 *
 *  Locking: ctrl_lock
 */

int __tty_check_change(struct tty_struct *tty, int sig)
{
    pid_t pgrp = 0, tty_pgrp = 0;
    int ret = 0;
    int level = 0;

    level = rt_hw_interrupt_disable();
    if (current == RT_NULL)
    {
        rt_hw_interrupt_enable(level);
        return 0;
    }

    if (current->tty != tty)
    {
        rt_hw_interrupt_enable(level);
        return 0;
    }

    pgrp = current->__pgrp;
    tty_pgrp = tty->pgrp;

    if (tty_pgrp && (pgrp != tty->pgrp))
    {
        if (is_ignored(sig))
        {
            if (sig == SIGTTIN)
            {
                ret = -EIO;
            }
        }
        else
        {
            struct lwp_avl_struct *pids = lwp_get_pid_ary();
            int i;
            /* Terminal job-control signals target the caller's background
             * group. Signalling tty->foreground kills the child that the
             * caller has just put in front of itself. */
            for (i = 0; i < RT_LWP_MAX_NR; i++)
            {
                struct rt_lwp *member = pids[i].data;
                if (member && !member->finish && member->tty == tty &&
                    member->session == current->session && member->__pgrp == pgrp)
                    lwp_kill(lwp_to_pid(member), sig);
            }
            ret = -EINTR;
        }
    }
    rt_hw_interrupt_enable(level);

    if (!tty_pgrp)
    {
        LOG_D(tty, "sig=%d, tty->pgrp == -1!\n", sig);
    }
    return ret;
}

int tty_check_change(struct tty_struct *tty)
{
    return __tty_check_change(tty, SIGTTOU);
}

/* ptmx is one shared pathname, but each successful open owns its own pair.
 * dup/fork share the dfs_fd, so its private endpoint follows the file lifetime. */
struct tty_struct *tty_from_fd(struct dfs_fd *fd)
{
    struct tty_struct *tty = (struct tty_struct *)fd->fnode->data;

    return tty->type == TTY_DRIVER_TYPE_PTY ? fd->data : tty;
}

static int tty_open(struct dfs_fd *fd)
{
    int ret = 0;
    int noctty = 0;
    struct tty_struct *tty = RT_NULL;
    struct tty_ldisc *ld = RT_NULL;

    tty = tty_from_fd(fd);
    RT_ASSERT(tty != RT_NULL);
    ld = tty->ldisc;
    /* Only the first open of the path sets up the line discipline (DFS runs
     * opens of a path one at a time, so ref_count is stable here). n_tty_open()
     * allocates fresh state; running it on every open replaced disc_data
     * under readers, dropping buffered input and leaking the old state. */
    if (ld->ops->open && fd->fnode->ref_count == 1)
    {
        ret = ld->ops->open(fd);
        /* The ldisc releases its own allocations on failure; do not let the
         * device open below overwrite the error and report success. */
        if (ret < 0)
            return ret;
    }
    noctty = (fd->flags & O_NOCTTY);

    rt_device_t device = (rt_device_t)fd->fnode->data;
    if (fd->fnode->ref_count == 1)
    {
        ret = devfs_device_open(device, fd->flags);
    }
    if (current == RT_NULL) //kernel mode not lwp
    {
        return ret;
    }

    if (!noctty &&
        current->leader &&
        !current->tty &&
        tty->session == -1)
    {
        current->tty = tty;
        current->tty_old_pgrp = 0;
        tty->session = current->session;
        tty->pgrp = current->__pgrp;
        tty->foreground = current;
    }

    return ret;
}

static int tty_close(struct dfs_fd *fd)
{
    int ret = 0;
    struct tty_struct *tty = RT_NULL;
    struct tty_ldisc *ld = RT_NULL;
    tty = tty_from_fd(fd);
    RT_ASSERT(tty != RT_NULL);
    ld = tty->ldisc;
    if (ld->ops->close)
    {
        //ld->ops->close(tty);
    }
    if (fd->fnode->ref_count == 1)
    {
        ret = rt_device_close((rt_device_t)tty);
    }
    return ret;
}

static int tiocsctty(struct tty_struct *tty, int arg)
{
    if (current->leader &&
        (current->session == tty->session) && current->tty == tty)
    {
        return 0;        
    }

    /*
     * The process must be a session leader and
     * not have a controlling tty already.
     */
    if (!current->leader || current->tty)
    {
        return -EPERM;        
    }

    if (tty->session > 0)
    {
        LOG_E("this tty have control process\n");

    }
    current->tty = tty;
    current->tty_old_pgrp = 0;
    tty->session = current->session;
    tty->pgrp = current->__pgrp;
    tty->foreground = current;
    if (tty->type == TTY_DRIVER_TYPE_PTY)
    {
        tty->other_struct->foreground = current;
    }
    return 0;
}

/* Whether the caller may query @real_tty's job-control state: through the
 * PTY master, from the kernel, or as a process whose controlling terminal it
 * is (Linux returns ENOTTY otherwise). */
static int tty_may_query(struct tty_struct *tty, struct tty_struct *real_tty)
{
    struct rt_lwp *self = current;

    if (tty->type == TTY_DRIVER_TYPE_PTY && tty->subtype == PTY_TYPE_MASTER)
        return 1;
    return self == RT_NULL || self->tty == real_tty;
}

/* Whether @lwp is a live member of @tty's session in group @pgrp. An exiting
 * process has released the terminal (tty_release_lwp() clears lwp->tty under
 * this lock) before finish is set and it can be reaped: a zombie made the
 * foreground would be signalled after it is freed. */
static int tty_pgrp_member_locked(struct rt_lwp *lwp, struct tty_struct *tty, pid_t pgrp)
{
    return lwp && !lwp->finish && lwp->tty == tty &&
           lwp->__pgrp == pgrp && lwp->session == tty->session;
}

/* A live process of @tty's session whose process group is @pgrp, preferring
 * the group leader. Caller holds the interrupt lock. */
static struct rt_lwp *tty_find_pgrp_locked(struct tty_struct *tty, pid_t pgrp)
{
    struct lwp_avl_struct *pids = lwp_get_pid_ary();
    struct rt_lwp *leader = lwp_from_pid(pgrp);
    int i;

    if (tty_pgrp_member_locked(leader, tty, pgrp))
        return leader;
    for (i = 0; i < RT_LWP_MAX_NR; i++)
    {
        struct rt_lwp *lwp = pids[i].data;

        if (tty_pgrp_member_locked(lwp, tty, pgrp))
            return lwp;
    }
    return RT_NULL;
}

/* tcsetpgrp(): signals for the terminal (tty->foreground) follow the group. */
static int tiocspgrp(struct tty_struct *real_tty, pid_t pgrp)
{
    struct rt_lwp *self = current;
    struct rt_lwp *target;
    rt_base_t level;

    if (pgrp < 0)
        return -EINVAL;
    if (self && (self->tty != real_tty || self->session != real_tty->session))
        return -ENOTTY;
    level = rt_hw_interrupt_disable();
    target = tty_find_pgrp_locked(real_tty, pgrp);
    if (!target)
    {
        rt_hw_interrupt_enable(level);
        return -EPERM;
    }
    rt_spin_lock(&real_tty->spinlock);
    real_tty->pgrp = pgrp;
    real_tty->foreground = target;
    if (real_tty->type == TTY_DRIVER_TYPE_PTY && real_tty->other_struct)
        real_tty->other_struct->foreground = target;
    rt_spin_unlock(&real_tty->spinlock);
    rt_hw_interrupt_enable(level);
    return 0;
}

/* The terminal ioctls musl's termios/unistd functions issue that do not need
 * the line discipline. -ENOIOCTLCMD for the rest. @args is checked for user
 * access by sys_ioctl(); a NULL pointer is still refused here. */
static int tty_generic_ioctl(struct dfs_fd *fd, struct tty_struct *tty,
                             struct tty_struct *real_tty, int cmd, void *args)
{
    struct winsize ws;
    rt_base_t level;
    pid_t pid = 0;
    int changed;

    switch (cmd)
    {
    case TIOCGWINSZ:                /* isatty(), tcgetwinsize() */
        if (!args)
            return -EFAULT;
        level = rt_hw_interrupt_disable();
        ws = real_tty->winsize;
        rt_hw_interrupt_enable(level);
        rt_memcpy(args, &ws, sizeof(ws));
        return 0;
    case TIOCSWINSZ:
        if (!args)
            return -EFAULT;
        rt_memcpy(&ws, args, sizeof(ws));
        level = rt_hw_interrupt_disable();
        changed = rt_memcmp(&real_tty->winsize, &ws, sizeof(ws)) != 0;
        real_tty->winsize = ws;
        if (changed && real_tty->foreground)
            pid = lwp_to_pid(real_tty->foreground);
        rt_hw_interrupt_enable(level);
        if (pid > 0)
            lwp_kill(pid, SIGWINCH);
        return 0;
    case TIOCGPGRP:                 /* tcgetpgrp() */
        if (!args)
            return -EFAULT;
        if (!tty_may_query(tty, real_tty))
            return -ENOTTY;
        *(int *)args = real_tty->pgrp > 0 ? real_tty->pgrp : 0;
        return 0;
    case TIOCSPGRP:                 /* tcsetpgrp() */
        if (!args)
            return -EFAULT;
        return tiocspgrp(real_tty, *(int *)args);
    case TIOCGSID:                  /* tcgetsid() */
        if (!args)
            return -EFAULT;
        if (!tty_may_query(tty, real_tty) || real_tty->session <= 0)
            return -ENOTTY;
        *(int *)args = real_tty->session;
        return 0;
    case TCSBRK:                    /* tcdrain(), tcsendbreak() */
        /* Output is written through synchronously (PTY output lands in
         * the peer's buffer); there is nothing to drain, and no line
         * break to send. */
        return 0;
    case TCXONC:                    /* tcflow() */
    {
        char c;

        switch ((int)(rt_ubase_t)args)
        {
        case TCIOFF:
            c = STOP_CHAR(real_tty);
            break;
        case TCION:
            c = START_CHAR(real_tty);
            break;
        case TCOOFF:
        case TCOON:
            /* Output suspension (and IXON ^S/^Q) is not implemented. */
            return -EOPNOTSUPP;
        default:
            return -EINVAL;
        }
        if (c != 0)                 /* _POSIX_VDISABLE */
            rt_device_write((rt_device_t)tty, -1, &c, 1);
        return 0;
    }
    case FIONBIO:
        if (!args)
            return -EFAULT;
        dfs_fd_lock();
        if (*(int *)args)
            fd->flags |= O_NONBLOCK;
        else
            fd->flags &= ~O_NONBLOCK;
        dfs_fd_unlock();
        return 0;
    default:
        return -ENOIOCTLCMD;
    }
}

static int tty_ioctl(struct dfs_fd *fd, int cmd, void *args)
{
    int ret = 0;
    struct tty_struct *tty = RT_NULL;
    struct tty_struct *real_tty = RT_NULL;
    struct tty_ldisc *ld = RT_NULL;

    tty = tty_from_fd(fd);
    RT_ASSERT(tty != RT_NULL);

    if (tty->type == TTY_DRIVER_TYPE_PTY && tty->subtype == PTY_TYPE_MASTER)
    {
        real_tty = tty->other_struct;
    }
    else
    {
        real_tty = tty;
    }
    switch (cmd)
    {
    case TIOCSCTTY:
        return tiocsctty(real_tty, 1);
    }
    ret = tty_generic_ioctl(fd, tty, real_tty, cmd, args);
    if (ret != -ENOIOCTLCMD)
        return ret;
    ret = -ENOTTY;
    ld = tty->ldisc;
    if (ld->ops->ioctl)
    {
        ret = ld->ops->ioctl(fd, cmd, args);
    }
    return ret;
}

static int tty_read(struct dfs_fd *fd, void *buf, size_t count)
{
    int ret = 0;
    struct tty_struct *tty = RT_NULL;
    struct tty_ldisc *ld = RT_NULL;

    tty = tty_from_fd(fd);
    RT_ASSERT(tty != RT_NULL);
    ld = tty->ldisc;
    if (ld->ops->read)
    {
        ret = ld->ops->read(fd, buf, count);
    }
    return ret;
}

static int tty_write(struct dfs_fd *fd, const void *buf, size_t count)
{
    int ret = 0;
    struct tty_struct *tty = RT_NULL;
    struct tty_ldisc *ld = RT_NULL;

    tty = tty_from_fd(fd);
    RT_ASSERT(tty != RT_NULL);
    ld = tty->ldisc;
    if (ld->ops->write)
    {
        ret = ld->ops->write(fd, buf, count);
    }
    return ret;
}

static int tty_poll(struct dfs_fd *fd, struct rt_pollreq *req)
{
    int ret = 0;
    struct tty_struct *tty = RT_NULL;
    struct tty_ldisc *ld = RT_NULL;

    tty = tty_from_fd(fd);
    RT_ASSERT(tty != RT_NULL);
    ld = tty->ldisc;
    if (ld->ops->poll)
    {
        ret = ld->ops->poll(fd, req);
    }
    return ret;
}

static const struct dfs_file_ops tty_fops =
{
    tty_open,
    tty_close,
    tty_ioctl,
    tty_read,
    tty_write,
    RT_NULL, /* flush */
    RT_NULL, /* lseek */
    RT_NULL, /* getdents */
    tty_poll,
};
static const struct dfs_file_ops console_fops =
{
    tty_open,
    tty_close,
    tty_ioctl,
    tty_read,
    tty_write,
    RT_NULL, /* flush */
    RT_NULL, /* lseek */
    RT_NULL, /* getdents */
    tty_poll,
};

void console_init()
{
    n_tty_init();
}

void tty_set_fops(struct dfs_file_ops *fops)
{
    *fops = tty_fops;
}

void console_set_fops(struct dfs_file_ops *fops)
{
    *fops = console_fops;
}

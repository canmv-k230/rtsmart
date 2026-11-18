/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <rtthread.h>
#include <dfs_file.h>
#include <tty.h>
#include <tty_ldisc.h>

#define PTY_PTS_SIZE 10
static struct tty_struct ptm_driver;
/* Device lookup can race a last close before entering our open callback.
 * Keep endpoint storage static and fops published with the name (see
 * pty_endpoint_init()); the lifecycle lock validates the slot before use. */
static struct pty_pair
{
    struct tty_struct master;
    struct tty_struct slave;
} pty_pairs[PTY_PTS_SIZE];
static struct rt_mutex ptmx_lock;
static struct dfs_file_ops pts_fops;
static struct dfs_file_ops ptmx_fops;

static rt_err_t pty_device_init(struct rt_device *dev)
{
    ((struct tty_struct *)dev)->init_flag = TTY_INIT_FLAG_INITED;
    return RT_EOK;
}

static rt_err_t pty_device_open(struct rt_device *dev, rt_uint16_t oflag)
{
    return RT_EOK;
}

static rt_err_t pty_device_close(struct rt_device *dev)
{
    return RT_EOK;
}

static rt_size_t pty_device_read(struct rt_device *dev, rt_off_t pos,
                               void *buffer, rt_size_t size)
{
    return 0;
}

static rt_size_t pty_device_write(struct rt_device *dev, rt_off_t pos,
                                const void *buffer, rt_size_t size)
{
    struct tty_struct *tty = (struct tty_struct *)dev;
    struct tty_struct *to = tty->other_struct;
    rt_base_t level = rt_hw_interrupt_disable();
    rt_size_t len;

    /* Both disciplines live until the last file on both ends is released.
     * DFS pins a file for every operation, including sleeping reads/polls. */
    if (tty->pty_hungup)
        len = 0;
    else
        len = to->ldisc->ops->receive_buf(to, (char *)buffer, size);
    rt_hw_interrupt_enable(level);
    return len;
}

ssize_t pty_write_exact(struct tty_struct *tty, const void *buffer, size_t size)
{
    struct tty_struct *to = tty->other_struct;
    rt_base_t level = rt_hw_interrupt_disable();
    ssize_t len;

    if (tty->pty_hungup)
        len = -EIO;
    else if (!to->ldisc->ops->receive_room ||
             to->ldisc->ops->receive_room(to) < (int)size)
        len = -EAGAIN;
    else
    {
        len = to->ldisc->ops->receive_buf(to, (char *)buffer, size);
        if (len != (ssize_t)size)
            len = -EAGAIN;
    }
    rt_hw_interrupt_enable(level);
    return len;
}

static rt_err_t pty_device_control(rt_device_t dev, int cmd, void *args)
{
    struct tty_struct *tty = (struct tty_struct *)dev;
    int ret = 0;

    if (cmd != (int)TIOCSPTLCK && cmd != (int)TIOCGPTLCK && cmd != (int)TIOCGPTN)
        return -ENOIOCTLCMD;
    if (!args)
        return -EINVAL;

    rt_mutex_take(&ptmx_lock, RT_WAITING_FOREVER);
    if (cmd == TIOCSPTLCK)
        tty->pts_lock = !!*(int *)args;
    else if (cmd == (int)TIOCGPTLCK)
        *(int *)args = tty->pts_lock;
    else
        *(int *)args = tty->index;
    rt_mutex_release(&ptmx_lock);
    return ret;
}

#ifdef RT_USING_DEVICE_OPS
static const struct rt_device_ops pty_device_ops =
{
    pty_device_init,
    pty_device_open,
    pty_device_close,
    pty_device_read,
    pty_device_write,
    pty_device_control,
};
#endif

static void pty_endpoint_release(struct tty_struct *tty)
{
    tty_detach_session(tty);
    tty_ldisc_kill(tty);
    if (tty->parent.ref_count)
        rt_device_close(&tty->parent);
    if (tty->init_flag >= TTY_INIT_FLAG_REGED)
        rt_device_unregister(&tty->parent);
    tty->other_struct = RT_NULL;
    tty->init_flag = TTY_INIT_FLAG_NONE;
}

static int pty_endpoint_init(struct tty_struct *tty, int index, int subtype)
{
    extern struct termios tty_std_termios;
    struct rt_device *device = &tty->parent;
    struct dfs_fnode node = {0};
    struct dfs_fd file = {0};
    char name[RT_NAME_MAX];
    int ret;

    /* devfs reads fops without ptmx_lock and takes the raw rt_device path
     * when it is NULL. rt_device_register() clears fops after publishing the
     * name, so publish the endpoint and its fops without a preemption point.
     * fops is never NULL outside this section, even if registration fails, so
     * a stale lookup of a recycled slot still reaches the PTY open callbacks,
     * which reject it. */
    rt_enter_critical();
    rt_memset(tty, 0, sizeof(*tty));
    device->fops = subtype == PTY_TYPE_MASTER ? &ptmx_fops : &pts_fops;
    tty->init_flag = TTY_INIT_FLAG_ALLOCED;
    tty->type = TTY_DRIVER_TYPE_PTY;
    tty->subtype = subtype;
    tty->index = index;
    tty->pts_lock = 1;
    tty->pgrp = tty->session = -1;
    rt_spin_lock_init(&tty->spinlock);
    rt_wqueue_init(&tty->wait_queue);
    rt_wqueue_init(&tty->write_wait_queue);
    tty->init_termios = tty_std_termios;
    tty->init_termios.c_cflag = B38400 | CS8 | CREAD;
    if (subtype == PTY_TYPE_MASTER)
    {
        tty->init_termios.c_iflag = 0;
        tty->init_termios.c_oflag = 0;
        tty->init_termios.c_lflag = 0;
    }
    else
        tty->init_termios.c_lflag |= ICANON;

    device->type = RT_Device_Class_Char;
#ifdef RT_USING_DEVICE_OPS
    device->ops = &pty_device_ops;
#else
    device->init = pty_device_init;
    device->open = pty_device_open;
    device->close = pty_device_close;
    device->read = pty_device_read;
    device->write = pty_device_write;
    device->control = pty_device_control;
#endif
    rt_snprintf(name, sizeof(name), subtype == PTY_TYPE_MASTER ? "ptm%d" : "pts%d", index);
    if (rt_device_register(device, name, RT_DEVICE_FLAG_RDWR) != RT_EOK)
    {
        rt_exit_critical();
        return -EIO;
    }
    tty->init_flag = TTY_INIT_FLAG_REGED;
    device->fops = subtype == PTY_TYPE_MASTER ? &ptmx_fops : &pts_fops;
    rt_exit_critical();

    ret = tty_ldisc_init(tty);
    if (ret < 0)
        return ret;
    node.data = tty;
    file.fnode = &node;
    file.data = tty;
    ret = tty->ldisc->ops->open(&file);
    if (ret < 0)
        return ret;

    /* One device reference per endpoint for the pair's lifetime, including
     * slave echo before its first open. File ownership is counted separately. */
    if (rt_device_open(device, RT_DEVICE_OFLAG_RDWR) != RT_EOK)
        return -EIO;
    return 0;
}

static int pty_file_close(struct dfs_fd *fd)
{
    struct tty_struct *tty = tty_from_fd(fd);
    struct tty_struct *peer = tty->other_struct;
    rt_base_t level;

    rt_mutex_take(&ptmx_lock, RT_WAITING_FOREVER);
    RT_ASSERT(tty->pty_open_count > 0);
    level = rt_hw_interrupt_disable();
    if (--tty->pty_open_count == 0)
    {
        peer->pty_hungup = 1;
        tty_wakeup_all(&peer->wait_queue, (void *)(POLLIN | POLLHUP));
        tty_wakeup_all(&peer->write_wait_queue, (void *)(POLLOUT | POLLHUP));
    }
    rt_hw_interrupt_enable(level);
    if (!tty->pty_open_count && !peer->pty_open_count)
    {
        pty_endpoint_release(tty);
        pty_endpoint_release(peer);
    }
    fd->data = RT_NULL;
    rt_mutex_release(&ptmx_lock);
    return 0;
}

static int pts_open(struct dfs_fd *fd)
{
    struct tty_struct *tty = fd->fnode->data;
    int ret = -EIO;
    rt_base_t level;

    rt_mutex_take(&ptmx_lock, RT_WAITING_FOREVER);
    if (tty->init_flag != TTY_INIT_FLAG_INITED ||
        !tty->other_struct->pty_open_count || tty->pts_lock)
        goto out;
    if (tty->pty_open_count == (unsigned int)-1)
    {
        ret = -EMFILE;
        goto out;
    }
    level = rt_hw_interrupt_disable();
    tty->pty_open_count++;
    tty->other_struct->pty_hungup = 0;
    fd->data = tty;
    if (current && !(fd->flags & O_NOCTTY) && current->leader &&
        !current->tty && tty->session == -1)
    {
        current->tty = tty;
        current->tty_old_pgrp = 0;
        tty->session = current->session;
        tty->pgrp = current->__pgrp;
        tty->foreground = current;
    }
    rt_hw_interrupt_enable(level);
    ret = 0;
out:
    rt_mutex_release(&ptmx_lock);
    return ret;
}

static int ptmx_open(struct dfs_fd *fd)
{
    struct pty_pair *pair = RT_NULL;
    int i, ret = -ENOSPC;

    /* The registered per-pair master is an internal rt_device endpoint. */
    if (fd->fnode->data != &ptm_driver)
        return -EIO;
    rt_mutex_take(&ptmx_lock, RT_WAITING_FOREVER);
    for (i = 0; i < PTY_PTS_SIZE; i++)
    {
        if (pty_pairs[i].master.init_flag == TTY_INIT_FLAG_NONE)
        {
            pair = &pty_pairs[i];
            break;
        }
    }
    if (!pair)
        goto out;
    ret = pty_endpoint_init(&pair->master, i, PTY_TYPE_MASTER);
    if (ret == 0)
        ret = pty_endpoint_init(&pair->slave, i, PTY_TYPE_SLAVE);
    if (ret < 0)
    {
        pty_endpoint_release(&pair->master);
        pty_endpoint_release(&pair->slave);
        goto out;
    }
    pair->master.other_struct = &pair->slave;
    pair->slave.other_struct = &pair->master;
    pair->master.pty_open_count = 1;
    fd->data = &pair->master;
out:
    rt_mutex_release(&ptmx_lock);
    return ret;
}

static int ptmx_register(void)
{
    int ret = rt_mutex_init(&ptmx_lock, "ptmx", RT_IPC_FLAG_PRIO);
    if (ret != RT_EOK)
        return ret;
    tty_set_fops(&ptmx_fops);
    ptmx_fops.open = ptmx_open;
    ptmx_fops.close = pty_file_close;
    tty_set_fops(&pts_fops);
    pts_fops.open = pts_open;
    pts_fops.close = pty_file_close;
    ptm_driver.type = TTY_DRIVER_TYPE_PTY;
    ptm_driver.parent.type = RT_Device_Class_Char;
    ret = rt_device_register(&ptm_driver.parent, "ptmx", RT_DEVICE_FLAG_RDWR);
    if (ret == RT_EOK)
        ptm_driver.parent.fops = &ptmx_fops;
    return ret;
}
INIT_DEVICE_EXPORT(ptmx_register);

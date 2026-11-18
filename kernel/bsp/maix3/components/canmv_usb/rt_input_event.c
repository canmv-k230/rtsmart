#include <rthw.h>
#include <rtthread.h>
#include <rtdevice.h>
#include <errno.h>

#ifdef RT_USING_POSIX
#include <devfs.h>
#include <dfs_file.h>
#include <dfs_posix.h>
#include <fcntl.h>
/* The DFS poll bits these fops report (musl's <poll.h> has other values:
 * its POLLOUT is DFS's POLLERR). */
#include <dfs_poll.h>
#if POLLOUT != 0x02 || POLLERR != 0x04 || POLLHUP != 0x08
#error "poll bits must be DFS's (dfs_poll.h), not musl's"
#endif
#endif

#include "rt_input_event.h"

static rt_uint32_t g_input_id_bitmap;
static rt_bool_t g_input_dir_ready;
static struct rt_mutex g_input_lifecycle_lock;
#ifdef RT_USING_SMP
/* One lock covers short FIFO and state updates for the HID keyboard and pointer. */
static struct rt_spinlock g_input_state_lock;
#endif

/* Caller holds g_input_state_lock. */
static void rt_input_reset_fifo_locked(struct rt_input_dev *input)
{
    RT_ASSERT(input != RT_NULL);

    if (input->rx_fifo != RT_NULL) {
        rt_ringbuffer_reset(&input->rx_fifo->rb);
    }
}

static int rt_input_alloc_id(void)
{
    int id;

    for (id = 0; id < 32; id++) {
        rt_uint32_t mask = (rt_uint32_t)1u << id;

        if ((g_input_id_bitmap & mask) == 0) {
            g_input_id_bitmap |= mask;
            return id;
        }
    }

    return -1;
}

static void rt_input_free_id(int id)
{
    if (id < 0 || id >= 32)
        return;

    g_input_id_bitmap &= ~((rt_uint32_t)1u << id);
}

#ifdef RT_USING_POSIX
static rt_err_t input_fops_rx_ind(rt_device_t dev, rt_size_t size)
{
    rt_wqueue_wakeup_all(&dev->wait_queue, (void *)POLLIN);
    return RT_EOK;
}

static int input_fops_open(struct dfs_fd *fd)
{
    int ret;
    rt_base_t level;
    rt_device_t device;
    struct rt_input_dev *input;

    device = (rt_device_t)fd->fnode->data;
    RT_ASSERT(device != RT_NULL);
    input = (struct rt_input_dev *)device;
    rt_mutex_take(&g_input_lifecycle_lock, RT_WAITING_FOREVER);

    level = rt_spin_lock_irqsave(&g_input_state_lock);
    if (!input->connected) {
        rt_spin_unlock_irqrestore(&g_input_state_lock, level);
        rt_mutex_release(&g_input_lifecycle_lock);
        return -ENODEV;
    }
    rt_spin_unlock_irqrestore(&g_input_state_lock, level);

    /* rt_device_open() speaks RT_E*; user space needs an errno. */
    ret = devfs_device_open(device, 0);
    if (ret == 0 && (fd->flags & O_ACCMODE) != O_WRONLY) {
        level = rt_spin_lock_irqsave(&g_input_state_lock);
        input->reader_count++;
        input->parent.rx_indicate = input_fops_rx_ind;
        rt_spin_unlock_irqrestore(&g_input_state_lock, level);
    }
    rt_mutex_release(&g_input_lifecycle_lock);
    return ret;
}

static int input_fops_close(struct dfs_fd *fd)
{
    rt_base_t level;
    rt_device_t device;
    struct rt_input_dev *input;

    device = (rt_device_t)fd->fnode->data;
    RT_ASSERT(device != RT_NULL);
    input = (struct rt_input_dev *)device;

    rt_mutex_take(&g_input_lifecycle_lock, RT_WAITING_FOREVER);
    if ((fd->flags & O_ACCMODE) != O_WRONLY) {
        level = rt_spin_lock_irqsave(&g_input_state_lock);
        if (input->reader_count > 0 && --input->reader_count == 0)
            input->parent.rx_indicate = RT_NULL;
        rt_spin_unlock_irqrestore(&g_input_state_lock, level);
    }
    rt_device_close(device);
    rt_mutex_release(&g_input_lifecycle_lock);

    return 0;
}

static int input_fops_ioctl(struct dfs_fd *fd, int cmd, void *args)
{
    return rt_device_control((rt_device_t)fd->fnode->data, cmd, args);
}

static int input_poll_readable(void *data, struct rt_pollreq *req)
{
    struct rt_input_dev *input = data;
    rt_base_t level;
    int mask = 0;

    rt_poll_add(&input->parent.wait_queue, req);
    level = rt_spin_lock_irqsave(&g_input_state_lock);
    if (!input->connected)
        mask |= POLLHUP | POLLERR;
    if (input->rx_fifo && rt_ringbuffer_data_len(&input->rx_fifo->rb))
        mask |= POLLIN;
    rt_spin_unlock_irqrestore(&g_input_state_lock, level);
    return mask;
}

static int input_fops_read(struct dfs_fd *fd, void *buf, size_t count)
{
    rt_device_t dev = fd->fnode->data;

    if (!count)
        return 0;
    for (;;) {
        int mask;
        int size = rt_device_read(dev, -1, buf, count);
        if (size > 0)
            return size;

        /* Register before rechecking readiness so unplug cannot be missed. */
        mask = rt_poll_wait(input_poll_readable, dev, POLLIN, (fd->flags & O_NONBLOCK) ? 0 : -1);
        if (mask <= 0)
            return mask ? mask : -EAGAIN;
        if (mask & (POLLHUP | POLLERR))
            return -ENODEV;
    }
}

static int input_fops_write(struct dfs_fd *fd, const void *buf, size_t count)
{
    (void)fd;
    (void)buf;
    (void)count;
    return 0;
}

static int input_fops_poll(struct dfs_fd *fd, struct rt_pollreq *req)
{
    return (fd->flags & O_ACCMODE) == O_WRONLY ? 0 : input_poll_readable(fd->fnode->data, req);
}

static const struct dfs_file_ops input_fops = {
    input_fops_open,
    input_fops_close,
    input_fops_ioctl,
    input_fops_read,
    input_fops_write,
    RT_NULL,
    RT_NULL,
    RT_NULL,
    input_fops_poll,
};
#endif

static rt_err_t rt_input_init(struct rt_device *dev)
{
    rt_base_t level;
    struct rt_input_dev *input;

    RT_ASSERT(dev != RT_NULL);
    input = (struct rt_input_dev *)dev;
    level = rt_spin_lock_irqsave(&g_input_state_lock);
    input->rx_fifo = RT_NULL;
    input->reader_count = 0;
    rt_spin_unlock_irqrestore(&g_input_state_lock, level);
    if (input->rx_bufsz == 0)
        input->rx_bufsz = INPUT_EVENT_RX_BUFSIZE;

    return RT_EOK;
}

static rt_err_t rt_input_open(struct rt_device *dev, rt_uint16_t oflag)
{
    rt_base_t level;
    struct rt_input_dev *input;
    struct rt_input_rx_fifo *rx_fifo;

    (void)oflag;
    RT_ASSERT(dev != RT_NULL);

    input = (struct rt_input_dev *)dev;
    level = rt_spin_lock_irqsave(&g_input_state_lock);
    if (input->rx_fifo != RT_NULL) {
        rt_spin_unlock_irqrestore(&g_input_state_lock, level);
        return RT_EOK;
    }
    rt_spin_unlock_irqrestore(&g_input_state_lock, level);

    rx_fifo = (struct rt_input_rx_fifo *)rt_malloc(sizeof(struct rt_input_rx_fifo) + input->rx_bufsz);
    if (rx_fifo == RT_NULL)
        return -RT_ENOMEM;

    rt_ringbuffer_init(&rx_fifo->rb, rx_fifo->buffer, input->rx_bufsz);
    level = rt_spin_lock_irqsave(&g_input_state_lock);
    if (input->rx_fifo == RT_NULL) {
        input->rx_fifo = rx_fifo;
        rx_fifo = RT_NULL;
    }
    rt_spin_unlock_irqrestore(&g_input_state_lock, level);
    if (rx_fifo != RT_NULL)
        rt_free(rx_fifo);

    return RT_EOK;
}

static rt_err_t rt_input_close(struct rt_device *dev)
{
    rt_base_t level;
    struct rt_input_dev *input;
    struct rt_input_rx_fifo *rx_fifo;

    RT_ASSERT(dev != RT_NULL);
    input = (struct rt_input_dev *)dev;

    level = rt_spin_lock_irqsave(&g_input_state_lock);
    rx_fifo = input->rx_fifo;
    input->rx_fifo = RT_NULL;
    rt_spin_unlock_irqrestore(&g_input_state_lock, level);
    if (rx_fifo != RT_NULL)
        rt_free(rx_fifo);

    return RT_EOK;
}

static rt_size_t rt_input_read(struct rt_device *dev, rt_off_t pos, void *buffer, rt_size_t size)
{
    struct rt_input_dev *input;
    rt_size_t recv_len;
    rt_base_t level;

    (void)pos;
    RT_ASSERT(dev != RT_NULL);
    if (size == 0)
        return 0;

    input = (struct rt_input_dev *)dev;
    level = rt_spin_lock_irqsave(&g_input_state_lock);
    if (input->rx_fifo == RT_NULL)
        recv_len = 0;
    else
        recv_len = rt_ringbuffer_get(&input->rx_fifo->rb, buffer, size);
    rt_spin_unlock_irqrestore(&g_input_state_lock, level);

    return recv_len;
}

static rt_size_t rt_input_write(struct rt_device *dev, rt_off_t pos, const void *buffer, rt_size_t size)
{
    (void)dev;
    (void)pos;
    (void)buffer;
    (void)size;
    return 0;
}

/* DFS forwards this result to ioctl callers: all failures are negative POSIX errno. */
static rt_err_t rt_input_control(struct rt_device *dev, int cmd, void *args)
{
    struct rt_input_dev *input = (struct rt_input_dev *)dev;
    struct rt_input_info info;
    rt_base_t level = rt_spin_lock_irqsave(&g_input_state_lock);
    if (!input->connected) {
        rt_spin_unlock_irqrestore(&g_input_state_lock, level);
        return -ENODEV;
    }
    if (cmd != RT_INPUT_CTRL_GET_INFO || args == RT_NULL) {
        rt_spin_unlock_irqrestore(&g_input_state_lock, level);
        return -EINVAL;
    }
    info = input->info;
    rt_spin_unlock_irqrestore(&g_input_state_lock, level);
    rt_memcpy(args, &info, sizeof(info));
    return RT_EOK;
}

#ifdef RT_USING_DEVICE_OPS
static const struct rt_device_ops input_ops = {
    rt_input_init,
    rt_input_open,
    rt_input_close,
    rt_input_read,
    rt_input_write,
    rt_input_control,
};
#endif

void rt_input_set_capability(struct rt_input_info *info, rt_uint16_t type, rt_uint16_t code)
{
    rt_uint32_t mask;

    RT_ASSERT(info != RT_NULL);

    if (type >= 32)
        return;

    info->ev_bits |= (rt_uint32_t)1u << type;

    if (code >= 32)
        return;

    mask = (rt_uint32_t)1u << code;

    switch (type) {
    case EV_KEY:
        info->key_bits |= mask;
        break;
    case EV_REL:
        info->rel_bits |= mask;
        break;
    case EV_ABS:
        info->abs_bits |= mask;
        break;
    default:
        break;
    }
}

static void rt_input_connect_locked(struct rt_input_dev *input, const struct rt_input_info *info)
{
    RT_ASSERT(info != RT_NULL);
    input->info = *info;
    if (!input->info.name[0])
        rt_memcpy(input->info.name, "input", sizeof("input"));
    input->connected = RT_TRUE;
#ifdef RT_USING_POSIX
    if (input->reader_count)
        input->parent.rx_indicate = input_fops_rx_ind;
#endif
}

rt_err_t rt_input_dev_register(struct rt_input_dev *input, const struct rt_input_info *info)
{
    char name[RT_NAME_MAX];
    struct rt_device *device;
    rt_err_t ret;
    int id;

    RT_ASSERT(input != RT_NULL);
    rt_mutex_take(&g_input_lifecycle_lock, RT_WAITING_FOREVER);

    if (!g_input_dir_ready) {
#ifdef RT_USING_POSIX
        mkdir("/dev/input", 0777);
#endif
        g_input_dir_ready = RT_TRUE;
    }

    if (input->rx_bufsz == 0)
        input->rx_bufsz = INPUT_EVENT_RX_BUFSIZE;

    id = rt_input_alloc_id();
    if (id < 0) {
        ret = -RT_ENOMEM;
        goto out;
    }

    input->id = id;
    rt_snprintf(name, sizeof(name), "input/event%d", input->id);

    device = &input->parent;
    device->type = RT_Device_Class_Char;
    device->rx_indicate = RT_NULL;
    device->tx_complete = RT_NULL;

#ifdef RT_USING_DEVICE_OPS
    device->ops = &input_ops;
#else
    device->init = rt_input_init;
    device->open = rt_input_open;
    device->close = rt_input_close;
    device->read = rt_input_read;
    device->write = rt_input_write;
    device->control = rt_input_control;
#endif

    ret = rt_device_register(device, name, RT_DEVICE_FLAG_RDONLY);
    if (ret != RT_EOK) {
        rt_input_free_id(input->id);
        input->id = -1;
        goto out;
    }

#ifdef RT_USING_POSIX
    device->fops = &input_fops;
#endif

    {
        rt_base_t level = rt_spin_lock_irqsave(&g_input_state_lock);
        input->reader_count = 0;
        rt_input_connect_locked(input, info);
        rt_spin_unlock_irqrestore(&g_input_state_lock, level);
    }

out:
    rt_mutex_release(&g_input_lifecycle_lock);
    return ret;
}

rt_err_t rt_input_dev_reconnect(struct rt_input_dev *input, const struct rt_input_info *info)
{
    rt_base_t level;
    rt_err_t ret = RT_EOK;

    RT_ASSERT(input != RT_NULL);
    rt_mutex_take(&g_input_lifecycle_lock, RT_WAITING_FOREVER);

    if (input->id < 0) {
        ret = -RT_ERROR;
        goto out;
    }

    level = rt_spin_lock_irqsave(&g_input_state_lock);
    rt_input_reset_fifo_locked(input);
    rt_input_connect_locked(input, info);
    rt_spin_unlock_irqrestore(&g_input_state_lock, level);
out:
    rt_mutex_release(&g_input_lifecycle_lock);
    return ret;
}

rt_err_t rt_input_dev_disconnect(struct rt_input_dev *input)
{
    rt_base_t level;

    RT_ASSERT(input != RT_NULL);
    rt_mutex_take(&g_input_lifecycle_lock, RT_WAITING_FOREVER);

    if (input->id < 0) {
        rt_mutex_release(&g_input_lifecycle_lock);
        return RT_EOK;
    }

    level = rt_spin_lock_irqsave(&g_input_state_lock);
    rt_input_reset_fifo_locked(input);
    input->connected = RT_FALSE;
    input->parent.rx_indicate = RT_NULL;
    rt_spin_unlock_irqrestore(&g_input_state_lock, level);
    rt_wqueue_wakeup_all(&input->parent.wait_queue, (void *)(POLLHUP | POLLERR | POLLIN));

    rt_mutex_release(&g_input_lifecycle_lock);
    return RT_EOK;
}

rt_err_t rt_input_dev_unregister(struct rt_input_dev *input)
{
    rt_err_t ret = RT_EOK;
    rt_base_t level;

    RT_ASSERT(input != RT_NULL);
    rt_mutex_take(&g_input_lifecycle_lock, RT_WAITING_FOREVER);
    if (input->id < 0)
        goto out;
    if (input->parent.ref_count) {
        ret = -RT_EBUSY;
        goto out;
    }

    level = rt_spin_lock_irqsave(&g_input_state_lock);
    input->connected = RT_FALSE;
    input->parent.rx_indicate = RT_NULL;
    rt_spin_unlock_irqrestore(&g_input_state_lock, level);
    /* The final close already released the FIFO. */
    ret = rt_device_unregister(&input->parent);
    if (ret == RT_EOK) {
        rt_input_free_id(input->id);
        input->id = -1;
    }
out:
    rt_mutex_release(&g_input_lifecycle_lock);
    return ret;
}

rt_err_t rt_input_report(struct rt_input_dev *input, rt_uint16_t type, rt_uint16_t code, rt_int32_t value)
{
    struct input_event event;
    rt_size_t put_len;
    rt_base_t level;

    RT_ASSERT(input != RT_NULL);
    event.type = type;
    event.code = code;
    event.value = value;

    level = rt_spin_lock_irqsave(&g_input_state_lock);
    if (!input->connected) {
        rt_spin_unlock_irqrestore(&g_input_state_lock, level);
        return -RT_ERROR;
    }
    if (input->rx_fifo == RT_NULL) {
        rt_spin_unlock_irqrestore(&g_input_state_lock, level);
        return -RT_EEMPTY;
    }
    put_len = rt_ringbuffer_put(&input->rx_fifo->rb, (const rt_uint8_t *)&event, sizeof(event));
    rt_spin_unlock_irqrestore(&g_input_state_lock, level);

    if (put_len != sizeof(event))
        return -RT_EFULL;

    return RT_EOK;
}

rt_err_t rt_input_sync(struct rt_input_dev *input)
{
    rt_base_t level;
    rt_err_t ret;
    rt_err_t (*rx_indicate)(rt_device_t dev, rt_size_t size);

    RT_ASSERT(input != RT_NULL);

    ret = rt_input_report(input, EV_SYN, SYN_REPORT, 0);
    level = rt_spin_lock_irqsave(&g_input_state_lock);
    rx_indicate = input->parent.rx_indicate;
    rt_spin_unlock_irqrestore(&g_input_state_lock, level);
    if (ret == RT_EOK && rx_indicate != RT_NULL)
        rx_indicate(&input->parent, sizeof(struct input_event));

    return ret;
}

static int rt_input_event_init(void)
{
    rt_spin_lock_init(&g_input_state_lock);
    return rt_mutex_init(&g_input_lifecycle_lock, "input", RT_IPC_FLAG_PRIO);
}
INIT_DEVICE_EXPORT(rt_input_event_init);

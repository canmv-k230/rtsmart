#include <rthw.h>
#include <rtthread.h>
#include <rtdevice.h>
#include <errno.h>

#ifdef RT_USING_POSIX
#include <dfs_file.h>
#include <dfs_posix.h>
#include <fcntl.h>
#include <poll.h>
#endif

#include "rt_input_event.h"

static rt_uint32_t g_input_id_bitmap;
static rt_bool_t g_input_dir_ready;

static void rt_input_free_id(int id);

static void rt_input_reset_fifo(struct rt_input_dev *input)
{
    RT_ASSERT(input != RT_NULL);

    if (input->rx_fifo != RT_NULL) {
        rt_ringbuffer_reset(&input->rx_fifo->rb);
    }
}

static void rt_input_finalize_unregister(struct rt_input_dev *input)
{
    RT_ASSERT(input != RT_NULL);

    if (input->id < 0)
        return;

    rt_device_unregister(&input->parent);
    rt_input_free_id(input->id);
    input->id = -1;
    if (input->rx_fifo != RT_NULL) {
        rt_free(input->rx_fifo);
        input->rx_fifo = RT_NULL;
    }
    input->connected = RT_FALSE;
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
    rt_wqueue_wakeup(&(dev->wait_queue), (void *)POLLIN);
    return RT_EOK;
}

static int input_fops_open(struct dfs_fd *fd)
{
    rt_device_t device;
    struct rt_input_dev *input;

    device = (rt_device_t)fd->fnode->data;
    RT_ASSERT(device != RT_NULL);
    input = (struct rt_input_dev *)device;

    if (!input->connected)
        return -ENODEV;

    if ((fd->flags & O_ACCMODE) != O_WRONLY)
        rt_device_set_rx_indicate(device, input_fops_rx_ind);

    return rt_device_open(device, 0);
}

static int input_fops_close(struct dfs_fd *fd)
{
    rt_device_t device;

    device = (rt_device_t)fd->fnode->data;
    RT_ASSERT(device != RT_NULL);

    rt_device_set_rx_indicate(device, RT_NULL);
    rt_device_close(device);

    return 0;
}

static int input_fops_ioctl(struct dfs_fd *fd, int cmd, void *args)
{
    rt_device_t device;
    struct rt_input_dev *input;

    device = (rt_device_t)fd->fnode->data;
    RT_ASSERT(device != RT_NULL);
    input = (struct rt_input_dev *)device;

    if (!input->connected)
        return -ENODEV;

    return rt_device_control(device, cmd, args);
}

static int input_fops_read(struct dfs_fd *fd, void *buf, size_t count)
{
    int size = 0;
    rt_device_t device;
    struct rt_input_dev *input;

    device = (rt_device_t)fd->fnode->data;
    RT_ASSERT(device != RT_NULL);
    input = (struct rt_input_dev *)device;

    do {
        size = rt_device_read(device, -1, buf, count);
        if (size > 0)
            break;

        if (!input->connected) {
            size = -ENODEV;
            break;
        }

        if (fd->flags & O_NONBLOCK) {
            size = -EAGAIN;
            break;
        }

        rt_wqueue_wait(&(device->wait_queue), 0, RT_WAITING_FOREVER);
    } while (size <= 0);

    return size;
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
    int mask = 0;
    int flags;
    rt_device_t device;
    struct rt_input_dev *input;

    device = (rt_device_t)fd->fnode->data;
    RT_ASSERT(device != RT_NULL);

    input = (struct rt_input_dev *)device;

    flags = fd->flags & O_ACCMODE;
    if (flags == O_RDONLY || flags == O_RDWR) {
        rt_base_t level;

        rt_poll_add(&(device->wait_queue), req);

        if (!input->connected)
            mask |= POLLHUP | POLLERR;

        if (input->rx_fifo) {
            level = rt_hw_interrupt_disable();
            if (rt_ringbuffer_data_len(&input->rx_fifo->rb))
                mask |= POLLIN;
            rt_hw_interrupt_enable(level);
        }
    }

    return mask;
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
    struct rt_input_dev *input;

    RT_ASSERT(dev != RT_NULL);
    input = (struct rt_input_dev *)dev;
    input->rx_fifo = RT_NULL;
    if (input->rx_bufsz == 0)
        input->rx_bufsz = INPUT_EVENT_RX_BUFSIZE;

    return RT_EOK;
}

static rt_err_t rt_input_open(struct rt_device *dev, rt_uint16_t oflag)
{
    struct rt_input_dev *input;
    struct rt_input_rx_fifo *rx_fifo;

    (void)oflag;
    RT_ASSERT(dev != RT_NULL);

    input = (struct rt_input_dev *)dev;
    if (input->rx_fifo != RT_NULL)
        return RT_EOK;

    rx_fifo = (struct rt_input_rx_fifo *)rt_malloc(sizeof(struct rt_input_rx_fifo) + input->rx_bufsz);
    if (rx_fifo == RT_NULL)
        return -RT_ENOMEM;

    rt_ringbuffer_init(&rx_fifo->rb, rx_fifo->buffer, input->rx_bufsz);
    input->rx_fifo = rx_fifo;

    return RT_EOK;
}

static rt_err_t rt_input_close(struct rt_device *dev)
{
    struct rt_input_dev *input;

    RT_ASSERT(dev != RT_NULL);
    input = (struct rt_input_dev *)dev;

    if (dev->ref_count <= 1 && input->rx_fifo != RT_NULL) {
        rt_free(input->rx_fifo);
        input->rx_fifo = RT_NULL;
    }

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
    if (input->rx_fifo == RT_NULL)
        return 0;

    level = rt_hw_interrupt_disable();
    recv_len = rt_ringbuffer_get(&input->rx_fifo->rb, buffer, size);
    rt_hw_interrupt_enable(level);

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

static rt_err_t rt_input_control(struct rt_device *dev, int cmd, void *args)
{
    struct rt_input_dev *input;

    (void)dev;
    input = (struct rt_input_dev *)dev;

    switch (cmd) {
    case RT_INPUT_CTRL_GET_INFO:
        if (args == RT_NULL)
            return -RT_EINVAL;

        rt_memcpy(args, &input->info, sizeof(input->info));
        return RT_EOK;
    default:
        return -RT_EINVAL;
    }
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

void rt_input_set_name(struct rt_input_dev *input, const char *name)
{
    RT_ASSERT(input != RT_NULL);

    if (name == RT_NULL) {
        input->info.name[0] = '\0';
        return;
    }

    rt_strncpy(input->info.name, name, sizeof(input->info.name) - 1);
    input->info.name[sizeof(input->info.name) - 1] = '\0';
}

void rt_input_set_kind(struct rt_input_dev *input, rt_uint32_t kind)
{
    RT_ASSERT(input != RT_NULL);
    input->info.kind = kind;
}

void rt_input_set_capability(struct rt_input_dev *input, rt_uint16_t type, rt_uint16_t code)
{
    rt_uint32_t mask;

    RT_ASSERT(input != RT_NULL);

    if (type >= 32)
        return;

    input->info.ev_bits |= (rt_uint32_t)1u << type;

    if (code >= 32)
        return;

    mask = (rt_uint32_t)1u << code;

    switch (type) {
    case EV_KEY:
        input->info.key_bits |= mask;
        break;
    case EV_REL:
        input->info.rel_bits |= mask;
        break;
    case EV_ABS:
        input->info.abs_bits |= mask;
        break;
    default:
        break;
    }
}

rt_err_t rt_input_dev_register(struct rt_input_dev *input)
{
    char name[RT_NAME_MAX];
    struct rt_device *device;
    rt_err_t ret;
    int id;

    RT_ASSERT(input != RT_NULL);

    if (!g_input_dir_ready) {
#ifdef RT_USING_POSIX
        mkdir("/dev/input", 0777);
#endif
        g_input_dir_ready = RT_TRUE;
    }

    if (input->rx_bufsz == 0)
        input->rx_bufsz = INPUT_EVENT_RX_BUFSIZE;

    if (input->info.name[0] == '\0')
        rt_input_set_name(input, "input");

    id = rt_input_alloc_id();
    if (id < 0)
        return -RT_ENOMEM;

    input->id = id;
    input->connected = RT_TRUE;
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
        return ret;
    }

#ifdef RT_USING_POSIX
    device->fops = &input_fops;
#endif

    return RT_EOK;
}

rt_err_t rt_input_dev_reconnect(struct rt_input_dev *input)
{
    RT_ASSERT(input != RT_NULL);

    if (input->id < 0)
        return -RT_ERROR;

    rt_input_reset_fifo(input);
    input->connected = RT_TRUE;
    return RT_EOK;
}

rt_err_t rt_input_dev_disconnect(struct rt_input_dev *input)
{
    RT_ASSERT(input != RT_NULL);

    if (input->id < 0)
        return RT_EOK;

    rt_input_reset_fifo(input);
    input->connected = RT_FALSE;
    input->parent.rx_indicate = RT_NULL;
    rt_wqueue_wakeup(&(input->parent.wait_queue), (void *)(POLLHUP | POLLERR | POLLIN));

    return RT_EOK;
}

void rt_input_dev_unregister(struct rt_input_dev *input)
{
    RT_ASSERT(input != RT_NULL);

    rt_input_finalize_unregister(input);
}

rt_err_t rt_input_report(struct rt_input_dev *input, rt_uint16_t type, rt_uint16_t code, rt_int32_t value)
{
    struct input_event event;
    rt_size_t put_len;
    rt_base_t level;

    RT_ASSERT(input != RT_NULL);
    if (!input->connected)
        return -RT_ERROR;

    if (input->rx_fifo == RT_NULL)
        return -RT_EEMPTY;

    event.type = type;
    event.code = code;
    event.value = value;

    level = rt_hw_interrupt_disable();
    put_len = rt_ringbuffer_put(&input->rx_fifo->rb, (const rt_uint8_t *)&event, sizeof(event));
    rt_hw_interrupt_enable(level);

    if (put_len != sizeof(event))
        return -RT_EFULL;

    return RT_EOK;
}

rt_err_t rt_input_sync(struct rt_input_dev *input)
{
    rt_err_t ret;

    RT_ASSERT(input != RT_NULL);

    ret = rt_input_report(input, EV_SYN, SYN_REPORT, 0);
    if (ret == RT_EOK && input->parent.rx_indicate != RT_NULL)
        input->parent.rx_indicate(&input->parent, sizeof(struct input_event));

    return ret;
}
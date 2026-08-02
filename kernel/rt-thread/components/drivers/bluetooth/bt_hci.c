/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include <rtdevice.h>

#ifdef RT_USING_POSIX
#include <dfs_poll.h>
#include <dfs_posix.h>
#endif

static rt_uint16_t bt_hci_get_le16(const rt_uint8_t *data)
{
    return (rt_uint16_t)data[0] | ((rt_uint16_t)data[1] << 8);
}

static rt_bool_t bt_hci_packet_valid(rt_uint8_t packet_type,
                                     const rt_uint8_t *data,
                                     rt_size_t length, rt_bool_t from_host)
{
    rt_size_t expected;

    if (!data)
    {
        return RT_FALSE;
    }

    switch (packet_type)
    {
    case RT_BT_HCI_H4_CMD:
        if (!from_host || length < 3)
        {
            return RT_FALSE;
        }
        expected = 3 + data[2];
        break;
    case RT_BT_HCI_H4_ACL:
        if (length < 4)
        {
            return RT_FALSE;
        }
        expected = 4 + bt_hci_get_le16(data + 2);
        break;
    case RT_BT_HCI_H4_SCO:
        if (length < 3)
        {
            return RT_FALSE;
        }
        expected = 3 + data[2];
        break;
    case RT_BT_HCI_H4_EVT:
        if (from_host || length < 2)
        {
            return RT_FALSE;
        }
        expected = 2 + data[1];
        break;
    case RT_BT_HCI_H4_ISO:
        if (length < 4)
        {
            return RT_FALSE;
        }
        expected = 4 + (bt_hci_get_le16(data + 2) & 0x3fff);
        break;
    default:
        return RT_FALSE;
    }

    return expected == length;
}

static rt_size_t bt_hci_read(rt_device_t device, rt_off_t position,
                             void *buffer, rt_size_t size)
{
    struct rt_bt_hci_device *hci = (struct rt_bt_hci_device *)device;
    rt_base_t level;
    rt_size_t length;

    (void)position;
    if (!buffer || !size)
    {
        return 0;
    }

    level = rt_spin_lock_irqsave(&hci->rx_lock);
    length = rt_ringbuffer_get(&hci->rx_ring, buffer, size);
    rt_spin_unlock_irqrestore(&hci->rx_lock, level);
    return length;
}

static rt_err_t bt_hci_open(rt_device_t device, rt_uint16_t oflag)
{
    struct rt_bt_hci_device *hci = (struct rt_bt_hci_device *)device;
    rt_err_t result = RT_EOK;

    (void)oflag;
    rt_bt_hci_flush(hci);
    if (hci->ops->open)
    {
        result = hci->ops->open(hci);
        if (result == -RT_ENOSYS)
        {
            result = -RT_EIO;
        }
    }
    return result;
}

static rt_err_t bt_hci_close(rt_device_t device)
{
    struct rt_bt_hci_device *hci = (struct rt_bt_hci_device *)device;
    rt_err_t result = RT_EOK;

    if (hci->ops->close)
    {
        result = hci->ops->close(hci);
    }
    rt_bt_hci_flush(hci);
    return result;
}

static rt_size_t bt_hci_write(rt_device_t device, rt_off_t position,
                              const void *buffer, rt_size_t size)
{
    struct rt_bt_hci_device *hci = (struct rt_bt_hci_device *)device;
    const rt_uint8_t *packet = buffer;
    rt_err_t result;

    (void)position;
    if (!packet || size < 2 ||
        !bt_hci_packet_valid(packet[0], packet + 1, size - 1, RT_TRUE))
    {
        return -RT_EINVAL;
    }

    result = rt_bt_hci_send(hci, packet[0], packet + 1, size - 1);
    return result == RT_EOK ? size : result;
}

static rt_err_t bt_hci_control(rt_device_t device, int command, void *argument)
{
    (void)device;
    (void)command;
    (void)argument;
    return -RT_ENOSYS;
}

#ifdef RT_USING_POSIX
static rt_err_t bt_hci_fops_rx_indicate(rt_device_t device, rt_size_t size)
{
    (void)size;
    rt_wqueue_wakeup(&device->wait_queue, (void *)POLLIN);
    return RT_EOK;
}

static int bt_hci_fops_open(struct dfs_fd *fd)
{
    rt_device_t device = (rt_device_t)fd->fnode->data;
    rt_uint16_t flags;
    rt_err_t result;

    RT_ASSERT(device != RT_NULL);
    switch (fd->flags & O_ACCMODE)
    {
    case O_RDONLY:
        flags = RT_DEVICE_FLAG_RDONLY | RT_DEVICE_FLAG_INT_RX;
        break;
    case O_WRONLY:
        flags = RT_DEVICE_FLAG_WRONLY;
        break;
    case O_RDWR:
        flags = RT_DEVICE_FLAG_RDWR | RT_DEVICE_FLAG_INT_RX;
        break;
    default:
        return -RT_EINVAL;
    }

    if ((fd->flags & O_ACCMODE) != O_WRONLY)
    {
        rt_device_set_rx_indicate(device, bt_hci_fops_rx_indicate);
    }
    result = rt_device_open(device, flags);
    if (result != RT_EOK)
    {
        rt_device_set_rx_indicate(device, RT_NULL);
    }
    return result;
}

static int bt_hci_fops_close(struct dfs_fd *fd)
{
    rt_device_t device = (rt_device_t)fd->fnode->data;

    RT_ASSERT(device != RT_NULL);
    rt_device_set_rx_indicate(device, RT_NULL);
    return rt_device_close(device);
}

static int bt_hci_fops_ioctl(struct dfs_fd *fd, int command, void *argument)
{
    rt_device_t device = (rt_device_t)fd->fnode->data;

    RT_ASSERT(device != RT_NULL);
    return rt_device_control(device, command, argument);
}

static int bt_hci_fops_read(struct dfs_fd *fd, void *buffer, size_t size)
{
    rt_device_t device = (rt_device_t)fd->fnode->data;

    RT_ASSERT(device != RT_NULL);
    return rt_device_read(device, -1, buffer, size);
}

static int bt_hci_fops_write(struct dfs_fd *fd, const void *buffer, size_t size)
{
    rt_device_t device = (rt_device_t)fd->fnode->data;

    RT_ASSERT(device != RT_NULL);
    return rt_device_write(device, -1, buffer, size);
}

static int bt_hci_fops_poll(struct dfs_fd *fd, struct rt_pollreq *request)
{
    struct rt_bt_hci_device *hci;
    rt_device_t device = (rt_device_t)fd->fnode->data;
    int mask = 0;

    RT_ASSERT(device != RT_NULL);
    if ((fd->flags & O_ACCMODE) == O_WRONLY)
    {
        return mask;
    }

    hci = (struct rt_bt_hci_device *)device;
    rt_poll_add(&device->wait_queue, request);
    if (rt_bt_hci_rx_length(hci) > 0)
    {
        mask |= POLLIN;
    }
    return mask;
}

static const struct dfs_file_ops g_bt_hci_fops = {
    .open = bt_hci_fops_open,
    .close = bt_hci_fops_close,
    .ioctl = bt_hci_fops_ioctl,
    .read = bt_hci_fops_read,
    .write = bt_hci_fops_write,
    .poll = bt_hci_fops_poll,
};
#endif

#ifdef RT_USING_DEVICE_OPS
static const struct rt_device_ops g_bt_hci_device_ops = {
    RT_NULL,
    bt_hci_open,
    bt_hci_close,
    bt_hci_read,
    bt_hci_write,
    bt_hci_control,
};
#endif

rt_err_t rt_bt_hci_register(struct rt_bt_hci_device *hci, const char *name,
                            const struct rt_bt_hci_ops *ops,
                            rt_uint8_t *rx_buffer, rt_uint16_t rx_buffer_size,
                            void *user_data)
{
    rt_err_t result;

    if (!hci || !name || !ops || !ops->send || !rx_buffer ||
        rx_buffer_size < 64 || rx_buffer_size > 0x7fff)
    {
        return -RT_EINVAL;
    }

    rt_memset(hci, 0, sizeof(*hci));
    hci->ops = ops;
    rt_ringbuffer_init(&hci->rx_ring, rx_buffer, rx_buffer_size);
    rt_spin_lock_init(&hci->rx_lock);
    result = rt_mutex_init(&hci->tx_lock, name, RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        return result;
    }

    hci->parent.type = RT_Device_Class_Char;
#ifdef RT_USING_DEVICE_OPS
    hci->parent.ops = &g_bt_hci_device_ops;
#else
    hci->parent.init = RT_NULL;
    hci->parent.open = bt_hci_open;
    hci->parent.close = bt_hci_close;
    hci->parent.read = bt_hci_read;
    hci->parent.write = bt_hci_write;
    hci->parent.control = bt_hci_control;
#endif
    hci->parent.user_data = user_data;
    result = rt_device_register(&hci->parent, name,
                                RT_DEVICE_FLAG_RDWR | RT_DEVICE_FLAG_INT_RX |
                                RT_DEVICE_FLAG_STANDALONE);
    if (result != RT_EOK)
    {
        rt_mutex_detach(&hci->tx_lock);
    }
#ifdef RT_USING_POSIX
    else
    {
        hci->parent.fops = &g_bt_hci_fops;
    }
#endif
    return result;
}
RTM_EXPORT(rt_bt_hci_register);

rt_err_t rt_bt_hci_send(struct rt_bt_hci_device *hci, rt_uint8_t packet_type,
                        const rt_uint8_t *data, rt_size_t length)
{
    rt_base_t level;
    rt_err_t result;

    if (!hci || !hci->ops || !hci->ops->send ||
        !bt_hci_packet_valid(packet_type, data, length, RT_TRUE))
    {
        return -RT_EINVAL;
    }

    result = rt_mutex_take(&hci->tx_lock, RT_WAITING_FOREVER);
    if (result != RT_EOK)
    {
        return result;
    }
    result = hci->ops->send(hci, packet_type, data, length);
    level = rt_spin_lock_irqsave(&hci->rx_lock);
    if (result == RT_EOK)
    {
        hci->stats.tx_packets++;
        hci->stats.tx_bytes += length + 1;
    }
    else
    {
        hci->stats.tx_errors++;
    }
    rt_spin_unlock_irqrestore(&hci->rx_lock, level);
    rt_mutex_release(&hci->tx_lock);
    return result;
}
RTM_EXPORT(rt_bt_hci_send);

rt_err_t rt_bt_hci_receive(struct rt_bt_hci_device *hci,
                           rt_uint8_t packet_type, const rt_uint8_t *data,
                           rt_size_t length)
{
    rt_base_t level;
    rt_size_t available;
    rt_size_t queued;

    if (!hci || !bt_hci_packet_valid(packet_type, data, length, RT_FALSE))
    {
        if (hci)
        {
            level = rt_spin_lock_irqsave(&hci->rx_lock);
            hci->stats.rx_invalid++;
            rt_spin_unlock_irqrestore(&hci->rx_lock, level);
        }
        return -RT_EINVAL;
    }
    if (length > 0xffff - 1)
    {
        level = rt_spin_lock_irqsave(&hci->rx_lock);
        hci->stats.rx_invalid++;
        rt_spin_unlock_irqrestore(&hci->rx_lock, level);
        return -RT_EINVAL;
    }

    level = rt_spin_lock_irqsave(&hci->rx_lock);
    available = rt_ringbuffer_space_len(&hci->rx_ring);
    if (available < length + 1)
    {
        hci->stats.rx_dropped++;
        rt_spin_unlock_irqrestore(&hci->rx_lock, level);
        return -RT_EFULL;
    }
    queued = rt_ringbuffer_putchar(&hci->rx_ring, packet_type);
    queued += rt_ringbuffer_put(&hci->rx_ring, data, length);
    hci->stats.rx_packets++;
    hci->stats.rx_bytes += queued;
    rt_spin_unlock_irqrestore(&hci->rx_lock, level);

    if (hci->parent.rx_indicate)
    {
        hci->parent.rx_indicate(&hci->parent, rt_bt_hci_rx_length(hci));
    }
    return RT_EOK;
}
RTM_EXPORT(rt_bt_hci_receive);

void rt_bt_hci_flush(struct rt_bt_hci_device *hci)
{
    rt_base_t level;

    if (!hci)
    {
        return;
    }
    level = rt_spin_lock_irqsave(&hci->rx_lock);
    rt_ringbuffer_reset(&hci->rx_ring);
    rt_spin_unlock_irqrestore(&hci->rx_lock, level);
}
RTM_EXPORT(rt_bt_hci_flush);

rt_size_t rt_bt_hci_rx_length(struct rt_bt_hci_device *hci)
{
    rt_base_t level;
    rt_size_t length;

    if (!hci)
    {
        return 0;
    }
    level = rt_spin_lock_irqsave(&hci->rx_lock);
    length = rt_ringbuffer_data_len(&hci->rx_ring);
    rt_spin_unlock_irqrestore(&hci->rx_lock, level);
    return length;
}
RTM_EXPORT(rt_bt_hci_rx_length);

void rt_bt_hci_get_stats(struct rt_bt_hci_device *hci,
                         struct rt_bt_hci_stats *stats)
{
    rt_base_t level;

    if (!hci || !stats)
    {
        return;
    }
    level = rt_spin_lock_irqsave(&hci->rx_lock);
    *stats = hci->stats;
    rt_spin_unlock_irqrestore(&hci->rx_lock, level);
}
RTM_EXPORT(rt_bt_hci_get_stats);

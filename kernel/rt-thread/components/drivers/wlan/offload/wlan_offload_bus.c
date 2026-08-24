/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <wlan_offload_bus.h>

static rt_err_t wlan_offload_bus_lock(struct rt_wlan_offload_bus *bus)
{
    if (!bus || bus->state == RT_WLAN_OFFLOAD_BUS_UNINITIALIZED)
    {
        return -RT_EINVAL;
    }
    return rt_mutex_take(&bus->state_lock, RT_WAITING_FOREVER);
}

rt_err_t rt_wlan_offload_bus_init(struct rt_wlan_offload_bus *bus,
                             const struct rt_wlan_offload_bus_config *config)
{
    rt_err_t result;

    if (!bus || !config || !config->ops || !config->ops->transmit ||
        (int)config->type < 0 || config->type > RT_WLAN_OFFLOAD_BUS_CUSTOM ||
        ((config->capabilities & RT_WLAN_OFFLOAD_BUS_CAP_TX_PRIORITY) &&
         !config->ops->transmit_priority) ||
        ((config->capabilities & RT_WLAN_OFFLOAD_BUS_CAP_SUSPEND) &&
         (!config->ops->suspend || !config->ops->resume)))
    {
        return -RT_EINVAL;
    }
    if (config->alignment && (config->alignment & (config->alignment - 1)))
    {
        return -RT_EINVAL;
    }

    rt_memset(bus, 0, sizeof(*bus));
    bus->type = config->type;
    bus->ops = config->ops;
    bus->capabilities = config->capabilities;
    bus->max_tx_size = config->max_tx_size;
    bus->max_rx_size = config->max_rx_size;
    bus->alignment = config->alignment ? config->alignment : 1;
    bus->headroom = config->headroom;
    bus->tailroom = config->tailroom;
    bus->driver_data = config->driver_data;
    result = rt_mutex_init(&bus->state_lock, "wo-bus", RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        return result;
    }
    result = rt_mutex_init(&bus->tx_lock, "wo-btx", RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        rt_mutex_detach(&bus->state_lock);
        return result;
    }
    bus->state = RT_WLAN_OFFLOAD_BUS_STOPPED;
    return RT_EOK;
}

rt_err_t rt_wlan_offload_bus_deinit(struct rt_wlan_offload_bus *bus)
{
    rt_err_t result;

    if (!bus || bus->state == RT_WLAN_OFFLOAD_BUS_UNINITIALIZED)
    {
        return -RT_EINVAL;
    }
    result = rt_wlan_offload_bus_stop(bus);
    if (result != RT_EOK)
    {
        return result;
    }

    /*
     * Take both locks as a final barrier before invalidating them.  A packet
     * sender can enter just after bus_stop() releases tx_lock, observe the
     * stopped state, and still be unwinding from rt_wlan_offload_bus_transmit().
     * Releasing and then detaching these locks lets that sender acquire or
     * release an object whose storage may already have been reused by a USB
     * re-enumeration.
     */
    result = rt_mutex_take(&bus->tx_lock, RT_WAITING_FOREVER);
    if (result != RT_EOK)
    {
        return result;
    }
    result = rt_mutex_take(&bus->state_lock, RT_WAITING_FOREVER);
    if (result != RT_EOK)
    {
        rt_mutex_release(&bus->tx_lock);
        return result;
    }
    bus->state = RT_WLAN_OFFLOAD_BUS_UNINITIALIZED;
    bus->ops = RT_NULL;
    bus->rx_handler = RT_NULL;
    bus->event_handler = RT_NULL;
    bus->driver_data = RT_NULL;

    /* rt_mutex_detach() removes a mutex held by the current thread from its
     * taken list, so there must not be a release-to-detach race here. */
    result = rt_mutex_detach(&bus->state_lock);
    if (result != RT_EOK)
    {
        rt_mutex_release(&bus->tx_lock);
        return result;
    }
    return rt_mutex_detach(&bus->tx_lock);
}

rt_err_t rt_wlan_offload_bus_start(struct rt_wlan_offload_bus *bus)
{
    rt_err_t result;

    result = wlan_offload_bus_lock(bus);
    if (result != RT_EOK)
    {
        return result;
    }
    if (bus->state == RT_WLAN_OFFLOAD_BUS_STARTED)
    {
        result = RT_EOK;
        goto exit;
    }
    if (bus->state != RT_WLAN_OFFLOAD_BUS_STOPPED)
    {
        result = -RT_EBUSY;
        goto exit;
    }
    bus->state = RT_WLAN_OFFLOAD_BUS_STARTING;
    rt_mutex_release(&bus->state_lock);

    rt_mutex_take(&bus->tx_lock, RT_WAITING_FOREVER);
    result = bus->ops->start ? bus->ops->start(bus) : RT_EOK;
    rt_mutex_release(&bus->tx_lock);

    rt_mutex_take(&bus->state_lock, RT_WAITING_FOREVER);
    if (bus->state == RT_WLAN_OFFLOAD_BUS_STARTING)
    {
        bus->state = result == RT_EOK ? RT_WLAN_OFFLOAD_BUS_STARTED : RT_WLAN_OFFLOAD_BUS_FAILED;
    }
    else if (bus->state == RT_WLAN_OFFLOAD_BUS_FAILED && result == RT_EOK)
    {
        result = -RT_EIO;
    }

exit:
    rt_mutex_release(&bus->state_lock);
    return result;
}

rt_err_t rt_wlan_offload_bus_stop(struct rt_wlan_offload_bus *bus)
{
    rt_err_t result;
    enum rt_wlan_offload_bus_state previous_state;

    result = wlan_offload_bus_lock(bus);
    if (result != RT_EOK)
    {
        return result;
    }
    if (bus->state == RT_WLAN_OFFLOAD_BUS_STOPPED)
    {
        result = RT_EOK;
        goto exit;
    }
    if (bus->state == RT_WLAN_OFFLOAD_BUS_UNINITIALIZED ||
        bus->state == RT_WLAN_OFFLOAD_BUS_STARTING ||
        bus->state == RT_WLAN_OFFLOAD_BUS_STOPPING)
    {
        result = -RT_EBUSY;
        goto exit;
    }
    previous_state = bus->state;
    bus->state = RT_WLAN_OFFLOAD_BUS_STOPPING;
    rt_mutex_release(&bus->state_lock);

    rt_mutex_take(&bus->tx_lock, RT_WAITING_FOREVER);
    result = bus->ops->stop ? bus->ops->stop(bus) : RT_EOK;
    rt_mutex_release(&bus->tx_lock);

    rt_mutex_take(&bus->state_lock, RT_WAITING_FOREVER);
    if (result == RT_EOK)
    {
        bus->state = RT_WLAN_OFFLOAD_BUS_STOPPED;
    }
    else if (bus->state == RT_WLAN_OFFLOAD_BUS_STOPPING)
    {
        bus->state = previous_state;
    }

exit:
    rt_mutex_release(&bus->state_lock);
    return result;
}

rt_err_t rt_wlan_offload_bus_transmit(struct rt_wlan_offload_bus *bus,
                                 const void *data, rt_size_t length)
{
    return rt_wlan_offload_bus_transmit_priority(
        bus, RT_WLAN_OFFLOAD_BUS_PRIORITY_NORMAL, data, length);
}

rt_err_t rt_wlan_offload_bus_transmit_priority(
    struct rt_wlan_offload_bus *bus, enum rt_wlan_offload_bus_priority priority,
    const void *data, rt_size_t length)
{
    rt_err_t result;

    if (!bus || !data || !length ||
        (int)priority < RT_WLAN_OFFLOAD_BUS_PRIORITY_LOW ||
        priority > RT_WLAN_OFFLOAD_BUS_PRIORITY_CONTROL)
    {
        return -RT_EINVAL;
    }
    result = rt_mutex_take(&bus->tx_lock, RT_WAITING_FOREVER);
    if (result != RT_EOK)
    {
        return result;
    }
    result = wlan_offload_bus_lock(bus);
    if (result != RT_EOK)
    {
        rt_mutex_release(&bus->tx_lock);
        return result;
    }
    if (bus->state != RT_WLAN_OFFLOAD_BUS_STARTED)
    {
        result = -RT_EBUSY;
    }
    else if (bus->max_tx_size && length > bus->max_tx_size)
    {
        result = -RT_EINVAL;
    }
    else
    {
        rt_mutex_release(&bus->state_lock);
        if ((bus->capabilities & RT_WLAN_OFFLOAD_BUS_CAP_TX_PRIORITY) &&
            bus->ops->transmit_priority)
        {
            result = bus->ops->transmit_priority(bus, priority, data, length);
        }
        else
        {
            result = bus->ops->transmit(bus, data, length);
        }
        rt_mutex_release(&bus->tx_lock);
        return result;
    }
    rt_mutex_release(&bus->state_lock);
    rt_mutex_release(&bus->tx_lock);
    return result;
}

rt_err_t rt_wlan_offload_bus_transmitv(
    struct rt_wlan_offload_bus *bus, enum rt_wlan_offload_bus_priority priority,
    const struct rt_wlan_offload_bus_iovec *vectors, rt_size_t vector_count)
{
    rt_size_t total_length = 0;
    rt_size_t index;
    rt_err_t result;

    if (!bus || !vectors || !vector_count ||
        (int)priority < RT_WLAN_OFFLOAD_BUS_PRIORITY_LOW ||
        priority > RT_WLAN_OFFLOAD_BUS_PRIORITY_CONTROL)
    {
        return -RT_EINVAL;
    }
    for (index = 0; index < vector_count; index++)
    {
        if (!vectors[index].data || !vectors[index].length ||
            total_length > (rt_size_t)-1 - vectors[index].length)
        {
            return -RT_EINVAL;
        }
        total_length += vectors[index].length;
    }

    result = rt_mutex_take(&bus->tx_lock, RT_WAITING_FOREVER);
    if (result != RT_EOK)
    {
        return result;
    }
    result = wlan_offload_bus_lock(bus);
    if (result != RT_EOK)
    {
        rt_mutex_release(&bus->tx_lock);
        return result;
    }
    if (bus->state != RT_WLAN_OFFLOAD_BUS_STARTED)
    {
        result = -RT_EBUSY;
    }
    else if ((bus->max_tx_size && total_length > bus->max_tx_size) ||
             !bus->ops->transmitv)
    {
        result = bus->ops->transmitv ? -RT_EINVAL : -RT_ENOSYS;
    }
    else
    {
        rt_mutex_release(&bus->state_lock);
        result = bus->ops->transmitv(bus, vectors, vector_count);
        rt_mutex_release(&bus->tx_lock);
        return result;
    }
    rt_mutex_release(&bus->state_lock);
    rt_mutex_release(&bus->tx_lock);
    return result;
}

rt_err_t rt_wlan_offload_bus_reset(struct rt_wlan_offload_bus *bus)
{
    rt_err_t result;

    if (!bus)
    {
        return -RT_EINVAL;
    }
    result = rt_mutex_take(&bus->tx_lock, RT_WAITING_FOREVER);
    if (result != RT_EOK)
    {
        return result;
    }
    result = wlan_offload_bus_lock(bus);
    if (result != RT_EOK)
    {
        rt_mutex_release(&bus->tx_lock);
        return result;
    }
    /* Every other operation gates on the bus state; do the same here. In the
     * transitional states the transport is between its own start/stop
     * bookkeeping and the matching callback, so its hardware is in an
     * indeterminate state even though tx_lock keeps the calls serialized. */
    if (bus->state == RT_WLAN_OFFLOAD_BUS_STARTING ||
        bus->state == RT_WLAN_OFFLOAD_BUS_STOPPING)
    {
        result = -RT_EBUSY;
        rt_mutex_release(&bus->state_lock);
        rt_mutex_release(&bus->tx_lock);
        return result;
    }
    if (!bus->ops->reset)
    {
        result = -RT_ENOSYS;
        rt_mutex_release(&bus->state_lock);
        rt_mutex_release(&bus->tx_lock);
        return result;
    }
    rt_mutex_release(&bus->state_lock);
    result = bus->ops->reset(bus);
    rt_mutex_release(&bus->tx_lock);
    return result;
}

rt_err_t rt_wlan_offload_bus_suspend(struct rt_wlan_offload_bus *bus)
{
    rt_err_t result;

    result = wlan_offload_bus_lock(bus);
    if (result != RT_EOK)
    {
        return result;
    }
    if (bus->state != RT_WLAN_OFFLOAD_BUS_STARTED)
    {
        result = -RT_EBUSY;
    }
    else if (!bus->ops->suspend)
    {
        result = -RT_ENOSYS;
    }
    else
    {
        bus->state = RT_WLAN_OFFLOAD_BUS_SUSPENDED;
        rt_mutex_release(&bus->state_lock);
        rt_mutex_take(&bus->tx_lock, RT_WAITING_FOREVER);
        result = bus->ops->suspend(bus);
        rt_mutex_release(&bus->tx_lock);
        rt_mutex_take(&bus->state_lock, RT_WAITING_FOREVER);
        if (result != RT_EOK && bus->state == RT_WLAN_OFFLOAD_BUS_SUSPENDED)
        {
            bus->state = RT_WLAN_OFFLOAD_BUS_STARTED;
        }
        else if (result == RT_EOK && bus->state == RT_WLAN_OFFLOAD_BUS_FAILED)
        {
            result = -RT_EIO;
        }
    }
    rt_mutex_release(&bus->state_lock);
    return result;
}

rt_err_t rt_wlan_offload_bus_resume(struct rt_wlan_offload_bus *bus)
{
    rt_err_t result;

    result = wlan_offload_bus_lock(bus);
    if (result != RT_EOK)
    {
        return result;
    }
    if (bus->state != RT_WLAN_OFFLOAD_BUS_SUSPENDED)
    {
        result = -RT_EBUSY;
    }
    else if (!bus->ops->resume)
    {
        result = -RT_ENOSYS;
    }
    else
    {
        bus->state = RT_WLAN_OFFLOAD_BUS_STARTING;
        rt_mutex_release(&bus->state_lock);
        rt_mutex_take(&bus->tx_lock, RT_WAITING_FOREVER);
        result = bus->ops->resume(bus);
        rt_mutex_release(&bus->tx_lock);
        rt_mutex_take(&bus->state_lock, RT_WAITING_FOREVER);
        if (bus->state == RT_WLAN_OFFLOAD_BUS_STARTING)
        {
            bus->state = result == RT_EOK ? RT_WLAN_OFFLOAD_BUS_STARTED : RT_WLAN_OFFLOAD_BUS_FAILED;
        }
        else if (bus->state == RT_WLAN_OFFLOAD_BUS_FAILED && result == RT_EOK)
        {
            result = -RT_EIO;
        }
    }
    rt_mutex_release(&bus->state_lock);
    return result;
}

rt_err_t rt_wlan_offload_bus_control(struct rt_wlan_offload_bus *bus,
                                int command, void *argument)
{
    rt_err_t result;

    if (!bus)
    {
        return -RT_EINVAL;
    }
    result = rt_mutex_take(&bus->tx_lock, RT_WAITING_FOREVER);
    if (result != RT_EOK)
    {
        return result;
    }
    result = wlan_offload_bus_lock(bus);
    if (result != RT_EOK)
    {
        rt_mutex_release(&bus->tx_lock);
        return result;
    }
    /* Same state gate as rt_wlan_offload_bus_reset(): do not enter the
     * transport while it is between its own start/stop bookkeeping and the
     * matching callback. */
    if (bus->state == RT_WLAN_OFFLOAD_BUS_STARTING ||
        bus->state == RT_WLAN_OFFLOAD_BUS_STOPPING)
    {
        result = -RT_EBUSY;
        rt_mutex_release(&bus->state_lock);
        rt_mutex_release(&bus->tx_lock);
        return result;
    }
    if (!bus->ops->control)
    {
        result = -RT_ENOSYS;
        rt_mutex_release(&bus->state_lock);
        rt_mutex_release(&bus->tx_lock);
        return result;
    }
    rt_mutex_release(&bus->state_lock);
    result = bus->ops->control(bus, command, argument);
    rt_mutex_release(&bus->tx_lock);
    return result;
}

void rt_wlan_offload_bus_set_callbacks(struct rt_wlan_offload_bus *bus,
                                  rt_wlan_offload_bus_rx_handler_t rx_handler,
                                  rt_wlan_offload_bus_event_handler_t event_handler,
                                  void *parameter)
{
    rt_err_t result;

    if (!bus || bus->state == RT_WLAN_OFFLOAD_BUS_UNINITIALIZED)
    {
        return;
    }
    result = wlan_offload_bus_lock(bus);
    if (result != RT_EOK)
    {
        return;
    }
    bus->rx_handler = rx_handler;
    bus->event_handler = event_handler;
    bus->callback_parameter = parameter;
    rt_mutex_release(&bus->state_lock);
}

rt_err_t rt_wlan_offload_bus_rx(struct rt_wlan_offload_bus *bus,
                           const void *data, rt_size_t length)
{
    rt_wlan_offload_bus_rx_handler_t handler;
    void *parameter;
    rt_bool_t invalid_length;
    rt_err_t result;

    if (!bus || !data || !length)
    {
        return -RT_EINVAL;
    }
    result = wlan_offload_bus_lock(bus);
    if (result != RT_EOK)
    {
        return result;
    }
    if (bus->state != RT_WLAN_OFFLOAD_BUS_STARTING &&
        bus->state != RT_WLAN_OFFLOAD_BUS_STARTED)
    {
        rt_mutex_release(&bus->state_lock);
        return -RT_EBUSY;
    }
    invalid_length = bus->max_rx_size && length > bus->max_rx_size;
    handler = bus->rx_handler;
    parameter = bus->callback_parameter;
    rt_mutex_release(&bus->state_lock);

    if (invalid_length)
    {
        rt_wlan_offload_bus_notify(bus, RT_WLAN_OFFLOAD_BUS_EVENT_ERROR, -RT_EINVAL);
        return -RT_EINVAL;
    }
    if (handler)
    {
        return handler(bus, data, length, parameter);
    }
    return -RT_ENOSYS;
}

void rt_wlan_offload_bus_notify(struct rt_wlan_offload_bus *bus,
                           enum rt_wlan_offload_bus_event event,
                           rt_err_t status)
{
    rt_wlan_offload_bus_event_handler_t handler;
    void *parameter;

    if (!bus || (int)event < 0 || event > RT_WLAN_OFFLOAD_BUS_EVENT_WAKE ||
        wlan_offload_bus_lock(bus) != RT_EOK)
    {
        return;
    }
    if (event == RT_WLAN_OFFLOAD_BUS_EVENT_ERROR ||
        event == RT_WLAN_OFFLOAD_BUS_EVENT_UNAVAILABLE)
    {
        bus->state = RT_WLAN_OFFLOAD_BUS_FAILED;
    }
    handler = bus->event_handler;
    parameter = bus->callback_parameter;
    rt_mutex_release(&bus->state_lock);

    if (handler)
    {
        handler(bus, event, status, parameter);
    }
}

void *rt_wlan_offload_bus_get_driver_data(struct rt_wlan_offload_bus *bus)
{
    return bus ? bus->driver_data : RT_NULL;
}

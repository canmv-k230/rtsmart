/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <wlan_offload_command.h>

struct rt_wlan_offload_pending_command
{
    rt_list_t node;
    struct rt_completion completion;
    rt_uint32_t token;
    rt_uint16_t command_id;
    rt_uint16_t confirmation_id;
    void *response;
    rt_size_t response_capacity;
    rt_size_t response_length;
    rt_err_t result;
    rt_bool_t pending;
};

static rt_err_t wlan_offload_command_lock(
    struct rt_wlan_offload_command_manager *manager)
{
    if (!manager ||
        manager->state == RT_WLAN_OFFLOAD_COMMAND_MANAGER_UNINITIALIZED)
    {
        return -RT_EINVAL;
    }
    return rt_mutex_take(&manager->lock, RT_WAITING_FOREVER);
}

static void wlan_offload_command_remove_locked(
    struct rt_wlan_offload_command_manager *manager,
    struct rt_wlan_offload_pending_command *command)
{
    if (!command->pending)
    {
        return;
    }
    rt_list_remove(&command->node);
    command->pending = RT_FALSE;
    manager->pending_count--;
}

static rt_uint32_t wlan_offload_command_alloc_token_locked(
    struct rt_wlan_offload_command_manager *manager)
{
    rt_bool_t in_use;

    do
    {
        manager->next_token++;
        if (!manager->next_token)
        {
            manager->next_token++;
        }
        in_use = RT_FALSE;
        if (manager->pending_count)
        {
            rt_list_t *node;

            rt_list_for_each(node, &manager->pending)
            {
                const struct rt_wlan_offload_pending_command *command =
                    rt_list_entry(node, struct rt_wlan_offload_pending_command,
                                  node);

                if (command->token == manager->next_token)
                {
                    in_use = RT_TRUE;
                    break;
                }
            }
        }
    } while (in_use);
    return manager->next_token;
}

rt_err_t rt_wlan_offload_command_manager_init(
    struct rt_wlan_offload_command_manager *manager,
    const struct rt_wlan_offload_command_manager_config *config)
{
    rt_err_t result;

    if (!manager || !config || !config->push || !config->max_pending)
    {
        return -RT_EINVAL;
    }

    rt_memset(manager, 0, sizeof(*manager));
    result = rt_mutex_init(&manager->lock, "wo-xact", RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        return result;
    }
    rt_list_init(&manager->pending);
    rt_completion_init(&manager->idle);
    manager->max_pending = config->max_pending;
    manager->push = config->push;
    manager->driver_data = config->driver_data;
    manager->state = RT_WLAN_OFFLOAD_COMMAND_MANAGER_READY;
    return RT_EOK;
}

rt_err_t rt_wlan_offload_command_manager_deinit(
    struct rt_wlan_offload_command_manager *manager)
{
    rt_list_t *node;
    rt_list_t *next;
    rt_bool_t wait;
    rt_err_t result;

    result = wlan_offload_command_lock(manager);
    if (result != RT_EOK)
    {
        return result;
    }
    manager->state = RT_WLAN_OFFLOAD_COMMAND_MANAGER_UNINITIALIZED;
    rt_list_for_each_safe(node, next, &manager->pending)
    {
        struct rt_wlan_offload_pending_command *command =
            rt_list_entry(node, struct rt_wlan_offload_pending_command, node);

        command->result = -RT_EIO;
        wlan_offload_command_remove_locked(manager, command);
        rt_completion_done(&command->completion);
    }
    manager->push = RT_NULL;
    manager->driver_data = RT_NULL;
    wait = manager->active_pushes != 0;
    manager->deinitializing = wait;
    if (wait)
    {
        rt_mutex_release(&manager->lock);
        result = rt_completion_wait(&manager->idle, RT_WAITING_FOREVER);
        if (result != RT_EOK)
        {
            return result;
        }
        result = rt_mutex_take(&manager->lock, RT_WAITING_FOREVER);
        if (result != RT_EOK)
        {
            return result;
        }
    }
    manager->deinitializing = RT_FALSE;

    /* Keep the lock owned through detach.  Releasing it first allows a
     * completion path awakened above to race with object destruction. */
    return rt_mutex_detach(&manager->lock);
}

rt_err_t rt_wlan_offload_command_execute(
    struct rt_wlan_offload_command_manager *manager,
    rt_uint16_t command_id, rt_uint16_t confirmation_id,
    const void *request, rt_size_t request_length,
    void *response, rt_size_t response_capacity,
    rt_size_t *response_length, rt_int32_t timeout,
    rt_uint32_t *token)
{
    struct rt_wlan_offload_pending_command command;
    rt_wlan_offload_command_push_t push;
    void *driver_data;
    rt_err_t result;
    rt_err_t wait_result;

    if (!manager || (request_length && !request) ||
        (response_capacity && !response) || timeout == 0)
    {
        return -RT_EINVAL;
    }

    rt_memset(&command, 0, sizeof(command));
    rt_list_init(&command.node);
    rt_completion_init(&command.completion);
    command.command_id = command_id;
    command.confirmation_id = confirmation_id;
    command.response = response;
    command.response_capacity = response_capacity;
    command.result = -RT_EINTR;

    result = wlan_offload_command_lock(manager);
    if (result != RT_EOK)
    {
        return result;
    }
    if (manager->state != RT_WLAN_OFFLOAD_COMMAND_MANAGER_READY)
    {
        result = -RT_EIO;
        goto unlock;
    }
    if (manager->pending_count >= manager->max_pending)
    {
        result = -RT_EFULL;
        goto unlock;
    }

    command.token = wlan_offload_command_alloc_token_locked(manager);
    command.pending = RT_TRUE;
    rt_list_insert_before(&manager->pending, &command.node);
    manager->pending_count++;
    if (token)
    {
        *token = command.token;
    }
    push = manager->push;
    driver_data = manager->driver_data;
    manager->active_pushes++;
    rt_mutex_release(&manager->lock);

    result = push(manager, command.token, command.command_id,
                  request, request_length, driver_data);
    wait_result = rt_mutex_take(&manager->lock, RT_WAITING_FOREVER);
    if (wait_result != RT_EOK)
    {
        return wait_result;
    }
    manager->active_pushes--;
    if (!manager->active_pushes && manager->deinitializing)
    {
        rt_completion_done(&manager->idle);
    }
    if (result != RT_EOK)
    {
        if (command.pending)
        {
            wlan_offload_command_remove_locked(manager, &command);
            command.result = result;
        }
        result = command.result;
        rt_mutex_release(&manager->lock);
        return result;
    }
    rt_mutex_release(&manager->lock);

    wait_result = rt_completion_wait(&command.completion, timeout);
    if (wait_result == RT_EOK)
    {
        if (response_length)
        {
            *response_length = command.response_length;
        }
        return command.result;
    }
    if (wlan_offload_command_lock(manager) != RT_EOK)
    {
        return wait_result;
    }
    if (command.pending)
    {
        wlan_offload_command_remove_locked(manager, &command);
        command.result = wait_result;
    }
    result = command.result;
    if (response_length)
    {
        *response_length = command.response_length;
    }
    rt_mutex_release(&manager->lock);
    return result;

unlock:
    rt_mutex_release(&manager->lock);
    return result;
}

rt_err_t rt_wlan_offload_command_complete(
    struct rt_wlan_offload_command_manager *manager,
    rt_uint32_t token, rt_uint16_t confirmation_id,
    rt_err_t status, const void *response, rt_size_t response_length)
{
    struct rt_wlan_offload_pending_command *command = RT_NULL;
    struct rt_wlan_offload_pending_command *candidate;
    rt_list_t *node;
    rt_err_t result;

    if (!manager || (response_length && !response))
    {
        return -RT_EINVAL;
    }
    result = wlan_offload_command_lock(manager);
    if (result != RT_EOK)
    {
        return result;
    }

    rt_list_for_each(node, &manager->pending)
    {
        candidate = rt_list_entry(node, struct rt_wlan_offload_pending_command,
                                  node);
        if (candidate->confirmation_id != RT_WLAN_OFFLOAD_COMMAND_RESPONSE_ANY &&
            candidate->confirmation_id != confirmation_id)
        {
            continue;
        }
        if (token && candidate->token != token)
        {
            continue;
        }
        if (!token && command)
        {
            result = -RT_EBUSY;
            goto unlock;
        }
        command = candidate;
        if (token)
        {
            break;
        }
    }
    if (!command)
    {
        result = -RT_EEMPTY;
        goto unlock;
    }

    command->response_length = response_length;
    if (response_length && command->response_capacity)
    {
        rt_size_t copy_length = response_length < command->response_capacity ?
                                response_length : command->response_capacity;

        rt_memcpy(command->response, response, copy_length);
    }
    command->result = status;
    if (status == RT_EOK && command->response &&
        response_length > command->response_capacity)
    {
        command->result = -RT_EFULL;
    }
    wlan_offload_command_remove_locked(manager, command);
    rt_completion_done(&command->completion);
    result = RT_EOK;

unlock:
    rt_mutex_release(&manager->lock);
    return result;
}

rt_bool_t rt_wlan_offload_command_is_pending(
    struct rt_wlan_offload_command_manager *manager, rt_uint32_t token)
{
    rt_list_t *node;
    rt_bool_t pending = RT_FALSE;

    if (wlan_offload_command_lock(manager) != RT_EOK)
    {
        return RT_FALSE;
    }
    if (!token)
    {
        pending = manager->pending_count != 0;
        rt_mutex_release(&manager->lock);
        return pending;
    }
    rt_list_for_each(node, &manager->pending)
    {
        const struct rt_wlan_offload_pending_command *command =
            rt_list_entry(node, struct rt_wlan_offload_pending_command, node);

        if (command->token == token)
        {
            pending = RT_TRUE;
            break;
        }
    }
    rt_mutex_release(&manager->lock);
    return pending;
}

rt_err_t rt_wlan_offload_command_manager_fail(
    struct rt_wlan_offload_command_manager *manager, rt_err_t status)
{
    rt_list_t *node;
    rt_list_t *next;
    rt_err_t result;

    if (status == RT_EOK)
    {
        return -RT_EINVAL;
    }
    result = wlan_offload_command_lock(manager);
    if (result != RT_EOK)
    {
        return result;
    }
    manager->state = RT_WLAN_OFFLOAD_COMMAND_MANAGER_FAILED;
    rt_list_for_each_safe(node, next, &manager->pending)
    {
        struct rt_wlan_offload_pending_command *command =
            rt_list_entry(node, struct rt_wlan_offload_pending_command, node);

        command->result = status;
        wlan_offload_command_remove_locked(manager, command);
        rt_completion_done(&command->completion);
    }
    rt_mutex_release(&manager->lock);
    return RT_EOK;
}

rt_err_t rt_wlan_offload_command_manager_reset(
    struct rt_wlan_offload_command_manager *manager)
{
    rt_err_t result = wlan_offload_command_lock(manager);

    if (result != RT_EOK)
    {
        return result;
    }
    if (manager->pending_count)
    {
        result = -RT_EBUSY;
    }
    else
    {
        manager->state = RT_WLAN_OFFLOAD_COMMAND_MANAGER_READY;
        result = RT_EOK;
    }
    rt_mutex_release(&manager->lock);
    return result;
}

void *rt_wlan_offload_command_get_driver_data(
    struct rt_wlan_offload_command_manager *manager)
{
    return manager ? manager->driver_data : RT_NULL;
}

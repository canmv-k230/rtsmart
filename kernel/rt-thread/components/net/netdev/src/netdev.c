/*
 * Copyright (c) 2006-2019, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2019-03-18     ChenYong     First version
 */

#include <stdio.h>
#include <string.h>

#include <rtthread.h>
#include <rthw.h>

#include <netdev_ipaddr.h>
#include <netdev.h>

#ifdef RT_USING_SAL
#include <sal_netdb.h>
#include <sal.h>
#endif /* RT_USING_SAL */

#define DBG_TAG              "netdev"
#define DBG_LVL              DBG_INFO
#include <rtdbg.h>

#define NETDEV_METRIC_LAN           100
#define NETDEV_METRIC_WLAN_STA      600
#define NETDEV_METRIC_UNKNOWN      1000

/* The list of network interface device */
struct netdev *netdev_list;
/* The default network interface device */
struct netdev *netdev_default;
/* A userspace-selected uplink is preferred when it is available. */
static struct netdev *netdev_preferred;

/**
 * This function will register network interface device and
 * add it to network interface device list.
 *
 * @param netdev the network interface device object
 * @param name the network interface device name
 * @param user_data user-specific data
 *
 * @return  0: registered successfully
 *         -1: registered failed
 */
int netdev_register(struct netdev *netdev, const char *name, void *user_data)
{
    rt_base_t level;
    uint16_t flags_mask;
    int index;
    rt_ubase_t len;

    RT_ASSERT(netdev);
    RT_ASSERT(name);

    /* clean network interface device */
    flags_mask = NETDEV_FLAG_UP | NETDEV_FLAG_LINK_UP | NETDEV_FLAG_INTERNET_UP | NETDEV_FLAG_DHCP;
    netdev->flags &= ~flags_mask;
    netdev->type = NETDEV_TYPE_UNKNOWN;
    netdev->metric = NETDEV_METRIC_UNKNOWN;

    ip_addr_set_zero(&(netdev->ip_addr));
    ip_addr_set_zero(&(netdev->netmask));
    ip_addr_set_zero(&(netdev->gw));

    IP_SET_TYPE_VAL(netdev->ip_addr, IPADDR_TYPE_V4);
    IP_SET_TYPE_VAL(netdev->netmask, IPADDR_TYPE_V4);
    IP_SET_TYPE_VAL(netdev->gw, IPADDR_TYPE_V4);
#if NETDEV_IPV6
    for (index = 0; index < NETDEV_IPV6_NUM_ADDRESSES; index++)
    {
        ip_addr_set_zero(&(netdev->ip6_addr[index]));
        IP_SET_TYPE_VAL(netdev->ip_addr, IPADDR_TYPE_V6);
    }
#endif /* NETDEV_IPV6 */
    for (index = 0; index < NETDEV_DNS_SERVERS_NUM; index++)
    {
        ip_addr_set_zero(&(netdev->dns_servers[index]));
        IP_SET_TYPE_VAL(netdev->ip_addr, IPADDR_TYPE_V4);
    }
    netdev->status_callback = RT_NULL;
    netdev->addr_callback = RT_NULL;

    /* fill network interface device */
    len = (rt_strlen(name) >= RT_NAME_MAX) ? RT_NAME_MAX - 1 : rt_strlen(name);
    rt_strncpy(netdev->name, name, len);
    netdev->name[len] = '\0';
    netdev->user_data = user_data;

    /* initialize current network interface device single list */
    rt_slist_init(&(netdev->list));

    level = rt_hw_interrupt_disable();

    if (netdev_list != RT_NULL)
    {
        rt_slist_t *node;

        for (node = &(netdev_list->list); node; node = rt_slist_next(node))
        {
            struct netdev *registered =
                rt_slist_entry(node, struct netdev, list);

            if (rt_strcmp(registered->name, netdev->name) == 0)
            {
                rt_hw_interrupt_enable(level);
                return -RT_EBUSY;
            }
        }
    }

    if (netdev_list == RT_NULL)
    {
        netdev_list = netdev;
    }
    else
    {
        /* tail insertion */
        rt_slist_append(&(netdev_list->list), &(netdev->list));
    }

    rt_hw_interrupt_enable(level);

    return RT_EOK;
}

/**
 * This function will unregister network interface device and
 * delete it from network interface device list.
 *
 * @param netdev the network interface device object
 *
 * @return  0: unregistered successfully
 *         -1: unregistered failed
 */
int netdev_unregister(struct netdev *netdev)
{
    rt_base_t level;
    rt_slist_t *node = RT_NULL;
    struct netdev *cur_netdev = RT_NULL;

    RT_ASSERT(netdev);

    if (netdev_list == RT_NULL)
    {
        return -RT_ERROR;
    }

    level = rt_hw_interrupt_disable();

    for (node = &(netdev_list->list); node; node = rt_slist_next(node))
    {
        cur_netdev = rt_slist_entry(node, struct netdev, list);
        if (cur_netdev == netdev)
        {
            /* find this network interface device in network interface device list */
            if (netdev_list == netdev)
            {
                rt_slist_t *next = rt_slist_next(node);
                if (next)
                {
                    netdev_list = rt_slist_entry(next, struct netdev, list);
                }
                else
                {
                    netdev_list = RT_NULL;
                }
            }
            else
            {
                rt_slist_remove(&(netdev_list->list), &(cur_netdev->list));
            }
            if (netdev_preferred == netdev)
            {
                netdev_preferred = RT_NULL;
            }
            break;
        }
    }
    rt_hw_interrupt_enable(level);

    if (cur_netdev == netdev)
    {
#ifdef RT_USING_SAL
        extern int sal_netdev_cleanup(struct netdev *netdev);
        sal_netdev_cleanup(netdev);
#endif
        netdev_set_default_auto(RT_NULL);
        netdev_update_resolv_conf();
        rt_memset(netdev, 0, sizeof(*netdev));
        return RT_EOK;
    }

    return -RT_ERROR;
}

/**
 * This function will get the first network interface device
 * with the flags in network interface device list.
 *
 * @param flags the network interface device flags
 *
 * @return != NULL: network interface device object
 *            NULL: get failed
 */
struct netdev *netdev_get_first_by_flags(uint16_t flags)
{
    rt_base_t level;
    rt_slist_t *node = RT_NULL;
    struct netdev *netdev = RT_NULL;

    if (netdev_list == RT_NULL)
    {
        return RT_NULL;
    }

    level = rt_hw_interrupt_disable();

    for (node = &(netdev_list->list); node; node = rt_slist_next(node))
    {
        netdev = rt_slist_entry(node, struct netdev, list);
        if (netdev && (netdev->flags & flags) != 0)
        {
            rt_hw_interrupt_enable(level);
            return netdev;
        }
    }

    rt_hw_interrupt_enable(level);

    return RT_NULL;
}

/**
 * This function will get the first network interface device
 * in network interface device list by IP address.
 *
 * @param addr the network interface device IP address
 *
 * @return != NULL: network interface device object
 *            NULL: get failed
 */
struct netdev *netdev_get_by_ipaddr(ip_addr_t *ip_addr)
{
    rt_base_t level;
    rt_slist_t *node = RT_NULL;
    struct netdev *netdev = RT_NULL;

    if (netdev_list == RT_NULL)
    {
        return RT_NULL;
    }

    level = rt_hw_interrupt_disable();

    for (node = &(netdev_list->list); node; node = rt_slist_next(node))
    {
        netdev = rt_slist_entry(node, struct netdev, list);
        if (netdev && ip_addr_cmp(&(netdev->ip_addr), ip_addr))
        {
            rt_hw_interrupt_enable(level);
            return netdev;
        }
    }

    rt_hw_interrupt_enable(level);

    return RT_NULL;
}

/**
 * This function will get network interface device
 * in network interface device list by netdev name.
 *
 * @param name the network interface device name
 *
 * @return != NULL: network interface device object
 *            NULL: get failed
 */
struct netdev *netdev_get_by_name(const char *name)
{
    rt_base_t level;
    rt_slist_t *node = RT_NULL;
    struct netdev *netdev = RT_NULL;

    if (netdev_list == RT_NULL)
    {
        return RT_NULL;
    }

    level = rt_hw_interrupt_disable();

    for (node = &(netdev_list->list); node; node = rt_slist_next(node))
    {
        netdev = rt_slist_entry(node, struct netdev, list);
        if (netdev && rt_strcmp(netdev->name, name) == 0)
        {
            rt_hw_interrupt_enable(level);
            return netdev;
        }
    }

    rt_hw_interrupt_enable(level);

    return RT_NULL;
}

/**
 * This function gets a network interface device by its driver data pointer.
 *
 * @param user_data the driver data pointer supplied to netdev_register
 *
 * @return != NULL: network interface device object
 *            NULL: get failed
 */
struct netdev *netdev_get_by_user_data(const void *user_data)
{
    rt_base_t level;
    rt_slist_t *node;

    if (netdev_list == RT_NULL || user_data == RT_NULL)
    {
        return RT_NULL;
    }

    level = rt_hw_interrupt_disable();
    for (node = &(netdev_list->list); node; node = rt_slist_next(node))
    {
        struct netdev *netdev = rt_slist_entry(node, struct netdev, list);

        if (netdev->user_data == user_data)
        {
            rt_hw_interrupt_enable(level);
            return netdev;
        }
    }
    rt_hw_interrupt_enable(level);

    return RT_NULL;
}

struct netdev *netdev_get_by_type(enum netdev_type type)
{
    rt_base_t level;
    rt_slist_t *node;
    struct netdev *fallback = RT_NULL;

    if (netdev_list == RT_NULL)
    {
        return RT_NULL;
    }

    level = rt_hw_interrupt_disable();
    for (node = &(netdev_list->list); node; node = rt_slist_next(node))
    {
        struct netdev *netdev = rt_slist_entry(node, struct netdev, list);

        if (netdev->type != type)
        {
            continue;
        }
        if (netdev_is_link_up(netdev))
        {
            rt_hw_interrupt_enable(level);
            return netdev;
        }
        if (fallback == RT_NULL)
        {
            fallback = netdev;
        }
    }
    rt_hw_interrupt_enable(level);
    return fallback;
}

#ifdef RT_USING_SAL
/**
 * This function will get the first network interface device
 * in network interface device list by protocol family type.
 *
 * @param family the network interface device protocol family type
 *
 * @return != NULL: network interface device object
 *            NULL: get failed
 */
struct netdev *netdev_get_by_family(int family)
{
    rt_base_t level;
    rt_slist_t *node = RT_NULL;
    struct netdev *netdev = RT_NULL;
    struct sal_proto_family *pf = RT_NULL;

    if (netdev_list == RT_NULL)
    {
        return RT_NULL;
    }

    level = rt_hw_interrupt_disable();

    for (node = &(netdev_list->list); node; node = rt_slist_next(node))
    {
        netdev = rt_slist_entry(node, struct netdev, list);
        pf = (struct sal_proto_family *) netdev->sal_user_data;
        if (pf && pf->skt_ops && pf->family == family && netdev_is_up(netdev))
        {
            rt_hw_interrupt_enable(level);
            return netdev;
        }
    }

    for (node = &(netdev_list->list); node; node = rt_slist_next(node))
    {
        netdev = rt_slist_entry(node, struct netdev, list);
        pf = (struct sal_proto_family *) netdev->sal_user_data;
        if (pf && pf->skt_ops && pf->sec_family == family && netdev_is_up(netdev))
        {
            rt_hw_interrupt_enable(level);
            return netdev;
        }
    }

    rt_hw_interrupt_enable(level);

    return RT_NULL;
}

/**
 * This function will get the family type from network interface device
 *
 * @param netdev network interface device object
 *
 * @return the network interface device family type
 */
int netdev_family_get(struct netdev *netdev)
{
    RT_ASSERT(netdev);

    return ((struct sal_proto_family *)netdev->sal_user_data)->family;
}

#endif /* RT_USING_SAL */

/**
 * This function will set default network interface device.
 *
 * @param netdev the network interface device to change
 */
rt_bool_t netdev_can_be_default(const struct netdev *netdev)
{
    return netdev != RT_NULL &&
           (netdev->type == NETDEV_TYPE_WLAN_STA ||
            netdev->type == NETDEV_TYPE_LAN);
}

static uint16_t netdev_type_metric(enum netdev_type type)
{
    switch (type)
    {
    case NETDEV_TYPE_LAN:
        return NETDEV_METRIC_LAN;
    case NETDEV_TYPE_WLAN_STA:
        return NETDEV_METRIC_WLAN_STA;
    default:
        return NETDEV_METRIC_UNKNOWN;
    }
}

static int netdev_route_readiness(const struct netdev *netdev)
{
    if (!netdev_can_be_default(netdev) || !netdev_is_up(netdev) ||
        !netdev_is_link_up(netdev))
    {
        return -1;
    }
    if (ip_addr_isany(&netdev->ip_addr))
    {
        return 0;
    }
    if (ip_addr_isany(&netdev->gw))
    {
        return 1;
    }
    return 2;
}

static void netdev_apply_default(struct netdev *netdev)
{
    struct netdev *old_default = netdev_default;
    struct netdev *dns_netdev = netdev;
    int index;

    if (netdev == old_default)
    {
        return;
    }

    netdev_default = netdev;

    if (netdev && netdev->ops && netdev->ops->set_default)
    {
        netdev->ops->set_default(netdev);
    }
    else if (!netdev && old_default && old_default->ops &&
             old_default->ops->set_default)
    {
        old_default->ops->set_default(RT_NULL);
    }

    if (dns_netdev && dns_netdev->ops && dns_netdev->ops->set_dns_server)
    {
        for (index = 0; index < NETDEV_DNS_SERVERS_NUM; index++)
        {
            dns_netdev->ops->set_dns_server(
                dns_netdev, index, &dns_netdev->dns_servers[index]);
        }
    }

    netdev_update_resolv_conf();

    LOG_D("Default network interface is now %s.",
          netdev ? netdev->name : "unset");
}

void netdev_set_default(struct netdev *netdev)
{
    if (!netdev_can_be_default(netdev))
    {
        LOG_W("Network interface device(%s) cannot be the default uplink.",
              netdev ? netdev->name : "NULL");
        return;
    }
    netdev_preferred = netdev;
    netdev_set_default_auto(netdev);
}

void netdev_clear_default(void)
{
    netdev_preferred = RT_NULL;
    netdev_set_default_auto(RT_NULL);
}

void netdev_set_default_auto(struct netdev *netdev)
{
    rt_slist_t *node;
    struct netdev *candidate = RT_NULL;
    int candidate_readiness = -1;

    (void)netdev;
    if (netdev_preferred && netdev_can_be_default(netdev_preferred) &&
        netdev_route_readiness(netdev_preferred) == 2)
    {
        netdev_apply_default(netdev_preferred);
        return;
    }
    if (netdev_list != RT_NULL)
    {
        for (node = &(netdev_list->list); node; node = rt_slist_next(node))
        {
            struct netdev *current =
                rt_slist_entry(node, struct netdev, list);
            int readiness = netdev_route_readiness(current);

            if (readiness < 2)
            {
                continue;
            }
            if (candidate == RT_NULL || readiness > candidate_readiness ||
                (readiness == candidate_readiness &&
                 current->metric < candidate->metric))
            {
                candidate = current;
                candidate_readiness = readiness;
            }
        }
    }
    if (candidate)
    {
        netdev_apply_default(candidate);
    }
    else
    {
        netdev_apply_default(RT_NULL);
    }
}

void netdev_set_type(struct netdev *netdev, enum netdev_type type)
{
    if (netdev)
    {
        netdev->type = (uint8_t)type;
        netdev->metric = netdev_type_metric(type);
        netdev_set_default_auto(netdev);
        netdev_update_resolv_conf();
    }
}

void netdev_set_metric(struct netdev *netdev, uint16_t metric)
{
    if (netdev)
    {
        netdev->metric = metric;
        netdev_set_default_auto(netdev);
    }
}

/**
 * This function will enable network interface device .
 *
 * @param netdev the network interface device to change
 *
 * @return  0: set status successfully
 *         -1: set status failed
 */
int netdev_set_up(struct netdev *netdev)
{
    RT_ASSERT(netdev);

    if (!netdev->ops || !netdev->ops->set_up)
    {
        LOG_E("The network interface device(%s) not support to set status.", netdev->name);
        return -RT_ERROR;
    }

    /* network interface device status flags check */
    if (netdev_is_up(netdev))
    {
        return RT_EOK;
    }

    /* execute enable network interface device operations by network interface device driver */
    return netdev->ops->set_up(netdev);
}
/**
 * This function will disable network interface device.
 *
 * @param netdev the network interface device to change
 *
 * @return  0: set status successfully
 *         -1: set sttaus failed
 */
int netdev_set_down(struct netdev *netdev)
{
    RT_ASSERT(netdev);

    if (!netdev->ops || !netdev->ops->set_down)
    {
        LOG_E("The network interface device(%s) not support to set status.", netdev->name);
        return -RT_ERROR;
    }

    /* network interface device status flags check */
    if (!netdev_is_up(netdev))
    {
        return RT_EOK;
    }

    /* execute disable network interface device operations by network interface driver */
    return netdev->ops->set_down(netdev);
}

/**
 * This function will control network interface device DHCP capability enable or disable.
 *
 * @param netdev the network interface device device to change
 * @param is_enable the new DHCP status
 *
 * @return  0: set DHCP status successfully
 *         -1: set DHCP status failed
 */
int netdev_dhcp_enabled(struct netdev *netdev, rt_bool_t is_enabled)
{
    RT_ASSERT(netdev);

    if (!netdev->ops || !netdev->ops->set_dhcp)
    {
        LOG_E("The network interface device(%s) not support to set DHCP status.", netdev->name);
        return -RT_ERROR;
    }

    /* network interface device DHCP flags check */
    if (netdev_is_dhcp_enabled(netdev) == is_enabled)
    {
        return RT_EOK;
    }

    /* execute network interface device DHCP capability control operations */
    return netdev->ops->set_dhcp(netdev, is_enabled);
}

/**
 * This function will set network interface device IP address.
 *
 * @param netdev the network interface device to change
 * @param ipaddr the new IP address
 *
 * @return  0: set IP address successfully
 *         -1: set IP address failed
 */
int netdev_set_ipaddr(struct netdev *netdev, const ip_addr_t *ip_addr)
{
    RT_ASSERT(netdev);
    RT_ASSERT(ip_addr);

    if (!netdev->ops || !netdev->ops->set_addr_info)
    {
        LOG_E("The network interface device(%s) not support to set IP address.", netdev->name);
        return -RT_ERROR;
    }

    if (netdev_is_dhcp_enabled(netdev))
    {
        LOG_E("The network interface device(%s) DHCP capability is enable, not support set IP address.", netdev->name);
        return -RT_ERROR;
    }

     /* execute network interface device set IP address operations */
    return netdev->ops->set_addr_info(netdev, (ip_addr_t *)ip_addr, RT_NULL, RT_NULL);
}

/**
 * This function will set network interface device netmask address.
 *
 * @param netdev the network interface device to change
 * @param netmask the new netmask address
 *
 * @return  0: set netmask address successfully
 *         -1: set netmask address failed
 */
int netdev_set_netmask(struct netdev *netdev, const ip_addr_t *netmask)
{
    RT_ASSERT(netdev);
    RT_ASSERT(netmask);

    if (!netdev->ops || !netdev->ops->set_addr_info)
    {
        LOG_E("The network interface device(%s) not support to set netmask address.", netdev->name);
        return -RT_ERROR;
    }

    if (netdev_is_dhcp_enabled(netdev))
    {
        LOG_E("The network interface device(%s) DHCP capability is enable, not support set netmask address.", netdev->name);
        return -RT_ERROR;
    }

    /* execute network interface device set netmask address operations */
    return netdev->ops->set_addr_info(netdev, RT_NULL, (ip_addr_t *)netmask, RT_NULL);
}

/**
 * This function will set network interface device gateway address.
 *
 * @param netdev the network interface device to change
 * @param gateway the new gateway address
 *
 * @return  0: set gateway address successfully
 *         -1: set gateway address failed
 */
int netdev_set_gw(struct netdev *netdev, const ip_addr_t *gw)
{
    RT_ASSERT(netdev);
    RT_ASSERT(gw);

    if (!netdev->ops || !netdev->ops->set_addr_info)
    {
        LOG_E("The network interface device(%s) not support to set gateway address.", netdev->name);
        return -RT_ERROR;
    }

    if (netdev_is_dhcp_enabled(netdev))
    {
        LOG_E("The network interface device(%s) DHCP capability is enable, not support set gateway address.", netdev->name);
        return -RT_ERROR;
    }

    /* execute network interface device set gateway address operations */
    return netdev->ops->set_addr_info(netdev, RT_NULL, RT_NULL, (ip_addr_t *)gw);
}

/**
 * This function will set network interface device DNS server address.
 *
 * @param netdev the network interface device to change
 * @param dns_server the new DNS server address
 *
 * @return  0: set netmask address successfully
 *         -1: set netmask address failed
 */
int netdev_set_dns_server(struct netdev *netdev, uint8_t dns_num, const ip_addr_t *dns_server)
{
    RT_ASSERT(netdev);
    RT_ASSERT(dns_server);

    if (dns_num >= NETDEV_DNS_SERVERS_NUM)
    {
        LOG_E("The number of DNS servers(%d) set exceeds the maximum number(%d).", dns_num + 1, NETDEV_DNS_SERVERS_NUM);
        return -RT_ERROR;
    }

    if (!netdev->ops || !netdev->ops->set_dns_server)
    {
        LOG_E("The network interface device(%s) not support to set DNS server address.", netdev->name);
        return -RT_ERROR;
    }

    /* execute network interface device set DNS server address operations */
    return netdev->ops->set_dns_server(netdev, dns_num, (ip_addr_t *)dns_server);
}

/**
 * This function will set callback to be called when the network interface device status has been changed.
 *
 * @param netdev the network interface device to change
 * @param status_callback the callback be called when the status has been changed.
 */
void netdev_set_status_callback(struct netdev *netdev, netdev_callback_fn status_callback)
{
    RT_ASSERT(netdev);
    RT_ASSERT(status_callback);

    netdev->status_callback = status_callback;
}

/**
 * This function will set callback to be called when the network interface device address has been changed.
 *
 * @param netdev the network interface device to change
 * @param addr_callback the callback be called when the address has been changed.
 */
void netdev_set_addr_callback(struct netdev *netdev, netdev_callback_fn addr_callback)
{
    RT_ASSERT(netdev);
    RT_ASSERT(addr_callback);

    netdev->addr_callback = addr_callback;
}


/**
 * This function will set network interface device IP address.
 * @NOTE it can only be called in the network interface device driver.
 *
 * @param netdev the network interface device to change
 * @param ipaddr the new IP address
 */
void netdev_low_level_set_ipaddr(struct netdev *netdev, const ip_addr_t *ip_addr)
{
    RT_ASSERT(ip_addr);

    if (netdev && ip_addr_cmp(&(netdev->ip_addr), ip_addr) == 0)
    {
        ip_addr_copy(netdev->ip_addr, *ip_addr);

#ifdef RT_USING_SAL
        /* set network interface device flags to internet up */
        if (netdev_is_up(netdev) && netdev_is_link_up(netdev))
        {
            sal_check_netdev_internet_up(netdev);
        }
#endif /* RT_USING_SAL */

        /* execute IP address change callback function */
        if (netdev->addr_callback)
        {
            netdev->addr_callback(netdev, NETDEV_CB_ADDR_IP);
        }

#ifdef NETDEV_USING_AUTO_DEFAULT
        netdev_set_default_auto(netdev);
#endif
    }
}

/**
 * This function will set network interface device netmask address.
 * @NOTE it can only be called in the network interface device driver.
 *
 * @param netdev the network interface device to change
 * @param netmask the new netmask address
 */
void netdev_low_level_set_netmask(struct netdev *netdev, const ip_addr_t *netmask)
{
    RT_ASSERT(netmask);

    if (netdev && ip_addr_cmp(&(netdev->netmask), netmask) == 0)
    {
        ip_addr_copy(netdev->netmask, *netmask);

#ifdef RT_USING_SAL
        /* set network interface device flags to internet up */
        if (netdev_is_up(netdev) && netdev_is_link_up(netdev) &&
                !ip_addr_isany(&(netdev->ip_addr)))
        {
            sal_check_netdev_internet_up(netdev);
        }
#endif /* RT_USING_SAL */

        /* execute netmask address change callback function */
        if (netdev->addr_callback)
        {
            netdev->addr_callback(netdev, NETDEV_CB_ADDR_NETMASK);
        }
    }
}

/**
 * This function will set network interface device gateway address.
 * @NOTE it can only be called in the network interface device driver.
 *
 * @param netdev the network interface device to change
 * @param gateway the new gateway address
 */
void netdev_low_level_set_gw(struct netdev *netdev, const ip_addr_t *gw)
{
    RT_ASSERT(gw);

    if (netdev && ip_addr_cmp(&(netdev->gw), gw) == 0)
    {
        ip_addr_copy(netdev->gw, *gw);

#ifdef RT_USING_SAL
        /* set network interface device flags to internet up */
        if (netdev_is_up(netdev) && netdev_is_link_up(netdev) &&
                !ip_addr_isany(&(netdev->ip_addr)))
        {
            sal_check_netdev_internet_up(netdev);
        }
#endif /* RT_USING_SAL */

        /* execute gateway address change callback function */
        if (netdev->addr_callback)
        {
            netdev->addr_callback(netdev, NETDEV_CB_ADDR_GATEWAY);
        }

#ifdef NETDEV_USING_AUTO_DEFAULT
        netdev_set_default_auto(netdev);
#endif
    }
}

RT_WEAK void netdev_update_resolv_conf(void)
{

}

/**
 * This function will set network interface device DNS server address.
 * @NOTE it can only be called in the network interface device driver.
 *
 * @param netdev the network interface device to change
 * @param dns_server the new DNS server address
 *
 */
void netdev_low_level_set_dns_server(struct netdev *netdev, uint8_t dns_num, const ip_addr_t *dns_server)
{
    RT_ASSERT(dns_server);

    if (netdev == RT_NULL || dns_num >= NETDEV_DNS_SERVERS_NUM)
    {
        return;
    }
    if (ip_addr_cmp(&netdev->dns_servers[dns_num], dns_server))
    {
        return;
    }

    ip_addr_copy(netdev->dns_servers[dns_num], *dns_server);

    /* execute DNS servers address change callback function */
    if (netdev->addr_callback)
    {
        netdev->addr_callback(netdev, NETDEV_CB_ADDR_DNS_SERVER);
    }

    netdev_update_resolv_conf();
}

#ifdef NETDEV_USING_AUTO_DEFAULT
/* Change to the first link_up network interface device automatically */
static void netdev_auto_change_default(struct netdev *netdev)
{
    netdev_set_default_auto(netdev);
}
#endif /* NETDEV_USING_AUTO_DEFAULT */

/**
 * This function will set network interface device status.
 * @NOTE it can only be called in the network interface device driver.
 *
 * @param netdev the network interface device to change
 * @param is_up the new status
 */
void netdev_low_level_set_status(struct netdev *netdev, rt_bool_t is_up)
{
    if (netdev && netdev_is_up(netdev) != is_up)
    {
        if (is_up)
        {
            netdev->flags |= NETDEV_FLAG_UP;
        }
        else
        {
            netdev->flags &= ~NETDEV_FLAG_UP;
        }

#ifdef NETDEV_USING_AUTO_DEFAULT
        netdev_auto_change_default(netdev);
#endif /* NETDEV_USING_AUTO_DEFAULT */

        /* execute  network interface device status change callback function */
        if (netdev->status_callback)
        {
            netdev->status_callback(netdev, is_up ? NETDEV_CB_STATUS_UP : NETDEV_CB_STATUS_DOWN);
        }
    }
}

/**
 * This function will set network interface device active link status.
 * @NOTE it can only be called in the network interface device driver.
 *
 * @param netdev the network interface device to change
 * @param is_up the new link status
 */
void netdev_low_level_set_link_status(struct netdev *netdev, rt_bool_t is_up)
{
    if (netdev && netdev_is_link_up(netdev) != is_up)
    {
        if (is_up)
        {
            netdev->flags |= NETDEV_FLAG_LINK_UP;

#ifdef RT_USING_SAL
            /* set network interface device flags to internet up */
            if (netdev_is_up(netdev) && !ip_addr_isany(&(netdev->ip_addr)))
            {
                sal_check_netdev_internet_up(netdev);
            }
#endif /* RT_USING_SAL */
        }
        else
        {
            netdev->flags &= ~NETDEV_FLAG_LINK_UP;

            /* set network interface device flags to internet down */
            netdev->flags &= ~NETDEV_FLAG_INTERNET_UP;
        }

#ifdef NETDEV_USING_AUTO_DEFAULT
            /* change to the first link_up network interface device automatically */
            netdev_auto_change_default(netdev);
#endif /* NETDEV_USING_AUTO_DEFAULT */

        /* execute link status change callback function */
        if (netdev->status_callback)
        {
            netdev->status_callback(netdev, is_up ? NETDEV_CB_STATUS_LINK_UP : NETDEV_CB_STATUS_LINK_DOWN);
        }
    }
}

/**
 * This function will set network interface device DHCP status.
 * @NOTE it can only be called in the network interface device driver.
 *
 * @param netdev the network interface device to change
 * @param is_up the new DHCP status
 */
void netdev_low_level_set_dhcp_status(struct netdev *netdev, rt_bool_t is_enable)
{
    if (netdev && netdev_is_dhcp_enabled(netdev) != is_enable)
    {
        if (is_enable)
        {
            netdev->flags |= NETDEV_FLAG_DHCP;
        }
        else
        {
            netdev->flags &= ~NETDEV_FLAG_DHCP;
        }

        /* execute DHCP status change callback function */
        if (netdev->status_callback)
        {
            netdev->status_callback(netdev, is_enable ? NETDEV_CB_STATUS_DHCP_ENABLE : NETDEV_CB_STATUS_DHCP_DISABLE);
        }
    }
}

#ifdef RT_USING_FINSH

#include <finsh.h>

#ifdef NETDEV_USING_IFCONFIG
static void netdev_list_if(void)
{
#define NETDEV_IFCONFIG_MAC_MAX_LEN    6
#define NETDEV_IFCONFIG_IEMI_MAX_LEN   8

    rt_ubase_t index;
    rt_slist_t *node  = RT_NULL;
    struct netdev *netdev = RT_NULL;
    struct netdev *cur_netdev_list = netdev_list;

    if (cur_netdev_list == RT_NULL)
    {
        rt_kprintf("ifconfig: network interface device list error.\n");
        return;
    }

    for (node = &(cur_netdev_list->list); node; node = rt_slist_next(node))
    {
        netdev = rt_list_entry(node, struct netdev, list);

        rt_kprintf("network interface device: %.*s%s\n",
                   RT_NAME_MAX, netdev->name,
                   (netdev == netdev_default) ? " (Default)" : "");
        rt_kprintf("MTU: %d\n", netdev->mtu);
        rt_kprintf("Metric: %u\n", netdev->metric);

        /* 6 - MAC address, 8 - IEMI */
        if (netdev->hwaddr_len == NETDEV_IFCONFIG_MAC_MAX_LEN)
        {
            rt_kprintf("MAC: ");
            for (index = 0; index < netdev->hwaddr_len; index++)
            {
                rt_kprintf("%02x ", netdev->hwaddr[index]);
            }
        }
        else if (netdev->hwaddr_len == NETDEV_IFCONFIG_IEMI_MAX_LEN)
        {
            rt_kprintf("IMEI: ");
            for (index = 0; index < netdev->hwaddr_len; index++)
            {
                /* two numbers are displayed at one time*/
                if (netdev->hwaddr[index] < 10 && index != netdev->hwaddr_len - 1)
                    rt_kprintf("0");

                rt_kprintf("%d", netdev->hwaddr[index]);
            }
        }

        rt_kprintf("\nFLAGS:");
        if (netdev->flags & NETDEV_FLAG_UP) rt_kprintf(" UP");
        else rt_kprintf(" DOWN");
        if (netdev->flags & NETDEV_FLAG_LINK_UP) rt_kprintf(" LINK_UP");
        else rt_kprintf(" LINK_DOWN");
        if (netdev->flags & NETDEV_FLAG_INTERNET_UP) rt_kprintf(" INTERNET_UP");
        else rt_kprintf(" INTERNET_DOWN");
        if (netdev->flags & NETDEV_FLAG_DHCP) rt_kprintf(" DHCP_ENABLE");
        else rt_kprintf(" DHCP_DISABLE");
        if (netdev->flags & NETDEV_FLAG_ETHARP) rt_kprintf(" ETHARP");
        if (netdev->flags & NETDEV_FLAG_BROADCAST) rt_kprintf(" BROADCAST");
        if (netdev->flags & NETDEV_FLAG_IGMP) rt_kprintf(" IGMP");
        rt_kprintf("\n");
        rt_kprintf("ip address: %s\n", inet_ntoa(netdev->ip_addr));
        rt_kprintf("gw address: %s\n", inet_ntoa(netdev->gw));
        rt_kprintf("net mask  : %s\n", inet_ntoa(netdev->netmask));

#if NETDEV_IPV6
        {
            ip_addr_t *addr;
            int i;

            addr = &netdev->ip6_addr[0];

            if (!ip_addr_isany(addr))
            {
                rt_kprintf("ipv6 link-local: %s %s\n", inet_ntoa(*addr),
                        !ip_addr_isany(addr) ? "VALID" : "INVALID");

                for (i = 1; i < NETDEV_IPV6_NUM_ADDRESSES; i++)
                {
                    addr = &netdev->ip6_addr[i];
                    rt_kprintf("ipv6[%d] address: %s %s\n", i, inet_ntoa(*addr),
                            !ip_addr_isany(addr) ? "VALID" : "INVALID");
                }
            }
        }
#endif /* NETDEV_IPV6 */

        for (index = 0; index < NETDEV_DNS_SERVERS_NUM; index++)
        {
            rt_kprintf("dns server #%d: %s\n", index, inet_ntoa(netdev->dns_servers[index]));
        }

        if (rt_slist_next(node))
        {
            rt_kprintf("\n");
        }
    }
}

#ifdef RT_LWIP_DHCP
int netdev_dhcp_open(char* netdev_name)
{
    struct netdev *netdev = RT_NULL;
    netdev = netdev_get_by_name(netdev_name);
    if (netdev == RT_NULL)
    {
        rt_kprintf("bad network interface device name(%s).\n", netdev_name);
        return -1;
    }    
    netdev_dhcp_enabled(netdev,RT_TRUE);
    return 0;
}

int netdev_dhcp_close(char* netdev_name)
{
    struct netdev *netdev = RT_NULL;

    netdev = netdev_get_by_name(netdev_name);
    if (netdev == RT_NULL)
    {
        rt_kprintf("bad network interface device name(%s).\n", netdev_name);
        return -1;
    }
    netdev_dhcp_enabled(netdev,RT_FALSE);
    return 0;
}
#endif

static void netdev_set_if(char* netdev_name, char* ip_addr, char* gw_addr, char* nm_addr)
{
    struct netdev *netdev = RT_NULL;
    ip_addr_t addr;

    netdev = netdev_get_by_name(netdev_name);
    if (netdev == RT_NULL)
    {
        rt_kprintf("bad network interface device name(%s).\n", netdev_name);
        return;
    }

#ifdef RT_LWIP_DHCP
    netdev_dhcp_close(netdev_name);
#endif

    /* set IP address */
    if ((ip_addr != RT_NULL) && inet_aton(ip_addr, &addr))
    {
        netdev_set_ipaddr(netdev, &addr);
    }

    /* set gateway address */
    if ((gw_addr != RT_NULL) && inet_aton(gw_addr, &addr))
    {
        netdev_set_gw(netdev, &addr);
    }

    /* set netmask address */
    if ((nm_addr != RT_NULL) && inet_aton(nm_addr, &addr))
    {
        netdev_set_netmask(netdev, &addr);
    }
}

int netdev_ifconfig(int argc, char **argv)
{
    if (argc == 1)
    {
        netdev_list_if();
    }
#ifdef RT_LWIP_DHCP    
    else if(argc == 3)
    {
        if (!strcmp(argv[2], "dhcp"))
        {
            netdev_dhcp_open(argv[1]);    
        }
    }
#endif    
    else if (argc == 5)
    {
        rt_kprintf("config : %s\n", argv[1]);
        rt_kprintf("IP addr: %s\n", argv[2]);
        rt_kprintf("Gateway: %s\n", argv[3]);
        rt_kprintf("netmask: %s\n", argv[4]);
        netdev_set_if(argv[1], argv[2], argv[3], argv[4]);
    }
    else
    {
        rt_kprintf("bad parameter! e.g: ifconfig e0 192.168.1.30 192.168.1.1 255.255.255.0\n");
#ifdef RT_LWIP_DHCP        
        rt_kprintf("bad parameter! e.g: ifconfig e0 dhcp\n");
#endif        
    }

    return 0;
}
FINSH_FUNCTION_EXPORT_ALIAS(netdev_ifconfig, __cmd_ifconfig, list the information of all network interfaces);
#endif /* NETDEV_USING_IFCONFIG */

#ifdef NETDEV_USING_PING
int netdev_cmd_ping(char* target_name, rt_uint32_t times, rt_size_t size, const char *dev_name)
{
#define NETDEV_PING_DATA_SIZE       32
/** ping receive timeout - in milliseconds */
#define NETDEV_PING_RECV_TIMEO      (2 * RT_TICK_PER_SECOND)
/** ping delay - in milliseconds */
#define NETDEV_PING_DELAY           (1 * RT_TICK_PER_SECOND)
/* check netdev ping options */
#define NETDEV_PING_IS_COMMONICABLE(netdev) \
    ((netdev) && (netdev)->ops && (netdev)->ops->ping && \
        netdev_is_up(netdev) && netdev_is_link_up(netdev)) \

    struct netdev *netdev_temp = NULL;
    struct netdev_ping_resp ping_resp;
    int index, ret = 0;

    int netdev_pool_cnt = 0;
    char net_dev_name_pool[16][RT_NAME_MAX];
    const int netdev_pool_max_cnt = sizeof(net_dev_name_pool) / sizeof(net_dev_name_pool[0]);

    if (size == 0) {
        size = NETDEV_PING_DATA_SIZE;
    }

    for(int i = 0; i < netdev_pool_cnt; i++) {
        rt_memset(&net_dev_name_pool[i][0], 0x00, RT_NAME_MAX);
    }

    if(NULL == dev_name) {
        rt_base_t level;
        rt_slist_t *node = RT_NULL;

        level = rt_hw_interrupt_disable();

        for (node = &(netdev_list->list); node; node = rt_slist_next(node))
        {
            if (RT_NULL == (netdev_temp = rt_slist_entry(node, struct netdev, list)))
            {
                rt_kprintf("ping: not found available network interface device.\n");
                continue;
            }
            else if (netdev_temp->ops == RT_NULL || netdev_temp->ops->ping == RT_NULL)
            {
                rt_kprintf("ping: network interface device(%s) not support ping feature.\n", netdev_temp->name);
                continue;
            }
            else if (!netdev_is_up(netdev_temp) || !netdev_is_link_up(netdev_temp))
            {
                rt_kprintf("ping: network interface device(%s) status error.\n", netdev_temp->name);
                continue;
            }

            strncpy(net_dev_name_pool[netdev_pool_cnt++], netdev_temp->name, sizeof(netdev_temp->name));
            if(netdev_pool_cnt >= netdev_pool_max_cnt) {
                break;
            }
        }

        rt_hw_interrupt_enable(level);
    } else {
        strncpy(net_dev_name_pool[netdev_pool_cnt++], dev_name, RT_NAME_MAX);
    }

    for(int i = 0; i < netdev_pool_cnt; i++) {
        char *dev_name = net_dev_name_pool[i];

        if(0x00 == dev_name[0]) {
            continue;
        }

        if (RT_NULL == (netdev_temp = netdev_get_by_name(dev_name))) {
            rt_kprintf("ping: not found available network interface device.\n");
            continue;
        } else if (netdev_temp->ops == RT_NULL || netdev_temp->ops->ping == RT_NULL) {
            rt_kprintf("ping: network interface device(%s) not support ping feature.\n", netdev_temp->name);
            continue;
        } else if (!netdev_is_up(netdev_temp) || !netdev_is_link_up(netdev_temp)) {
            rt_kprintf("ping: network interface device(%s) status error.\n", netdev_temp->name);
            continue;
        }

        rt_kprintf("ping with device %s\n", dev_name);

        for (index = 0; index < times; index++) {
            int delay_tick = 0;
            rt_tick_t start_tick = 0;

            rt_memset(&ping_resp, 0x00, sizeof(struct netdev_ping_resp));

            if (netdev_temp == RT_NULL || !netdev_is_up(netdev_temp) || !netdev_is_link_up(netdev_temp)) {
                rt_kprintf("ping: not found available network interface device.\n");
                break;
            }

            start_tick = rt_tick_get();

            ret = netdev_temp->ops->ping(netdev_temp, (const char *)target_name, size, NETDEV_PING_RECV_TIMEO, &ping_resp);
            if (ret == -RT_ETIMEOUT) {
                rt_kprintf("ping: from %s icmp_seq=%d timeout\n",
                    (ip_addr_isany(&(ping_resp.ip_addr))) ? target_name : inet_ntoa(ping_resp.ip_addr), index);
            } else if (ret == -RT_ERROR) {
                rt_kprintf("ping: unknown %s %s\n",
                    (ip_addr_isany(&(ping_resp.ip_addr))) ? "host" : "address",
                        (ip_addr_isany(&(ping_resp.ip_addr))) ? target_name : inet_ntoa(ping_resp.ip_addr));
            } else {
                if (ping_resp.ttl == 0) {
                    rt_kprintf("%d bytes from %s icmp_seq=%d time=%d ms\n",
                                ping_resp.data_len, inet_ntoa(ping_resp.ip_addr), index, ping_resp.ticks);
                } else {
                    rt_kprintf("%d bytes from %s icmp_seq=%d ttl=%d time=%d ms\n",
                                ping_resp.data_len, inet_ntoa(ping_resp.ip_addr), index, ping_resp.ttl, ping_resp.ticks);
                }
            }

            /* if the response time is more than NETDEV_PING_DELAY, no need to delay */
            delay_tick = ((rt_tick_get() - start_tick) > NETDEV_PING_DELAY) || (index == times) ? 0 : NETDEV_PING_DELAY;
            rt_thread_delay(delay_tick);
        }
    }

    return RT_EOK;
}

int netdev_ping(int argc, char **argv)
{
    int count = 4;
    char *dev_name = NULL;

    if (2 > argc) {
        rt_kprintf("Please input: ping <host address> [count] [dev]\n");
        return 0;
    }

    if (3 <= argc) {
        count = atoi(argv[2]);
    }

    if(4 <= argc) {
        dev_name = argv[3];
    }

    netdev_cmd_ping(argv[1], count, 0, dev_name);

    return 0;
}
FINSH_FUNCTION_EXPORT_ALIAS(netdev_ping, __cmd_ping, ping network host);
#endif /* NETDEV_USING_IFCONFIG */

static void netdev_list_dns(void)
{
    int index = 0;
    struct netdev *netdev = RT_NULL;
    rt_slist_t *node  = RT_NULL;

    for (node = &(netdev_list->list); node; node = rt_slist_next(node))
    {
        netdev = rt_list_entry(node, struct netdev, list);

        rt_kprintf("network interface device: %.*s%s\n",
                RT_NAME_MAX, netdev->name,
                (netdev == netdev_default)?" (Default)":"");

        for(index = 0; index < NETDEV_DNS_SERVERS_NUM; index++)
        {
            rt_kprintf("dns server #%d: %s\n", index, inet_ntoa(netdev->dns_servers[index]));
        }

        if (rt_slist_next(node))
        {
            rt_kprintf("\n");
        }
    }
}

static void netdev_set_dns(char *netdev_name, uint8_t dns_num, char *dns_server)
{
    struct netdev *netdev = RT_NULL;
    ip_addr_t dns_addr;

    netdev = netdev_get_by_name(netdev_name);
    if (netdev == RT_NULL)
    {
        rt_kprintf("bad network interface device name(%s).\n", netdev_name);
        return;
    }

    inet_aton(dns_server, &dns_addr);
    if (netdev_set_dns_server(netdev, dns_num, &dns_addr) == RT_EOK)
    {
        rt_kprintf("set network interface device(%s) dns server #%d: %s\n", netdev_name, dns_num, dns_server);
    }
}

int netdev_dns(int argc, char **argv)
{
    if (argc == 1)
    {
        netdev_list_dns();
    }
    else if (argc == 3)
    {
        netdev_set_dns(argv[1], 0, argv[2]);
    }
    else if (argc == 4)
    {
        netdev_set_dns(argv[1], atoi(argv[2]), argv[3]);
    }
    else
    {
        rt_kprintf("bad parameter! input: dns <netdev_name> [dns_num] <dns_server>\n");
        return -1;
    }

    return 0;
}
FINSH_FUNCTION_EXPORT_ALIAS(netdev_dns, __cmd_dns, list and set the information of dns);

#ifdef NETDEV_USING_NETSTAT
static void netdev_cmd_netstat(void)
{
    rt_slist_t *node  = RT_NULL;
    struct netdev *netdev = RT_NULL;
    struct netdev *cur_netdev_list = netdev_list;

    if (cur_netdev_list == RT_NULL)
    {
        rt_kprintf("netstat: network interface device list error.\n");
        return;
    }

    for (node = &(cur_netdev_list->list); node; node = rt_slist_next(node))
    {
        netdev = rt_list_entry(node, struct netdev, list);

        if (netdev && netdev->ops && netdev->ops->netstat)
        {
            break;
        }
    }

    if (netdev->ops->netstat != RT_NULL)
    {
        netdev->ops->netstat(netdev);
    }
    else
    {
        rt_kprintf("netstat: this command is not supported!\n");
    }
}

int netdev_netstat(int argc, char **argv)
{
    if (argc != 1)
    {
        rt_kprintf("Please input: netstat \n");
    }
    else
    {
        netdev_cmd_netstat();
    }

    return 0;
}
FINSH_FUNCTION_EXPORT_ALIAS(netdev_netstat, __cmd_netstat, list the information of TCP / IP);
#endif /* NETDEV_USING_NETSTAT */

#endif /* RT_USING_FINSH */

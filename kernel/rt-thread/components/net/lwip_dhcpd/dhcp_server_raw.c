/*
 * File      : dhcp_server_raw.c
 *             A simple DHCP server implementation
 * COPYRIGHT (C) 2011-2018, Shanghai Real-Thread Technology Co., Ltd
 * http://www.rt-thread.com
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 * 
 * Change Logs:
 * Date           Author       Notes
 * 2014-04-01     Ren.Haibo    the first version
 * 2018-06-12     aozima       ignore DHCP_OPTION_SERVER_ID.
 */

#include <stdio.h>
#include <stdint.h>

#include <rtthread.h>

#include <lwip/opt.h>
#include <lwip/sockets.h>
#include <lwip/sys.h>
#include <lwip/inet_chksum.h>
#include <netif/etharp.h>
#include <netif/ethernetif.h>
#include <lwip/ip.h>
#include <lwip/init.h>
#include <lwip/dns.h>
#include "lwip/inet.h"

#if (LWIP_VERSION) < 0x02000000U
    #error "not support old LWIP"
#endif

#if !LWIP_IPV4
    #error "must enable IPV4"
#endif

#if (LWIP_VERSION) >= 0x02000000U
    #include <lwip/prot/dhcp.h>
#endif

/* DHCP server option */

/* allocated client ip range */
#ifndef DHCPD_CLIENT_IP_MIN
    #define DHCPD_CLIENT_IP_MIN     2
#endif
#ifndef DHCPD_CLIENT_IP_MAX
    #define DHCPD_CLIENT_IP_MAX     254
#endif

/* the DHCP server address */
#ifndef DHCPD_SERVER_IP
    #define DHCPD_SERVER_IP "192.168.169.1"
#endif

#define DHCP_DEBUG_PRINTF

#ifdef  DHCP_DEBUG_PRINTF
    #define DEBUG_PRINTF        rt_kprintf("[DHCP] "); rt_kprintf
#else
    #define DEBUG_PRINTF(...)
#endif /* DHCP_DEBUG_PRINTF */

/* we need some routines in the DHCP of lwIP */
#undef  LWIP_DHCP
#define LWIP_DHCP   1
#include <lwip/dhcp.h>

/** Mac address length  */
#define DHCP_MAX_HLEN               6
/** dhcp default live time */
#define DHCP_DEFAULT_LIVE_TIME      86400U

/** Minimum length for request before packet is parsed */
#define DHCP_MIN_REQUEST_LEN        (DHCP_OPTIONS_OFS + 3U)

#define LWIP_NETIF_LOCK(...)
#define LWIP_NETIF_UNLOCK(...)

static void dhcpd_get_dns_server(const struct netif *netif,
                                 ip4_addr_t *dns_addr)
{
#ifdef DHCP_DNS_SERVER_IP
    if (ip4addr_aton(DHCP_DNS_SERVER_IP, dns_addr))
    {
        return;
    }
#else
    const ip_addr_t *server = dns_getserver(0);

    if (server != NULL && IP_IS_V4(server) &&
        !ip4_addr_isany(ip_2_ip4(server)) &&
        !ip4_addr_cmp(ip_2_ip4(server), netif_ip4_addr(netif)))
    {
        ip4_addr_copy(*dns_addr, *ip_2_ip4(server));
        return;
    }
#endif

    ip4_addr_copy(*dns_addr, *netif_ip4_addr(netif));
}

#ifndef DHCP_SERVER_PORT
#define DHCP_SERVER_PORT 67
#endif

/**
* The dhcp client node struct.
*/
struct dhcp_client_node
{
    struct dhcp_client_node *next;
    u8_t chaddr[DHCP_MAX_HLEN];
    ip4_addr_t ipaddr;
    u32_t lease_deadline_ms;
};

/**
* The dhcp server struct.
*/
struct dhcp_server
{
    struct dhcp_server *next;
    struct netif *netif;
    struct udp_pcb *pcb;
    struct dhcp_client_node *node_list;
    ip4_addr_t start;
    ip4_addr_t end;
    ip4_addr_t current;
};

static u8_t *dhcp_server_option_find(u8_t *buf, u16_t len, u8_t option);

static void dhcpd_set_netif_addr(struct netif *netif, const char *ip_addr,
                                 const char *gw_addr, const char *nm_addr)
{
    ip4_addr_t addr;

    if (ip_addr != RT_NULL && ip4addr_aton(ip_addr, &addr))
    {
        netif_set_ipaddr(netif, &addr);
    }
    if (gw_addr != RT_NULL && ip4addr_aton(gw_addr, &addr))
    {
        netif_set_gw(netif, &addr);
    }
    if (nm_addr != RT_NULL && ip4addr_aton(nm_addr, &addr))
    {
        netif_set_netmask(netif, &addr);
    }
}

/**
* The dhcp server struct list.
*/
static struct dhcp_server *lw_dhcp_server;

static u32_t dhcp_client_lease_deadline(void)
{
    return sys_now() + DHCP_DEFAULT_LIVE_TIME * 1000U;
}

static void dhcp_client_expire(struct dhcp_server *dhcpserver)
{
    struct dhcp_client_node **link = &dhcpserver->node_list;
    u32_t now = sys_now();

    while (*link != NULL)
    {
        struct dhcp_client_node *node = *link;

        if ((s32_t)(now - node->lease_deadline_ms) >= 0)
        {
            *link = node->next;
            mem_free(node);
        }
        else
        {
            link = &node->next;
        }
    }
}

/**
* Find a dhcp client node by mac address
*
* @param dhcpserver The dhcp server
* @param chaddr Mac address
* @param hlen   Mac address length
* @return dhcp client node
*/
static struct dhcp_client_node *
dhcp_client_find_by_mac(struct dhcp_server *dhcpserver, const u8_t *chaddr, u8_t hlen)
{
    struct dhcp_client_node *node;

    for (node = dhcpserver->node_list; node != NULL; node = node->next)
    {
        if (memcmp(node->chaddr, chaddr, hlen) == 0)
        {
            return node;
        }
    }

    return NULL;
}

/**
* Find a dhcp client node by ip address
*
* @param dhcpserver The dhcp server
* @param chaddr Mac address
* @param hlen   Mac address length
* @return dhcp client node
*/
static struct dhcp_client_node *
dhcp_client_find_by_ip(struct dhcp_server *dhcpserver, const ip4_addr_t *ip)
{
    struct dhcp_client_node *node;

    for (node = dhcpserver->node_list; node != NULL; node = node->next)
    {
        if (ip4_addr_cmp(&node->ipaddr, ip))
        {
            return node;
        }
    }

    return NULL;
}

/**
* Find a dhcp client node by ip address
*
* @param dhcpserver The dhcp server
* @param chaddr Mac address
* @param hlen   Mac address length
* @return dhcp client node
*/
static struct dhcp_client_node *
dhcp_client_find(struct dhcp_server *dhcpserver, struct dhcp_msg *msg,
                 u8_t *opt_buf, u16_t len)
{
    LWIP_UNUSED_ARG(opt_buf);
    LWIP_UNUSED_ARG(len);

    /*
     * A lease belongs to a client MAC, not to whichever client requests its
     * address. Returning an IP match here allowed a second client to reuse
     * another station's lease.
     */
    return dhcp_client_find_by_mac(dhcpserver, msg->chaddr, msg->hlen);
}

/**
* Find a dhcp client node by ip address
*
* @param dhcpserver The dhcp server
* @param chaddr Mac address
* @param hlen   Mac address length
* @return dhcp client node
*/
static struct dhcp_client_node *
dhcp_client_alloc(struct dhcp_server *dhcpserver, struct dhcp_msg *msg,
                  u8_t *opt_buf, u16_t len)
{
    ip4_addr_t first;
    ip4_addr_t requested;
    u8_t *opt;
    u32_t ipaddr;
    struct dhcp_client_node *node;

    node = dhcp_client_find_by_mac(dhcpserver, msg->chaddr, msg->hlen);
    if (node != NULL)
    {
        return node;
    }

    /*
     * Honor a requested address only when it is inside our pool and unowned.
     * In particular, never return the lease node of a different MAC.
     */
    opt = dhcp_server_option_find(opt_buf, len, DHCP_OPTION_REQUESTED_IP);
    if (opt != NULL && opt[1] == sizeof(requested.addr))
    {
        SMEMCPY(&requested.addr, &opt[2], sizeof(requested.addr));
        if (ntohl(requested.addr) >= ntohl(dhcpserver->start.addr) &&
            ntohl(requested.addr) <= ntohl(dhcpserver->end.addr) &&
            dhcp_client_find_by_ip(dhcpserver, &requested) == NULL)
        {
            dhcpserver->current = requested;
        }
    }

    first = dhcpserver->current;
    while (dhcp_client_find_by_ip(dhcpserver, &dhcpserver->current) != NULL)
    {
        ipaddr = ntohl(dhcpserver->current.addr) + 1U;
        if (ipaddr > ntohl(dhcpserver->end.addr))
        {
            ipaddr = ntohl(dhcpserver->start.addr);
        }
        dhcpserver->current.addr = htonl(ipaddr);
        if (ip4_addr_cmp(&dhcpserver->current, &first))
        {
            return NULL;
        }
    }

    node = (struct dhcp_client_node *)mem_malloc(sizeof(*node));
    if (node == NULL)
    {
        return NULL;
    }
    memset(node, 0, sizeof(*node));
    SMEMCPY(node->chaddr, msg->chaddr, DHCP_MAX_HLEN);
    node->ipaddr = dhcpserver->current;
    node->lease_deadline_ms = dhcp_client_lease_deadline();
    node->next = dhcpserver->node_list;
    dhcpserver->node_list = node;

    ipaddr = ntohl(dhcpserver->current.addr) + 1U;
    if (ipaddr > ntohl(dhcpserver->end.addr))
    {
        ipaddr = ntohl(dhcpserver->start.addr);
    }
    dhcpserver->current.addr = htonl(ipaddr);

    return node;
}

/**
* find option from buffer.
*
* @param buf The buffer to find option
* @param len The buffer length
* @param option Which option to find
* @return dhcp option buffer
*/
static u8_t *
dhcp_server_option_find(u8_t *buf, u16_t len, u8_t option)
{
    u8_t *end = buf + len;

    while (buf < end && *buf != DHCP_OPTION_END)
    {
        u8_t option_len;

        if (*buf == DHCP_OPTION_PAD)
        {
            buf++;
            continue;
        }
        if ((size_t)(end - buf) < 2U)
        {
            break;
        }
        option_len = buf[1];
        if ((size_t)(end - buf - 2) < option_len)
        {
            break;
        }
        if (*buf == option)
        {
            return buf;
        }
        buf += option_len + 2U;
    }
    return NULL;
}

/**
* If an incoming DHCP message is in response to us, then trigger the state machine
*/
static void
dhcp_server_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *recv_addr, u16_t port)
{
    struct dhcp_server *dhcp_server = (struct dhcp_server *)arg;
    char netif_name[RT_NAME_MAX];
    struct dhcp_msg *msg;
    struct pbuf *q;
    u8_t *opt_buf;
    u8_t *opt;
    struct dhcp_client_node *node;
    u8_t msg_type;
    u16_t length;
    u16_t request_len;
    ip_addr_t addr = *recv_addr;
    u32_t tmp;

    LWIP_DEBUGF(DHCP_DEBUG | LWIP_DBG_TRACE,
                ("[%s:%d] %s recv %d\n", __FUNCTION__, __LINE__,
                 rt_lwip_netif_name(dhcp_server->netif, netif_name,
                                    sizeof(netif_name)),
                 p->tot_len));
    LWIP_UNUSED_ARG(netif_name);
    /* prevent warnings about unused arguments */
    LWIP_UNUSED_ARG(pcb);
    LWIP_UNUSED_ARG(addr);
    LWIP_UNUSED_ARG(port);

    request_len = p->tot_len;
    if (request_len < DHCP_MIN_REQUEST_LEN)
    {
        LWIP_DEBUGF(DHCP_DEBUG | LWIP_DBG_TRACE | LWIP_DBG_LEVEL_WARNING, ("DHCP request message or pbuf too short\n"));
        pbuf_free(p);
        return;
    }

    q = pbuf_alloc(PBUF_TRANSPORT, 1500, PBUF_RAM);
    if (q == NULL)
    {
        LWIP_DEBUGF(DHCP_DEBUG | LWIP_DBG_TRACE | LWIP_DBG_LEVEL_WARNING, ("pbuf_alloc dhcp_msg failed!\n"));
        pbuf_free(p);
        return;
    }
    if (q->tot_len < p->tot_len)
    {
        LWIP_DEBUGF(DHCP_DEBUG | LWIP_DBG_TRACE | LWIP_DBG_LEVEL_WARNING, ("pbuf_alloc dhcp_msg too small %d:%d\n", q->tot_len, p->tot_len));
        pbuf_free(q);
        pbuf_free(p);
        return;
    }

    pbuf_copy(q, p);
    pbuf_free(p);

    msg = (struct dhcp_msg *)q->payload;
    if (msg->op != DHCP_BOOTREQUEST)
    {
        LWIP_DEBUGF(DHCP_DEBUG | LWIP_DBG_TRACE | LWIP_DBG_LEVEL_WARNING, ("not a DHCP request message, but type %"U16_F"\n", (u16_t)msg->op));
        goto free_pbuf_and_return;
    }

    if (msg->cookie != PP_HTONL(DHCP_MAGIC_COOKIE))
    {
        LWIP_DEBUGF(DHCP_DEBUG | LWIP_DBG_TRACE | LWIP_DBG_LEVEL_WARNING, ("bad DHCP_MAGIC_COOKIE!\n"));
        goto free_pbuf_and_return;
    }

    if (msg->htype != 1U || msg->hlen != DHCP_MAX_HLEN)
    {
        goto free_pbuf_and_return;
    }

    dhcp_client_expire(dhcp_server);
    opt_buf = (u8_t *)msg + DHCP_OPTIONS_OFS;
    length = request_len - DHCP_OPTIONS_OFS;
    opt = dhcp_server_option_find(opt_buf, length, DHCP_OPTION_MESSAGE_TYPE);
    if (opt != NULL && opt[1] == 1U)
    {
        msg_type = *(opt + 2);
        if (msg_type == DHCP_DISCOVER)
        {
            node = dhcp_client_alloc(dhcp_server, msg, opt_buf, length);
            if (node == NULL)
            {
                goto free_pbuf_and_return;
            }
            node->lease_deadline_ms = dhcp_client_lease_deadline();
            /* create dhcp offer and send */
            msg->op = DHCP_BOOTREPLY;
            msg->hops = 0;
            msg->secs = 0;
            SMEMCPY(&msg->siaddr, &(dhcp_server->netif->ip_addr), 4);
            msg->sname[0] = '\0';
            msg->file[0] = '\0';
            msg->cookie = PP_HTONL(DHCP_MAGIC_COOKIE);
            SMEMCPY(&msg->yiaddr, &node->ipaddr, 4);

            opt_buf = (u8_t *)msg + DHCP_OPTIONS_OFS;
            /* add msg type */
            *opt_buf++ = DHCP_OPTION_MESSAGE_TYPE;
            *opt_buf++ = 1;
            *opt_buf++ = DHCP_OFFER;

            /* add server id */
            *opt_buf++ = DHCP_OPTION_SERVER_ID;
            *opt_buf++ = 4;
            SMEMCPY(opt_buf, &(dhcp_server->netif->ip_addr), 4);
            opt_buf += 4;

            /* add_lease_time */
            *opt_buf++ = DHCP_OPTION_LEASE_TIME;
            *opt_buf++ = 4;
            tmp = PP_HTONL(DHCP_DEFAULT_LIVE_TIME);
            SMEMCPY(opt_buf, &tmp, 4);
            opt_buf += 4;

            /* add config */
            *opt_buf++ = DHCP_OPTION_SUBNET_MASK;
            *opt_buf++ = 4;
            SMEMCPY(opt_buf, &ip_2_ip4(&dhcp_server->netif->netmask)->addr, 4);
            opt_buf += 4;

            *opt_buf++ = DHCP_OPTION_DNS_SERVER;
            *opt_buf++ = 4;
            {
                ip4_addr_t dns_addr;

                dhcpd_get_dns_server(dhcp_server->netif, &dns_addr);
                SMEMCPY(opt_buf, &dns_addr.addr, 4);
            }
            opt_buf += 4;

            *opt_buf++ = DHCP_OPTION_ROUTER;
            *opt_buf++ = 4;
            SMEMCPY(opt_buf, &ip_2_ip4(&dhcp_server->netif->ip_addr)->addr, 4);
            opt_buf += 4;

            /* add option end */
            *opt_buf++ = DHCP_OPTION_END;

            length = (u16_t)(opt_buf - (u8_t *)msg);
            if (length < q->tot_len)
            {
                pbuf_realloc(q, length);
            }

            ip_2_ip4(&addr)->addr = INADDR_BROADCAST;
            udp_sendto_if(pcb, q, &addr, port, dhcp_server->netif);
        }
        else
        {
            if (1)
            {
                if (msg_type == DHCP_REQUEST)
                {
                    opt = dhcp_server_option_find(
                        opt_buf, length, DHCP_OPTION_SERVER_ID);
                    if (opt != NULL &&
                        (opt[1] != sizeof(ip4_addr_t) ||
                         memcmp(&opt[2], netif_ip4_addr(dhcp_server->netif),
                                sizeof(ip4_addr_t)) != 0))
                    {
                        goto free_pbuf_and_return;
                    }

                    node = dhcp_client_find(dhcp_server, msg, opt_buf, length);
                    opt = dhcp_server_option_find(
                        opt_buf, length, DHCP_OPTION_REQUESTED_IP);
                    if (node != NULL && opt != NULL &&
                        (opt[1] != sizeof(node->ipaddr.addr) ||
                         memcmp(&opt[2], &node->ipaddr.addr,
                                sizeof(node->ipaddr.addr)) != 0))
                    {
                        node = NULL;
                    }
                    if (node != NULL)
                    {
                        /* Send ack */
                        node->lease_deadline_ms = dhcp_client_lease_deadline();
                        /* create dhcp offer and send */
                        msg->op = DHCP_BOOTREPLY;
                        msg->hops = 0;
                        msg->secs = 0;
                        SMEMCPY(&msg->siaddr, &(dhcp_server->netif->ip_addr), 4);
                        msg->sname[0] = '\0';
                        msg->file[0] = '\0';
                        msg->cookie = PP_HTONL(DHCP_MAGIC_COOKIE);
                        SMEMCPY(&msg->yiaddr, &node->ipaddr, 4);
                        opt_buf = (u8_t *)msg + DHCP_OPTIONS_OFS;

                        /* add msg type */
                        *opt_buf++ = DHCP_OPTION_MESSAGE_TYPE;
                        *opt_buf++ = 1;
                        *opt_buf++ = DHCP_ACK;

                        /* add server id */
                        *opt_buf++ = DHCP_OPTION_SERVER_ID;
                        *opt_buf++ = 4;
                        SMEMCPY(opt_buf, &(dhcp_server->netif->ip_addr), 4);
                        opt_buf += 4;

                        /* add_lease_time */
                        *opt_buf++ = DHCP_OPTION_LEASE_TIME;
                        *opt_buf++ = 4;
                        tmp = PP_HTONL(DHCP_DEFAULT_LIVE_TIME);
                        SMEMCPY(opt_buf, &tmp, 4);
                        opt_buf += 4;

                        /* add config */
                        *opt_buf++ = DHCP_OPTION_SUBNET_MASK;
                        *opt_buf++ = 4;
                        SMEMCPY(opt_buf, &ip_2_ip4(&dhcp_server->netif->netmask)->addr, 4);
                        opt_buf += 4;

                        *opt_buf++ = DHCP_OPTION_DNS_SERVER;
                        *opt_buf++ = 4;
                        {
                            ip4_addr_t dns_addr;

                            dhcpd_get_dns_server(dhcp_server->netif, &dns_addr);
                            SMEMCPY(opt_buf, &dns_addr.addr, 4);
                        }
                        opt_buf += 4;

                        *opt_buf++ = DHCP_OPTION_ROUTER;
                        *opt_buf++ = 4;
                        SMEMCPY(opt_buf, &ip_2_ip4(&dhcp_server->netif->ip_addr)->addr, 4);
                        opt_buf += 4;

                        /* add option end */
                        *opt_buf++ = DHCP_OPTION_END;

                        length = (u16_t)(opt_buf - (u8_t *)msg);
                        if (length < q->tot_len)
                        {
                            pbuf_realloc(q, length);
                        }

                        ip_2_ip4(&addr)->addr = INADDR_BROADCAST;
                        udp_sendto_if(pcb, q, &addr, port, dhcp_server->netif);
                    }
                    else
                    {
                        /* Send no ack */
                        /* create dhcp offer and send */
                        msg->op = DHCP_BOOTREPLY;
                        msg->hops = 0;
                        msg->secs = 0;
                        SMEMCPY(&msg->siaddr, &(dhcp_server->netif->ip_addr), 4);
                        msg->sname[0] = '\0';
                        msg->file[0] = '\0';
                        msg->cookie = PP_HTONL(DHCP_MAGIC_COOKIE);
                        memset(&msg->yiaddr, 0, 4);
                        opt_buf = (u8_t *)msg + DHCP_OPTIONS_OFS;

                        /* add msg type */
                        *opt_buf++ = DHCP_OPTION_MESSAGE_TYPE;
                        *opt_buf++ = 1;
                        *opt_buf++ = DHCP_NAK;

                        /* add server id */
                        *opt_buf++ = DHCP_OPTION_SERVER_ID;
                        *opt_buf++ = 4;
                        SMEMCPY(opt_buf, &(dhcp_server->netif->ip_addr), 4);
                        opt_buf += 4;

                        /* add option end */
                        *opt_buf++ = DHCP_OPTION_END;
                        length = (u16_t)(opt_buf - (u8_t *)msg);
                        if (length < q->tot_len)
                        {
                            pbuf_realloc(q, length);
                        }

                        ip_2_ip4(&addr)->addr = INADDR_BROADCAST;
                        udp_sendto_if(pcb, q, &addr, port, dhcp_server->netif);
                    }
                }
                else if (msg_type == DHCP_RELEASE)
                {
                    struct dhcp_client_node *node_prev = NULL;

                    for (node = dhcp_server->node_list; node != NULL; node = node->next)
                    {
                        if (memcmp(node->chaddr, msg->chaddr, msg->hlen) == 0)
                        {
                            if (node == dhcp_server->node_list)
                            {
                                dhcp_server->node_list = node->next;
                            }
                            else
                            {
                                node_prev->next = node->next;
                            }
                            break;
                        }
                        node_prev = node;
                        node = node->next;
                    }

                    if (node != NULL)
                    {
                        mem_free(node);
                    }
                }
                else if (msg_type ==  DHCP_DECLINE)
                {
                    ;
                }
                else if (msg_type == DHCP_INFORM)
                {
                    ;
                }
            }
        }
    }

free_pbuf_and_return:
    pbuf_free(q);
}

/**
* start dhcp server for a netif
*
* @param netif The netif which use dhcp server
* @param start The Start IP address
* @param end The netif which use dhcp server
* @return lwIP error code
* - ERR_OK - No error
* - ERR_MEM - Out of memory
*/
err_t
dhcp_server_start(struct netif *netif, ip4_addr_t *start, ip4_addr_t *end)
{
    struct dhcp_server *dhcp_server;

    /* If this netif alreday use the dhcp server. */
    for (dhcp_server = lw_dhcp_server; dhcp_server != NULL; dhcp_server = dhcp_server->next)
    {
        if (dhcp_server->netif == netif)
        {
            dhcp_server->start = *start;
            dhcp_server->end = *end;
            dhcp_server->current = *start;
            return ERR_OK;
        }
    }

    dhcp_server = NULL;
    LWIP_DEBUGF(DHCP_DEBUG | LWIP_DBG_TRACE, ("dhcp_server_start(): starting new DHCP server\n"));
    dhcp_server = (struct dhcp_server *)mem_malloc(sizeof(struct dhcp_server));
    if (dhcp_server == NULL)
    {
        LWIP_DEBUGF(DHCP_DEBUG | LWIP_DBG_TRACE, ("dhcp_server_start(): could not allocate dhcp\n"));
        return ERR_MEM;
    }

    /* clear data structure */
    memset(dhcp_server, 0, sizeof(struct dhcp_server));

    /* store this dhcp server to list */
    dhcp_server->next = lw_dhcp_server;
    lw_dhcp_server = dhcp_server;
    dhcp_server->netif = netif;
    dhcp_server->node_list = NULL;
    dhcp_server->start = *start;
    dhcp_server->end = *end;
    dhcp_server->current = *start;

    /* allocate UDP PCB */
    dhcp_server->pcb = udp_new();
    if (dhcp_server->pcb == NULL)
    {
        LWIP_DEBUGF(DHCP_DEBUG  | LWIP_DBG_TRACE, ("dhcp_server_start(): could not obtain pcb\n"));
        return ERR_MEM;
    }

    ip_set_option(dhcp_server->pcb, SOF_BROADCAST);
    /* set up local and remote port for the pcb */
    udp_bind(dhcp_server->pcb, IP_ADDR_ANY, DHCP_SERVER_PORT);
    //udp_connect(dhcp_server->pcb, IP_ADDR_ANY, DHCP_CLIENT_PORT);
    /* set up the recv callback and argument */
    udp_recv(dhcp_server->pcb, dhcp_server_recv, dhcp_server);
    LWIP_DEBUGF(DHCP_DEBUG | LWIP_DBG_TRACE, ("dhcp_server_start(): starting DHCP server\n"));

    return ERR_OK;
}

void dhcpd_start(const char *netif_name)
{
    struct netif *netif;
    err_t res;

    DEBUG_PRINTF("%s: %s\r\n", __FUNCTION__, netif_name);

    LWIP_NETIF_LOCK();
    netif = rt_lwip_netif_find(netif_name);
    if (netif == RT_NULL)
    {
        DEBUG_PRINTF("network interface: %s not found!\r\n", netif_name);
        goto _exit;
    }

    if (1)
    {
        dhcp_stop(netif);

        dhcpd_set_netif_addr(netif, DHCPD_SERVER_IP, DHCPD_SERVER_IP,
                             "255.255.255.0");

        ip_addr_t dns_ip;
        ip4_addr_t dns_addr;

        dhcpd_get_dns_server(netif, &dns_addr);
        ip_addr_copy_from_ip4(dns_ip, dns_addr);
#if LWIP_VERSION >= 0x02020000U
        dns_setserver_for_netif(netif, 0, &dns_ip);
#elif LWIP_VERSION >= 0x02010000U
        dns_setserver(netif, 0, &dns_ip);
#else
        dns_setserver(0, &dns_ip);
#endif

        netif_set_up(netif);
    }

    {
        char str_tmp[4 * 4 + 4] = DHCPD_SERVER_IP;
        char *p = str_tmp;
        ip4_addr_t ip_start, ip_end;

        p = strchr(str_tmp, '.');
        if (p)
        {
            p = strchr(p + 1, '.');
            if (p)
            {
                p = strchr(p + 1, '.');
            }
        }
        if (!p)
        {
            DEBUG_PRINTF("DHCPD_SERVER_IP: %s error!\r\n", str_tmp);
            goto _exit;
        }
        p = p + 1; /* move to xxx.xxx.xxx.^ */

        sprintf(p, "%d", DHCPD_CLIENT_IP_MIN);
        ip4addr_aton(str_tmp, &ip_start);
        DEBUG_PRINTF("ip_start: [%s]\r\n", str_tmp);
        sprintf(p, "%d", DHCPD_CLIENT_IP_MAX);
        ip4addr_aton(str_tmp, &ip_end);
        DEBUG_PRINTF("ip_start: [%s]\r\n", str_tmp);

        res = dhcp_server_start(netif, &ip_start, &ip_end);
        if (res != 0)
        {
            DEBUG_PRINTF("dhcp_server_start res: %s.\r\n", res);
        }
    }

_exit:
    LWIP_NETIF_UNLOCK();
    return;
}

void dhcpd_stop(const char *netif_name)
{
    struct dhcp_server *dhcp_server, *server_node;
    struct netif *netif;
    struct dhcp_client_node *node, *next;

    DEBUG_PRINTF("%s: %s\r\n", __FUNCTION__, netif_name);

    LWIP_NETIF_LOCK();
    netif = rt_lwip_netif_find(netif_name);
    if (netif == RT_NULL)
    {
        DEBUG_PRINTF("network interface: %s not found!\r\n", netif_name);
        goto _exit;
    }

    /* If this netif alreday use the dhcp server. */
    for (dhcp_server = lw_dhcp_server; dhcp_server != NULL; dhcp_server = dhcp_server->next)
    {
        if (dhcp_server->netif == netif)
        {
            break;
        }
    }
    if (dhcp_server == RT_NULL)
    {
        goto _exit;
    }

    /* remove dhcp server */
    if (dhcp_server == lw_dhcp_server)
    {
        lw_dhcp_server = lw_dhcp_server->next;
    }
    else
    {
        server_node = lw_dhcp_server;
        while (server_node->next && server_node->next != dhcp_server)
        {
            server_node = server_node->next;
        }
        if (server_node->next != RT_NULL)
        {
            server_node->next = server_node->next->next;
        }
    }

    udp_disconnect(dhcp_server->pcb);
    udp_remove(dhcp_server->pcb);

    /* remove all client node */
    for (node = dhcp_server->node_list; node != NULL; node = next)
    {
        next = node->next;
        mem_free(node);
    }

    mem_free(dhcp_server);
    dhcpd_set_netif_addr(netif, "0.0.0.0", "0.0.0.0", "0.0.0.0");

_exit:
    LWIP_NETIF_UNLOCK();
}

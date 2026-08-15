/*
 * IPv4 network address and port translation for lwIP.
 *
 * Copyright (c) 2009 Christian Walter, Embedded Solutions, Vienna 2009.
 * Copyright (c) 2010 lwIP project.
 * Copyright (c) 2015-2026, RT-Thread Development Team.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ipv4_nat.h"

#if LWIP_IPV4 && defined(LWIP_USING_NAT)

#include <stddef.h>
#include <string.h>

#include "lwip/def.h"
#include "lwip/icmp.h"
#include "lwip/netifapi.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/sys.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"

#ifdef RT_USING_FINSH
#include <finsh.h>
#include <rtthread.h>
#include "netif/ethernetif.h"
#endif

#ifndef LWIP_NAT_TABLE_SIZE
#define LWIP_NAT_TABLE_SIZE             128
#endif
#ifndef LWIP_NAT_MAX_INTERFACES
#define LWIP_NAT_MAX_INTERFACES         2
#endif
#ifndef LWIP_NAT_PORT_MIN
#define LWIP_NAT_PORT_MIN               40000
#endif
#ifndef LWIP_NAT_PORT_MAX
#define LWIP_NAT_PORT_MAX               45000
#endif
#ifndef LWIP_NAT_TCP_TIMEOUT_SECONDS
#define LWIP_NAT_TCP_TIMEOUT_SECONDS    1800
#endif
#ifndef LWIP_NAT_TCP_CLOSE_SECONDS
#define LWIP_NAT_TCP_CLOSE_SECONDS      30
#endif
#ifndef LWIP_NAT_UDP_TIMEOUT_SECONDS
#define LWIP_NAT_UDP_TIMEOUT_SECONDS    120
#endif
#ifndef LWIP_NAT_ICMP_TIMEOUT_SECONDS
#define LWIP_NAT_ICMP_TIMEOUT_SECONDS   30
#endif

#if LWIP_NAT_TABLE_SIZE < 1
#error "LWIP_NAT_TABLE_SIZE must be at least 1"
#endif
#if LWIP_NAT_MAX_INTERFACES < 1
#error "LWIP_NAT_MAX_INTERFACES must be at least 1"
#endif
#if (LWIP_NAT_PORT_MIN < 1) || (LWIP_NAT_PORT_MIN > 65535) || \
    (LWIP_NAT_PORT_MAX < 1) || (LWIP_NAT_PORT_MAX > 65535) || \
    (LWIP_NAT_PORT_MIN > LWIP_NAT_PORT_MAX)
#error "invalid lwIP NAT translated port range"
#endif

#define NAT_ENTRY_IN_USE                0x01U
#define NAT_ENTRY_CLOSING               0x02U

struct ip_nat_state
{
    u32_t source_addr;
    u32_t dest_addr;
    u32_t mapped_addr;
    u32_t last_seen;
    u16_t source_port;
    u16_t dest_port;
    u16_t mapped_port;
    u8_t protocol;
    u8_t flags;
    struct netif *inside_if;
    struct netif *outside_if;
};

static struct ip_nat_state nat_table[LWIP_NAT_TABLE_SIZE];
static struct netif *nat_interfaces[LWIP_NAT_MAX_INTERFACES];
static u16_t nat_next_port = LWIP_NAT_PORT_MIN;

static int
ip_nat_interface_enabled(const struct netif *netif)
{
    size_t index;

    if (netif == NULL)
    {
        return 0;
    }

    for (index = 0; index < LWIP_NAT_MAX_INTERFACES; index++)
    {
        if (nat_interfaces[index] == netif)
        {
            return 1;
        }
    }
    return 0;
}

static int
ip_nat_any_interface_enabled(void)
{
    size_t index;

    for (index = 0; index < LWIP_NAT_MAX_INTERFACES; index++)
    {
        if (nat_interfaces[index] != NULL)
        {
            return 1;
        }
    }
    return 0;
}

static u32_t
ip_nat_timeout_ms(const struct ip_nat_state *state)
{
    switch (state->protocol)
    {
    case IP_PROTO_TCP:
        if ((state->flags & NAT_ENTRY_CLOSING) != 0U)
        {
            return LWIP_NAT_TCP_CLOSE_SECONDS * 1000UL;
        }
        return LWIP_NAT_TCP_TIMEOUT_SECONDS * 1000UL;
    case IP_PROTO_UDP:
        return LWIP_NAT_UDP_TIMEOUT_SECONDS * 1000UL;
    case IP_PROTO_ICMP:
        return LWIP_NAT_ICMP_TIMEOUT_SECONDS * 1000UL;
    default:
        return 0;
    }
}

static int
ip_nat_state_expired(const struct ip_nat_state *state, u32_t now)
{
    return ((state->flags & NAT_ENTRY_IN_USE) == 0U) ||
           ((u32_t)(now - state->last_seen) >= ip_nat_timeout_ms(state));
}

static void
ip_nat_expire(u32_t now)
{
    size_t index;

    for (index = 0; index < LWIP_NAT_TABLE_SIZE; index++)
    {
        if (ip_nat_state_expired(&nat_table[index], now))
        {
            nat_table[index].flags = 0;
        }
    }
}

static int
ip_nat_tcp_port_in_use(u16_t port)
{
#if LWIP_TCP
    struct tcp_pcb *pcb;
    struct tcp_pcb *lists[] =
    {
        tcp_bound_pcbs,
        tcp_listen_pcbs.pcbs,
        tcp_active_pcbs,
        tcp_tw_pcbs
    };
    size_t list_index;

    for (list_index = 0; list_index < LWIP_ARRAYSIZE(lists); list_index++)
    {
        for (pcb = lists[list_index]; pcb != NULL; pcb = pcb->next)
        {
            if (pcb->local_port == port)
            {
                return 1;
            }
        }
    }
#else
    LWIP_UNUSED_ARG(port);
#endif
    return 0;
}

static int
ip_nat_udp_port_in_use(u16_t port)
{
#if LWIP_UDP
    struct udp_pcb *pcb;

    for (pcb = udp_pcbs; pcb != NULL; pcb = pcb->next)
    {
        if (pcb->local_port == port)
        {
            return 1;
        }
    }
#else
    LWIP_UNUSED_ARG(port);
#endif
    return 0;
}

static int
ip_nat_mapped_port_in_use(u8_t protocol, u16_t port)
{
    size_t index;
    u16_t network_port = lwip_htons(port);

    for (index = 0; index < LWIP_NAT_TABLE_SIZE; index++)
    {
        if ((nat_table[index].flags & NAT_ENTRY_IN_USE) != 0U &&
            nat_table[index].protocol == protocol &&
            nat_table[index].mapped_port == network_port)
        {
            return 1;
        }
    }

    if (protocol == IP_PROTO_TCP)
    {
        return ip_nat_tcp_port_in_use(port);
    }
    if (protocol == IP_PROTO_UDP)
    {
        return ip_nat_udp_port_in_use(port);
    }
    return 0;
}

static u16_t
ip_nat_allocate_port(u8_t protocol)
{
    u32_t attempts;
    u32_t port_count;
    u16_t port;

    port_count = (u32_t)LWIP_NAT_PORT_MAX - LWIP_NAT_PORT_MIN + 1U;
    for (attempts = 0; attempts < port_count; attempts++)
    {
        port = nat_next_port;
        if (nat_next_port >= LWIP_NAT_PORT_MAX)
        {
            nat_next_port = LWIP_NAT_PORT_MIN;
        }
        else
        {
            nat_next_port++;
        }

        if (!ip_nat_mapped_port_in_use(protocol, port))
        {
            return lwip_htons(port);
        }
    }
    return 0;
}

static struct ip_nat_state *
ip_nat_allocate_state(u32_t now)
{
    struct ip_nat_state *oldest = NULL;
    u32_t oldest_age = 0;
    size_t index;

    ip_nat_expire(now);
    for (index = 0; index < LWIP_NAT_TABLE_SIZE; index++)
    {
        u32_t age;

        if ((nat_table[index].flags & NAT_ENTRY_IN_USE) == 0U)
        {
            memset(&nat_table[index], 0, sizeof(nat_table[index]));
            return &nat_table[index];
        }

        age = now - nat_table[index].last_seen;
        if (oldest == NULL || age > oldest_age)
        {
            oldest = &nat_table[index];
            oldest_age = age;
        }
    }

    if (oldest != NULL)
    {
        memset(oldest, 0, sizeof(*oldest));
    }
    return oldest;
}

static struct ip_nat_state *
ip_nat_find_outgoing(u8_t protocol, const struct ip_hdr *iphdr,
                     u16_t source_port, u16_t dest_port,
                     u32_t mapped_addr, struct netif *inside_if,
                     struct netif *outside_if)
{
    size_t index;

    for (index = 0; index < LWIP_NAT_TABLE_SIZE; index++)
    {
        struct ip_nat_state *state = &nat_table[index];

        if ((state->flags & NAT_ENTRY_IN_USE) != 0U &&
            state->protocol == protocol &&
            state->source_addr == iphdr->src.addr &&
            state->dest_addr == iphdr->dest.addr &&
            state->mapped_addr == mapped_addr &&
            state->source_port == source_port &&
            state->dest_port == dest_port &&
            state->inside_if == inside_if &&
            state->outside_if == outside_if)
        {
            return state;
        }
    }
    return NULL;
}

static struct ip_nat_state *
ip_nat_find_incoming(u8_t protocol, const struct ip_hdr *iphdr,
                     u16_t source_port, u16_t mapped_port,
                     struct netif *outside_if)
{
    size_t index;

    for (index = 0; index < LWIP_NAT_TABLE_SIZE; index++)
    {
        struct ip_nat_state *state = &nat_table[index];

        if ((state->flags & NAT_ENTRY_IN_USE) != 0U &&
            state->protocol == protocol &&
            state->dest_addr == iphdr->src.addr &&
            state->mapped_addr == iphdr->dest.addr &&
            state->dest_port == source_port &&
            state->mapped_port == mapped_port &&
            state->outside_if == outside_if)
        {
            return state;
        }
    }
    return NULL;
}

static struct ip_nat_state *
ip_nat_create_state(u8_t protocol, const struct ip_hdr *iphdr,
                    u16_t source_port, u16_t dest_port,
                    u32_t mapped_addr, struct netif *inside_if,
                    struct netif *outside_if, u32_t now)
{
    struct ip_nat_state *state;
    u16_t mapped_port;

    state = ip_nat_allocate_state(now);
    if (state == NULL)
    {
        return NULL;
    }

    mapped_port = ip_nat_allocate_port(protocol);
    if (mapped_port == 0)
    {
        return NULL;
    }

    state->source_addr = iphdr->src.addr;
    state->dest_addr = iphdr->dest.addr;
    state->mapped_addr = mapped_addr;
    state->last_seen = now;
    state->source_port = source_port;
    state->dest_port = dest_port;
    state->mapped_port = mapped_port;
    state->protocol = protocol;
    state->flags = NAT_ENTRY_IN_USE;
    state->inside_if = inside_if;
    state->outside_if = outside_if;
    return state;
}

static void
ip_nat_checksum_adjust(u8_t *checksum, const void *old_data,
                       u16_t old_length, const void *new_data,
                       u16_t new_length)
{
    const u8_t *old_bytes = (const u8_t *)old_data;
    const u8_t *new_bytes = (const u8_t *)new_data;
    s32_t value;

    value = (s32_t)((checksum[0] << 8) | checksum[1]);
    value = ~value & 0xffff;

    while (old_length != 0U)
    {
        value -= (s32_t)((old_bytes[0] << 8) | old_bytes[1]);
        if (value <= 0)
        {
            value--;
            value &= 0xffff;
        }
        old_bytes += 2;
        old_length -= 2;
    }

    while (new_length != 0U)
    {
        value += (s32_t)((new_bytes[0] << 8) | new_bytes[1]);
        if ((value & 0x10000) != 0)
        {
            value++;
            value &= 0xffff;
        }
        new_bytes += 2;
        new_length -= 2;
    }

    value = ~value & 0xffff;
    checksum[0] = (u8_t)(value >> 8);
    checksum[1] = (u8_t)value;
}

static void
ip_nat_modify_ip_addr(struct ip_hdr *iphdr, ip4_addr_p_t *field,
                      u32_t new_addr)
{
    u8_t *address = (u8_t *)field;

    ip_nat_checksum_adjust((u8_t *)&IPH_CHKSUM(iphdr), address, 4,
                           &new_addr, 4);
    memcpy(address, &new_addr, sizeof(new_addr));
}

#if LWIP_TCP
static void
ip_nat_modify_tcp_port(struct tcp_hdr *tcphdr, int destination, u16_t new_port)
{
    u8_t *port = (u8_t *)tcphdr +
        (destination ? offsetof(struct tcp_hdr, dest) :
                       offsetof(struct tcp_hdr, src));

    ip_nat_checksum_adjust((u8_t *)&tcphdr->chksum, port, 2, &new_port, 2);
    memcpy(port, &new_port, sizeof(new_port));
}

static void
ip_nat_modify_tcp_addr(struct tcp_hdr *tcphdr, u32_t old_addr, u32_t new_addr)
{
    ip_nat_checksum_adjust((u8_t *)&tcphdr->chksum, &old_addr, 4,
                           &new_addr, 4);
}
#endif

#if LWIP_UDP
static void
ip_nat_modify_udp_port(struct udp_hdr *udphdr, int destination, u16_t new_port)
{
    u8_t *port = (u8_t *)udphdr +
        (destination ? offsetof(struct udp_hdr, dest) :
                       offsetof(struct udp_hdr, src));

    if (udphdr->chksum != 0U)
    {
        ip_nat_checksum_adjust((u8_t *)&udphdr->chksum, port, 2,
                               &new_port, 2);
        if (udphdr->chksum == 0U)
        {
            udphdr->chksum = 0xffffU;
        }
    }
    memcpy(port, &new_port, sizeof(new_port));
}

static void
ip_nat_modify_udp_addr(struct udp_hdr *udphdr, u32_t old_addr, u32_t new_addr)
{
    if (udphdr->chksum != 0U)
    {
        ip_nat_checksum_adjust((u8_t *)&udphdr->chksum, &old_addr, 4,
                               &new_addr, 4);
        if (udphdr->chksum == 0U)
        {
            udphdr->chksum = 0xffffU;
        }
    }
}
#endif

static void *
ip_nat_transport_header(struct pbuf *p, const struct ip_hdr *iphdr,
                        u16_t header_size)
{
    u16_t ip_header_size = IPH_HL_BYTES(iphdr);

    if (ip_header_size < IP_HLEN ||
        p->len < (u16_t)(ip_header_size + header_size))
    {
        return NULL;
    }
    return (u8_t *)p->payload + ip_header_size;
}

static void
ip_nat_mark_tcp_state(struct ip_nat_state *state, const struct tcp_hdr *tcphdr)
{
    if ((TCPH_FLAGS(tcphdr) & (TCP_FIN | TCP_RST)) != 0U)
    {
        state->flags |= NAT_ENTRY_CLOSING;
    }
}

static err_t
ip_nat_enable_core(struct netif *netif)
{
    size_t index;

    LWIP_ASSERT_CORE_LOCKED();
    if (netif == NULL || ip_nat_interface_enabled(netif))
    {
        return netif == NULL ? ERR_ARG : ERR_OK;
    }

    for (index = 0; index < LWIP_NAT_MAX_INTERFACES; index++)
    {
        if (nat_interfaces[index] == NULL)
        {
            nat_interfaces[index] = netif;
            return ERR_OK;
        }
    }

    LWIP_DEBUGF(IP_DEBUG | LWIP_DBG_LEVEL_SERIOUS,
                ("ip_nat: no free interface slot\n"));
    return ERR_MEM;
}

static err_t
ip_nat_disable_core(struct netif *netif)
{
    size_t index;

    LWIP_ASSERT_CORE_LOCKED();
    for (index = 0; index < LWIP_NAT_MAX_INTERFACES; index++)
    {
        if (nat_interfaces[index] == netif)
        {
            nat_interfaces[index] = NULL;
        }
    }

    for (index = 0; index < LWIP_NAT_TABLE_SIZE; index++)
    {
        if (nat_table[index].inside_if == netif ||
            nat_table[index].outside_if == netif)
        {
            nat_table[index].flags = 0;
        }
    }
    return ERR_OK;
}

err_t
ip_nat_set_enabled(struct netif *netif, u8_t enabled)
{
    if (netif == NULL)
    {
        return ERR_ARG;
    }

    if (enabled != 0U)
    {
        return netifapi_netif_common(netif, NULL, ip_nat_enable_core);
    }
    return netifapi_netif_common(netif, NULL, ip_nat_disable_core);
}

struct netif *
ip_nat_input(struct pbuf *p, struct ip_hdr *iphdr, struct netif *inp)
{
    struct ip_nat_state *state = NULL;
    u32_t now;

    LWIP_ASSERT_CORE_LOCKED();
    if (!ip_nat_any_interface_enabled())
    {
        return NULL;
    }

    now = sys_now();
    ip_nat_expire(now);

    if ((lwip_ntohs(IPH_OFFSET(iphdr)) & (IP_OFFMASK | IP_MF)) != 0U)
    {
        return NULL;
    }

    switch (IPH_PROTO(iphdr))
    {
#if LWIP_TCP
    case IP_PROTO_TCP:
    {
        struct tcp_hdr *tcphdr = (struct tcp_hdr *)
            ip_nat_transport_header(p, iphdr, sizeof(struct tcp_hdr));
        if (tcphdr == NULL)
        {
            return NULL;
        }
        state = ip_nat_find_incoming(IP_PROTO_TCP, iphdr, tcphdr->src,
                                     tcphdr->dest, inp);
        if (state == NULL)
        {
            return NULL;
        }
        if (tcphdr->dest != state->source_port)
        {
            ip_nat_modify_tcp_port(tcphdr, 1, state->source_port);
        }
        ip_nat_modify_tcp_addr(tcphdr, iphdr->dest.addr, state->source_addr);
        ip_nat_mark_tcp_state(state, tcphdr);
        break;
    }
#endif
#if LWIP_UDP
    case IP_PROTO_UDP:
    {
        struct udp_hdr *udphdr = (struct udp_hdr *)
            ip_nat_transport_header(p, iphdr, sizeof(struct udp_hdr));
        if (udphdr == NULL)
        {
            return NULL;
        }
        state = ip_nat_find_incoming(IP_PROTO_UDP, iphdr, udphdr->src,
                                     udphdr->dest, inp);
        if (state == NULL)
        {
            return NULL;
        }
        if (udphdr->dest != state->source_port)
        {
            ip_nat_modify_udp_port(udphdr, 1, state->source_port);
        }
        ip_nat_modify_udp_addr(udphdr, iphdr->dest.addr, state->source_addr);
        break;
    }
#endif
#if LWIP_ICMP
    case IP_PROTO_ICMP:
    {
        struct icmp_echo_hdr *icmphdr = (struct icmp_echo_hdr *)
            ip_nat_transport_header(p, iphdr, sizeof(struct icmp_echo_hdr));
        if (icmphdr == NULL || ICMPH_TYPE(icmphdr) != ICMP_ER)
        {
            return NULL;
        }
        state = ip_nat_find_incoming(IP_PROTO_ICMP, iphdr, 0, icmphdr->id, inp);
        if (state == NULL)
        {
            return NULL;
        }
        if (icmphdr->id != state->source_port)
        {
            ip_nat_checksum_adjust((u8_t *)&icmphdr->chksum, &icmphdr->id, 2,
                                   &state->source_port, 2);
            icmphdr->id = state->source_port;
        }
        break;
    }
#endif
    default:
        return NULL;
    }

    ip_nat_modify_ip_addr(iphdr, &iphdr->dest, state->source_addr);
    state->last_seen = now;
    return state->inside_if;
}

err_t
ip_nat_forward(struct pbuf *p, struct ip_hdr *iphdr,
               struct netif *inp, struct netif *outp,
               struct netif *input_inside_if)
{
    struct ip_nat_state *state;
    u32_t external_addr;
    u32_t now = sys_now();

    LWIP_ASSERT_CORE_LOCKED();
    if (outp == NULL || outp == inp)
    {
        return ERR_RTE;
    }

    if (input_inside_if != NULL)
    {
        return (!ip_nat_interface_enabled(inp) &&
                outp == input_inside_if &&
                ip_nat_interface_enabled(input_inside_if)) ? ERR_OK : ERR_RTE;
    }

    if (!ip_nat_interface_enabled(inp) ||
        ip_nat_interface_enabled(outp) ||
        ip4_addr_isany_val(*netif_ip4_addr(outp)))
    {
        return ERR_RTE;
    }
    if ((lwip_ntohs(IPH_OFFSET(iphdr)) & (IP_OFFMASK | IP_MF)) != 0U)
    {
        return ERR_RTE;
    }

    external_addr = ip4_addr_get_u32(netif_ip4_addr(outp));
    switch (IPH_PROTO(iphdr))
    {
#if LWIP_TCP
    case IP_PROTO_TCP:
    {
        struct tcp_hdr *tcphdr = (struct tcp_hdr *)
            ip_nat_transport_header(p, iphdr, sizeof(struct tcp_hdr));
        if (tcphdr == NULL)
        {
            return ERR_BUF;
        }

        state = ip_nat_find_outgoing(IP_PROTO_TCP, iphdr, tcphdr->src,
                                     tcphdr->dest, external_addr, inp, outp);
        if (state == NULL)
        {
            if ((TCPH_FLAGS(tcphdr) & (TCP_SYN | TCP_ACK)) != TCP_SYN)
            {
                return ERR_RTE;
            }
            state = ip_nat_create_state(IP_PROTO_TCP, iphdr, tcphdr->src,
                                        tcphdr->dest, external_addr, inp, outp,
                                        now);
            if (state == NULL)
            {
                return ERR_MEM;
            }
        }
        if (tcphdr->src != state->mapped_port)
        {
            ip_nat_modify_tcp_port(tcphdr, 0, state->mapped_port);
        }
        ip_nat_modify_tcp_addr(tcphdr, iphdr->src.addr, external_addr);
        ip_nat_mark_tcp_state(state, tcphdr);
        break;
    }
#endif
#if LWIP_UDP
    case IP_PROTO_UDP:
    {
        struct udp_hdr *udphdr = (struct udp_hdr *)
            ip_nat_transport_header(p, iphdr, sizeof(struct udp_hdr));
        if (udphdr == NULL)
        {
            return ERR_BUF;
        }

        state = ip_nat_find_outgoing(IP_PROTO_UDP, iphdr, udphdr->src,
                                     udphdr->dest, external_addr, inp, outp);
        if (state == NULL)
        {
            state = ip_nat_create_state(IP_PROTO_UDP, iphdr, udphdr->src,
                                        udphdr->dest, external_addr, inp, outp,
                                        now);
            if (state == NULL)
            {
                return ERR_MEM;
            }
        }
        if (udphdr->src != state->mapped_port)
        {
            ip_nat_modify_udp_port(udphdr, 0, state->mapped_port);
        }
        ip_nat_modify_udp_addr(udphdr, iphdr->src.addr, external_addr);
        break;
    }
#endif
#if LWIP_ICMP
    case IP_PROTO_ICMP:
    {
        struct icmp_echo_hdr *icmphdr = (struct icmp_echo_hdr *)
            ip_nat_transport_header(p, iphdr, sizeof(struct icmp_echo_hdr));
        if (icmphdr == NULL || ICMPH_TYPE(icmphdr) != ICMP_ECHO)
        {
            return ERR_RTE;
        }

        state = ip_nat_find_outgoing(IP_PROTO_ICMP, iphdr, icmphdr->id, 0,
                                     external_addr, inp, outp);
        if (state == NULL)
        {
            state = ip_nat_create_state(IP_PROTO_ICMP, iphdr, icmphdr->id, 0,
                                        external_addr, inp, outp, now);
            if (state == NULL)
            {
                return ERR_MEM;
            }
        }
        if (icmphdr->id != state->mapped_port)
        {
            ip_nat_checksum_adjust((u8_t *)&icmphdr->chksum, &icmphdr->id, 2,
                                   &state->mapped_port, 2);
            icmphdr->id = state->mapped_port;
        }
        break;
    }
#endif
    default:
        return ERR_RTE;
    }

    ip_nat_modify_ip_addr(iphdr, &iphdr->src, external_addr);
    state->last_seen = now;
    return ERR_OK;
}

#ifdef RT_USING_FINSH
static err_t
ip_nat_show_core(struct netif *netif)
{
    size_t index;
    size_t enabled = 0;
    size_t tcp_sessions = 0;
    size_t udp_sessions = 0;
    size_t icmp_sessions = 0;
    char name[RT_NAME_MAX];

    LWIP_UNUSED_ARG(netif);
    LWIP_ASSERT_CORE_LOCKED();
    ip_nat_expire(sys_now());

    for (index = 0; index < LWIP_NAT_MAX_INTERFACES; index++)
    {
        if (nat_interfaces[index] != NULL)
        {
            rt_kprintf("internal interface: %s\n",
                       rt_lwip_netif_name(nat_interfaces[index], name,
                                          sizeof(name)));
            enabled++;
        }
    }

    for (index = 0; index < LWIP_NAT_TABLE_SIZE; index++)
    {
        if ((nat_table[index].flags & NAT_ENTRY_IN_USE) == 0U)
        {
            continue;
        }
        if (nat_table[index].protocol == IP_PROTO_TCP)
        {
            tcp_sessions++;
        }
        else if (nat_table[index].protocol == IP_PROTO_UDP)
        {
            udp_sessions++;
        }
        else if (nat_table[index].protocol == IP_PROTO_ICMP)
        {
            icmp_sessions++;
        }
    }

    rt_kprintf("NAT: %s, sessions: %u/%u (TCP %u, UDP %u, ICMP %u)\n",
               enabled != 0U ? "enabled" : "disabled",
               (unsigned int)(tcp_sessions + udp_sessions + icmp_sessions),
               (unsigned int)LWIP_NAT_TABLE_SIZE,
               (unsigned int)tcp_sessions, (unsigned int)udp_sessions,
               (unsigned int)icmp_sessions);
    return ERR_OK;
}

static int
nat(int argc, char **argv)
{
    struct netif *netif = netif_default;

    LWIP_UNUSED_ARG(argc);
    LWIP_UNUSED_ARG(argv);
    if (netif == NULL)
    {
        netif = netif_list;
    }
    if (netif == NULL)
    {
        rt_kprintf("NAT: network stack is not initialized\n");
        return -1;
    }
    return netifapi_netif_common(netif, NULL, ip_nat_show_core);
}
MSH_CMD_EXPORT(nat, show IPv4 NAT status and session counts);
#endif

#endif /* LWIP_IPV4 && LWIP_USING_NAT */

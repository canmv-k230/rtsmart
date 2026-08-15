/*
 * Copyright (c) 2009 Christian Walter, Embedded Solutions, Vienna 2009.
 * Copyright (c) 2010 lwIP project.
 * Copyright (c) 2015-2026, RT-Thread Development Team.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef IPV4_NAT_H
#define IPV4_NAT_H

#include "lwip/opt.h"

#if LWIP_IPV4 && defined(LWIP_USING_NAT)

#include "lwip/err.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/prot/ip4.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Thread-safe API for enabling NAT on an internal network interface. */
err_t ip_nat_set_enabled(struct netif *netif, u8_t enabled);

/* These functions are called by the IPv4 core and must run in its context. */
/* Returns the internal interface when an incoming reply was translated. */
struct netif *ip_nat_input(struct pbuf *p, struct ip_hdr *iphdr,
                           struct netif *inp);
err_t ip_nat_forward(struct pbuf *p, struct ip_hdr *iphdr,
                     struct netif *inp, struct netif *outp,
                     struct netif *input_inside_if);

#ifdef __cplusplus
}
#endif

#endif /* LWIP_IPV4 && LWIP_USING_NAT */

#endif /* IPV4_NAT_H */

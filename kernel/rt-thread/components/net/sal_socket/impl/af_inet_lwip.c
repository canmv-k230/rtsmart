/*
 * Copyright (c) 2006-2018, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2018-05-17     ChenYong     First version
 */

#include <rtthread.h>

#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <lwip/api.h>
#include <lwip/init.h>
#include <lwip/netif.h>

#ifdef SAL_USING_POSIX
#include <dfs_poll.h>
#endif

#include <sal.h>
#include <af_inet.h>

#include <netdev.h>

#if (LWIP_VERSION < 0x2000000) && NETDEV_IPV6
#error "The lwIP version is not support IPV6, please disable netdev IPV6 configuration "
#elif (LWIP_VERSION > 0x2000000) && (NETDEV_IPV6 != LWIP_IPV6)
#error "IPV6 configuration error, Please check and synchronize netdev and lwip IPV6 configuration."
#endif

#if LWIP_VERSION < 0x2000000
#define SELWAIT_T int
#else
#ifndef SELWAIT_T
#define SELWAIT_T u8_t
#endif
#endif

#ifdef SAL_USING_LWIP

#ifdef SAL_USING_POSIX

#if LWIP_VERSION >= 0x20100ff
#include <lwip/priv/sockets_priv.h>
#else /* LWIP_VERSION < 0x20100ff */
/*
 * Re-define lwip socket
 *
 * NOTE: please make sure the definitions same in lwip::net_socket.c
 */
struct lwip_sock {
    /** sockets currently are built on netconns, each socket has one netconn */
    struct netconn *conn;
    /** data that was left from the previous read */
    void *lastdata;
    /** offset in the data that was left from the previous read */
    u16_t lastoffset;
    /** number of times data was received, set by event_callback(),
     tested by the receive and select functions */
    s16_t rcvevent;
    /** number of times data was ACKed (free send buffer), set by event_callback(),
     tested by select */
    u16_t sendevent;
    /** error happened for this socket, set by event_callback(), tested by select */
    u16_t errevent;
   /** last error that occurred on this socket */
#if LWIP_VERSION < 0x2000000
   int err;
#else
   u8_t err;
#endif
    /** counter of how many threads are waiting for this socket using select */
    SELWAIT_T select_waiting;

    rt_wqueue_t wait_head;
};
#endif /* LWIP_VERSION >= 0x20100ff */

extern struct lwip_sock *lwip_tryget_socket(int s);

#if LWIP_VERSION >= 0x02020000
#define SAL_NETCONN_SOCKET(conn) ((conn)->callback_arg.socket)
#else
#define SAL_NETCONN_SOCKET(conn) ((conn)->socket)
#endif

static void event_callback(struct netconn *conn, enum netconn_evt evt, u16_t len)
{
    int s;
    struct lwip_sock *sock;
    uint32_t event = 0;
    SYS_ARCH_DECL_PROTECT(lev);

    LWIP_UNUSED_ARG(len);

    /* Get socket */
    if (conn)
    {
        s = SAL_NETCONN_SOCKET(conn);
        if (s < 0)
        {
            /* Data comes in right away after an accept, even though
             * the server task might not have created a new socket yet.
             * Just count down (or up) if that's the case and we
             * will use the data later. Note that only receive events
             * can happen before the new socket is set up. */
            SYS_ARCH_PROTECT(lev);
            if (SAL_NETCONN_SOCKET(conn) < 0)
            {
                if (evt == NETCONN_EVT_RCVPLUS)
                {
                    SAL_NETCONN_SOCKET(conn)--;
                }
                SYS_ARCH_UNPROTECT(lev);
                return;
            }
            s = SAL_NETCONN_SOCKET(conn);
            SYS_ARCH_UNPROTECT(lev);
        }

        sock = lwip_tryget_socket(s);
        if (!sock)
        {
            return;
        }
    }
    else
    {
        return;
    }

    SYS_ARCH_PROTECT(lev);
    /* Set event as required */
    switch (evt)
    {
    case NETCONN_EVT_RCVPLUS:
        sock->rcvevent++;
        break;
    case NETCONN_EVT_RCVMINUS:
        sock->rcvevent--;
        break;
    case NETCONN_EVT_SENDPLUS:
        sock->sendevent = 1;
        break;
    case NETCONN_EVT_SENDMINUS:
        sock->sendevent = 0;
        break;
    case NETCONN_EVT_ERROR:
        sock->errevent = 1;
        break;
    default:
        LWIP_ASSERT("unknown event", 0);
        break;
    }

#if LWIP_VERSION >= 0x20100ff
    if ((void*)(sock->lastdata.pbuf) || (sock->rcvevent > 0))
#else
    if ((void*)(sock->lastdata) || (sock->rcvevent > 0))
#endif
        event |= POLLIN;
    if (sock->sendevent)
        event |= POLLOUT;
    if (sock->errevent)
        event |= POLLERR;

    SYS_ARCH_UNPROTECT(lev);

    if (event)
    {
        rt_wqueue_wakeup(&sock->wait_head, (void*)(size_t)event);
    }
}
#endif /* SAL_USING_POSIX */

static int inet_socket(int domain, int type, int protocol)
{
#ifdef SAL_USING_POSIX
    int socket;

    socket = lwip_socket(domain, type, protocol);
    if (socket >= 0)
    {
        struct lwip_sock *lwsock;

        lwsock = lwip_tryget_socket(socket);
        lwsock->conn->callback = event_callback;

        rt_wqueue_init(&lwsock->wait_head);
    }

    return socket;
#else
    return lwip_socket(domain, type, protocol);
#endif /* SAL_USING_POSIX */
}

static int inet_accept(int socket, struct sockaddr *addr, socklen_t *addrlen)
{
#ifdef SAL_USING_POSIX
    int new_socket;

    new_socket = lwip_accept(socket, addr, addrlen);
    if (new_socket >= 0)
    {
        struct lwip_sock *lwsock;

        lwsock = lwip_tryget_socket(new_socket);

        rt_wqueue_init(&lwsock->wait_head);
    }

    return new_socket;
#else
    return lwip_accept(socket, addr, addrlen);
#endif /* SAL_USING_POSIX */
}

static int inet_getsockname(int socket, struct sockaddr *name, socklen_t *namelen)
{
#if LWIP_VERSION_MAJOR < 2U
    rt_kprintf("ERROR: Your lwIP version is not supported. Please using lwIP 2.0.0+.\n");
    RT_ASSERT(LWIP_VERSION_MAJOR >= 2U);
#endif

    return lwip_getsockname(socket, name, namelen);
}

#if LWIP_VERSION >= 0x02010000
static int inet_sendmsg(int socket, const struct sal_msghdr *message, int flags)
{
    struct msghdr lwip_message;
    struct iovec *lwip_iov;
    int i;
    int ret;

    lwip_iov = rt_malloc(sizeof(*lwip_iov) * message->msg_iovlen);
    if (lwip_iov == RT_NULL)
    {
        rt_set_errno(-ENOMEM);
        return -1;
    }

    for (i = 0; i < message->msg_iovlen; i++)
    {
        lwip_iov[i].iov_base = message->msg_iov[i].iov_base;
        lwip_iov[i].iov_len = message->msg_iov[i].iov_len;
    }

    lwip_message.msg_name = message->msg_name;
    lwip_message.msg_namelen = message->msg_namelen;
    lwip_message.msg_iov = lwip_iov;
    lwip_message.msg_iovlen = message->msg_iovlen;
    lwip_message.msg_control = message->msg_control;
    lwip_message.msg_controllen = message->msg_controllen;
    lwip_message.msg_flags = message->msg_flags;

    ret = (int)lwip_sendmsg(socket, &lwip_message, flags);
    rt_free(lwip_iov);
    return ret;
}

static int inet_recvmsg(int socket, struct sal_msghdr *message, int flags)
{
    struct msghdr lwip_message;
    struct iovec *lwip_iov;
    int i;
    int ret;

    lwip_iov = rt_malloc(sizeof(*lwip_iov) * message->msg_iovlen);
    if (lwip_iov == RT_NULL)
    {
        rt_set_errno(-ENOMEM);
        return -1;
    }

    for (i = 0; i < message->msg_iovlen; i++)
    {
        lwip_iov[i].iov_base = message->msg_iov[i].iov_base;
        lwip_iov[i].iov_len = message->msg_iov[i].iov_len;
    }

    lwip_message.msg_name = message->msg_name;
    lwip_message.msg_namelen = message->msg_namelen;
    lwip_message.msg_iov = lwip_iov;
    lwip_message.msg_iovlen = message->msg_iovlen;
    lwip_message.msg_control = message->msg_control;
    lwip_message.msg_controllen = message->msg_controllen;
    lwip_message.msg_flags = message->msg_flags;

    ret = (int)lwip_recvmsg(socket, &lwip_message, flags);
    message->msg_namelen = lwip_message.msg_namelen;
    message->msg_controllen = lwip_message.msg_controllen;
    message->msg_flags = lwip_message.msg_flags;
    rt_free(lwip_iov);
    return ret;
}
#endif /* LWIP_VERSION >= 2.1.0 */

int inet_ioctlsocket(int socket, long cmd, void *arg)
{
    switch (cmd)
    {
    case F_GETFL:
        return lwip_fcntl(socket, cmd, 0);

    case F_SETFL:
        /* musl adds O_LARGEFILE to F_SETFL. It is not a mutable socket
         * status flag and lwIP otherwise rejects it with ENOSYS. */
        return lwip_fcntl(socket, cmd,
                ((int)(size_t)arg & O_NONBLOCK) ? O_NONBLOCK : 0);

    default:
        return lwip_ioctl(socket, cmd, arg);
    }
}

#ifdef SAL_USING_POSIX
static int inet_poll(struct dfs_fd *file, struct rt_pollreq *req)
{
    int mask = 0;
    struct lwip_sock *sock;
    struct sal_socket *sal_sock;

    sal_sock = sal_get_socket((int)(size_t)file->fnode->data);
    if(!sal_sock)
    {
        return -1;
    }

    sock = lwip_tryget_socket((int)(size_t)sal_sock->user_data);
    if (sock != NULL)
    {
        rt_base_t level;

        rt_poll_add(&sock->wait_head, req);

        level = rt_hw_interrupt_disable();

#if LWIP_VERSION >= 0x20100ff
        if ((void*)(sock->lastdata.pbuf) || sock->rcvevent)
#else
        if ((void*)(sock->lastdata) || sock->rcvevent)
#endif
        {
            mask |= POLLIN;
        }
        if (sock->sendevent)
        {
            mask |= POLLOUT;
        }
        if (sock->errevent)
        {
            mask |= POLLERR;
            /* clean error event */
            sock->errevent = 0;
        }
        rt_hw_interrupt_enable(level);
    }

    return mask;
}
#endif

static const struct sal_socket_ops lwip_socket_ops =
{
    inet_socket,
    lwip_close,
    lwip_bind,
    lwip_listen,
    lwip_connect,
    inet_accept,
    (int (*)(int, const void *, size_t, int, const struct sockaddr *, socklen_t))lwip_sendto,
    (int (*)(int, void *, size_t, int, struct sockaddr *, socklen_t *))lwip_recvfrom,
#if LWIP_VERSION >= 0x02010000
    inet_sendmsg,
    inet_recvmsg,
#else
    RT_NULL,
    RT_NULL,
#endif
    lwip_getsockopt,
    //TODO fix on 1.4.1
    lwip_setsockopt,
    lwip_shutdown,
    lwip_getpeername,
    inet_getsockname,
    inet_ioctlsocket,
#ifdef SAL_USING_POSIX
    inet_poll,
#endif
};

static const struct sal_netdb_ops lwip_netdb_ops =
{
    lwip_gethostbyname,
    lwip_gethostbyname_r,
    lwip_getaddrinfo,
    lwip_freeaddrinfo,
};

static const struct sal_proto_family lwip_inet_family =
{
    AF_INET,
    AF_INET,
    &lwip_socket_ops,
    &lwip_netdb_ops,
};

/* Set lwIP network interface device protocol family information */
int sal_lwip_netdev_set_pf_info(struct netdev *netdev)
{
    RT_ASSERT(netdev);
    
    netdev->sal_user_data = (void *) &lwip_inet_family;
    return 0;
}

#endif /* SAL_USING_LWIP */

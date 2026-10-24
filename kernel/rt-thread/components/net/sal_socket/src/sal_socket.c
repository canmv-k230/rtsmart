/*
 * Copyright (c) 2006-2018, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2018-05-23     ChenYong     First version
 * 2018-11-12     ChenYong     Add TLS support
 */

#include <rtthread.h>
#include <rthw.h>
#include <fcntl.h>
#include <sys/time.h>

#include <sal_socket.h>
#include <sal_netdb.h>
#ifdef SAL_USING_LWIP
#include <lwip/init.h>
#endif
#ifdef SAL_USING_TLS
#include <sal_tls.h>
#endif
#include <sal.h>
#include <netdev.h>

#include <ipc/workqueue.h>

/* check system workqueue stack size */
#if RT_SYSTEM_WORKQUEUE_STACKSIZE < 1536
#error "The system workqueue stack size must more than 1536 bytes"
#endif

#define DBG_TAG                        "sal.skt"
#define DBG_LVL                        DBG_INFO
#include <rtdbg.h>

#define SOCKET_TABLE_STEP_LEN          4

/* the socket table used to dynamic allocate sockets */
struct sal_socket_table
{
    uint32_t max_socket;
    struct sal_socket **sockets;
};

#ifdef SAL_USING_TLS
/* The global TLS protocol options */
static struct sal_proto_tls *proto_tls;
#endif

/* The global socket table */
static struct sal_socket_table socket_table;
static struct rt_mutex sal_core_lock;
static rt_bool_t init_ok = RT_FALSE;

#define IS_SOCKET_PROTO_TLS(sock)                (((sock)->protocol == PROTOCOL_TLS) || \
                                                 ((sock)->protocol == PROTOCOL_DTLS))
#define SAL_SOCKOPS_PROTO_TLS_VALID(sock, name)  (proto_tls && (proto_tls->ops->name) && IS_SOCKET_PROTO_TLS(sock))

#define SAL_SOCKOPT_PROTO_TLS_EXEC(sock, name, optval, optlen)                    \
do {                                                                              \
    if (SAL_SOCKOPS_PROTO_TLS_VALID(sock, name)){                                 \
        return proto_tls->ops->name((sock)->user_data_tls, (optval), (optlen));   \
    }                                                                             \
}while(0)                                                                         \

#define SAL_SOCKET_OBJ_GET(sock, socket)                                          \
do {                                                                              \
    (sock) = sal_get_socket(socket);                                              \
    if ((sock) == RT_NULL) {                                                      \
        rt_set_errno(-EBADF);                                                     \
        return -1;                                                                \
    }                                                                             \
}while(0)                                                                         \

#define SAL_NETDEV_SOCKETOPS_VALID(netdev, pf, ops)                               \
do {                                                                              \
    (pf) = (netdev) ? (struct sal_proto_family *) (netdev)->sal_user_data : RT_NULL; \
    if ((pf) == RT_NULL || (pf)->skt_ops == RT_NULL || (pf)->skt_ops->ops == RT_NULL){ \
        rt_set_errno(-EOPNOTSUPP);                                                 \
        return -1;                                                                \
    }                                                                             \
}while(0)                                                                         \

#define SAL_SOCKETOPS_VALID(socket_obj, family_ops, operation)                    \
do {                                                                              \
    (family_ops) = (socket_obj)->pf;                                              \
    if ((family_ops) == RT_NULL || (family_ops)->skt_ops == RT_NULL ||            \
        (family_ops)->skt_ops->operation == RT_NULL){                             \
        rt_set_errno(-EOPNOTSUPP);                                                 \
        return -1;                                                                \
    }                                                                             \
}while(0)                                                                         \

#define SAL_NETDEV_NETDBOPS_VALID(netdev, pf, ops)                                \
    ((netdev) && netdev_is_up(netdev) &&                                          \
    ((pf) = (struct sal_proto_family *) (netdev)->sal_user_data) != RT_NULL &&    \
    (pf)->netdb_ops->ops)                                                         \

/**
 * SAL (Socket Abstraction Layer) initialize.
 *
 * @return result  0: initialize success
 *                -1: initialize failed
 */
int sal_init(void)
{
    int cn;

    if (init_ok)
    {
        LOG_D("Socket Abstraction Layer is already initialized.");
        return 0;
    }

    /* init sal socket table */
    cn = SOCKET_TABLE_STEP_LEN < SAL_SOCKETS_NUM ? SOCKET_TABLE_STEP_LEN : SAL_SOCKETS_NUM;
    socket_table.max_socket = cn;
    socket_table.sockets = rt_calloc(1, cn * sizeof(struct sal_socket *));
    if (socket_table.sockets == RT_NULL)
    {
        LOG_E("No memory for socket table.\n");
        return -1;
    }

    /* create sal socket lock */
    rt_mutex_init(&sal_core_lock, "sal_lock", RT_IPC_FLAG_FIFO);

#ifdef SAL_USING_LWIP
    LOG_I("Socket Abstraction Layer initialize success, lwIP %u.%u.%u.",
            (unsigned int)LWIP_VERSION_MAJOR,
            (unsigned int)LWIP_VERSION_MINOR,
            (unsigned int)LWIP_VERSION_REVISION);
#else
    LOG_I("Socket Abstraction Layer initialize success.");
#endif
    init_ok = RT_TRUE;

    return 0;
}
INIT_COMPONENT_EXPORT(sal_init);

#ifdef RT_USING_SAL
#include "ipc/waitqueue.h"

void update_work_count(struct netdev *netdev, int val)
{
    rt_base_t level;

    level = rt_hw_interrupt_disable();
    netdev->work_count += val;
    rt_hw_interrupt_enable(level);
}


rt_bool_t get_work_count(struct netdev *netdev)
{
    int cnt;
    rt_base_t level;

    level = rt_hw_interrupt_disable();
    cnt = netdev->work_count;
    rt_hw_interrupt_enable(level);

    return cnt;
}
#endif

/* check SAL network interface device internet status */
static void check_netdev_internet_up_work(struct rt_work *work, void *work_data)
{
#define SAL_INTERNET_VERSION   0x00
#define SAL_INTERNET_BUFF_LEN  12
#define SAL_INTERNET_TIMEOUT   (2)

#define SAL_INTERNET_HOST      "link.rt-thread.org"
#define SAL_INTERNET_PORT      8101

#define SAL_INTERNET_MONTH_LEN 4
#define SAL_INTERNET_DATE_LEN  16

    int index, sockfd = -1, result = 0;
    struct sockaddr_in server_addr;
    struct hostent *host;
    struct timeval timeout;
    struct netdev *netdev = (struct netdev *)work_data;
    socklen_t addr_len = sizeof(struct sockaddr_in);
    char send_data[SAL_INTERNET_BUFF_LEN], recv_data = 0;
    struct rt_delayed_work *delay_work = (struct rt_delayed_work *)work;

    const char month[][SAL_INTERNET_MONTH_LEN] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char date[SAL_INTERNET_DATE_LEN];
    int moth_num = 0;

    struct sal_proto_family *pf = (struct sal_proto_family *) netdev->sal_user_data;
    const struct sal_socket_ops *skt_ops;

    if (work)
    {
        rt_free(delay_work);
    }

    /* get network interface socket operations */
    if (pf == RT_NULL || pf->skt_ops == RT_NULL)
    {
        result = -RT_ERROR;
        goto __exit;
    }

#ifdef RT_USING_SAL
    rt_bool_t is_gone = RT_FALSE;

    if (netdev->is_gone) {
        is_gone = RT_TRUE;
    }
    update_work_count(netdev, -1);
    if (get_work_count(netdev) == 0) {
        rt_wqueue_wakeup(&netdev->wait_work, 0);
    }

    if (is_gone) {
        return;
    }
#endif

    host = (struct hostent *) pf->netdb_ops->gethostbyname(SAL_INTERNET_HOST);
    if (host == RT_NULL)
    {
        result = -RT_ERROR;
        goto __exit;
    }

    skt_ops = pf->skt_ops;
    if ((sockfd = skt_ops->socket(AF_INET, SOCK_DGRAM, 0)) < 0)
    {
        result = -RT_ERROR;
        goto __exit;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SAL_INTERNET_PORT);
    server_addr.sin_addr = *((struct in_addr *)host->h_addr);
    rt_memset(&(server_addr.sin_zero), 0, sizeof(server_addr.sin_zero));

    timeout.tv_sec = SAL_INTERNET_TIMEOUT;
    timeout.tv_usec = 0;

    /* set receive and send timeout */
    skt_ops->setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (void *) &timeout, sizeof(timeout));
    skt_ops->setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, (void *) &timeout, sizeof(timeout));

    /* get build moth value*/
    rt_memset(date, 0x00, SAL_INTERNET_DATE_LEN);
    rt_snprintf(date, SAL_INTERNET_DATE_LEN, "%s", __DATE__);

    for (index = 0; index < sizeof(month) / SAL_INTERNET_MONTH_LEN; index++)
    {
        if (rt_memcmp(date, month[index], SAL_INTERNET_MONTH_LEN - 1) == 0)
        {
            moth_num = index + 1;
            break;
        }
    }

    /* not find build month */
    if (moth_num == 0 || moth_num > sizeof(month) / SAL_INTERNET_MONTH_LEN)
    {
        result = -RT_ERROR;
        goto __exit;
    }

    rt_memset(send_data, 0x00, SAL_INTERNET_BUFF_LEN);
    send_data[0] = SAL_INTERNET_VERSION;
    for (index = 0; index < netdev->hwaddr_len; index++)
    {
        send_data[index + 1] = netdev->hwaddr[index] + moth_num;
    }
    send_data[9] = RT_VERSION;
    send_data[10] = RT_SUBVERSION;
    send_data[11] = RT_REVISION;

    skt_ops->sendto(sockfd, send_data, SAL_INTERNET_BUFF_LEN, 0,
                    (struct sockaddr *)&server_addr, sizeof(struct sockaddr));

    result = skt_ops->recvfrom(sockfd, &recv_data, sizeof(recv_data), 0, (struct sockaddr *)&server_addr, &addr_len);
    if (result < 0)
    {
        goto __exit;
    }

    if (recv_data == RT_FALSE)
    {
        result = -RT_ERROR;
        goto __exit;
    }

__exit:
    if (result > 0)
    {
        LOG_D("Set network interface device(%s) internet status up.", netdev->name);
        netdev->flags |= NETDEV_FLAG_INTERNET_UP;
    }
    else
    {
        LOG_D("Set network interface device(%s) internet status down.", netdev->name);
        netdev->flags &= ~NETDEV_FLAG_INTERNET_UP;
    }

    if (sockfd >= 0)
    {
        skt_ops->closesocket(sockfd);
    }
}

/**
 * This function will check SAL network interface device internet status.
 *
 * @param netdev the network interface device to check
 */
int sal_check_netdev_internet_up(struct netdev *netdev)
{
    /* workqueue for network connect */
    struct rt_delayed_work *net_work = RT_NULL;

    RT_ASSERT(netdev);

    net_work = (struct rt_delayed_work *)rt_calloc(1, sizeof(struct rt_delayed_work));
    if (net_work == RT_NULL)
    {
        LOG_W("No memory for network interface device(%s) delay work.", netdev->name);
        return -1;
    }

    rt_delayed_work_init(net_work, check_netdev_internet_up_work, (void *)netdev);
#ifndef RT_USING_SAL
    rt_work_submit(&(net_work->work), RT_TICK_PER_SECOND);
#else
    int ret;
    ret = rt_work_submit(&(net_work->work), RT_TICK_PER_SECOND);
    if (ret == RT_EOK) {
        update_work_count(netdev, 1);
    }
#endif

    return 0;
}

/**
 * This function will register TLS protocol to the global TLS protocol.
 *
 * @param pt TLS protocol object
 *
 * @return 0: TLS protocol object register success
 */
#ifdef SAL_USING_TLS
int sal_proto_tls_register(const struct sal_proto_tls *pt)
{
    RT_ASSERT(pt);
    proto_tls = (struct sal_proto_tls *) pt;

    return 0;
}
#endif

/**
 * This function will get sal socket object by sal socket descriptor.
 *
 * @param socket sal socket index
 *
 * @return sal socket object of the current sal socket index
 */
struct sal_socket *sal_get_socket(int socket)
{
    struct sal_socket_table *st = &socket_table;
    int idx = socket - SAL_SOCKET_OFFSET;

    if (idx < 0 || idx >= (int)st->max_socket || st->sockets == RT_NULL ||
        st->sockets[idx] == RT_NULL ||
        st->sockets[idx]->magic != SAL_SOCKET_MAGIC || st->sockets[idx]->closing)
    {
        return RT_NULL;
    }

    return st->sockets[idx];
}

/**
 * This function will lock sal socket.
 *
 * @note please don't invoke it on ISR.
 */
static void sal_lock(void)
{
    rt_err_t result;

    result = rt_mutex_take(&sal_core_lock, RT_WAITING_FOREVER);
    if (result != RT_EOK)
    {
        RT_ASSERT(0);
    }
}

/**
 * This function will lock sal socket.
 *
 * @note please don't invoke it on ISR.
 */
static void sal_unlock(void)
{
    rt_mutex_release(&sal_core_lock);
}

static void sal_socket_put(struct sal_socket *sock)
{
    sal_lock();
    RT_ASSERT(sock->io_users > 0);
    if (--sock->io_users == 0 && sock->closing)
        rt_sem_release(&sock->io_drained);
    sal_unlock();
}

static struct sal_socket *sal_socket_get(int socket)
{
    struct sal_socket *sock;

    sal_lock();
    sock = sal_get_socket(socket);
    if (sock)
        sock->io_users++;
    sal_unlock();
    if (!sock)
        rt_set_errno(-EBADF);
    return sock;
}

static int sal_socket_result(struct sal_socket *sock, int result)
{
    int error = rt_get_errno();
    sal_socket_put(sock);
    rt_set_errno(error);
    return result;
}

struct sal_rx_request
{
    void *mem;
    size_t len;
    int flags;
    struct sockaddr *addr;
    socklen_t *addrlen;
    struct sal_msghdr *message;
    rt_bool_t accept;
};

static rt_int32_t socket_rx_remaining(rt_tick_t started, rt_int32_t timeout)
{
    rt_tick_t elapsed = rt_tick_get() - started;
    return timeout < 0 ? RT_WAITING_FOREVER :
           elapsed < (rt_tick_t)timeout ? timeout - elapsed : 0;
}

/* Own the blocking wait here; backend attempts must never wait for packets.
 * rx_lock protects lwIP's lastdata and the listener's temporary nonblocking
 * mode, but is released while sleeping so another receiver can make progress. */
static int socket_receive(struct sal_socket *sock, struct sal_rx_request *request)
{
    const struct sal_socket_ops *ops = sock->pf->skt_ops;
    int socket = (int)(size_t)sock->user_data;
    rt_tick_t started = rt_tick_get();
    rt_int32_t timeout = RT_WAITING_FOREVER;
    int flags = request->flags;
    rt_bool_t first = RT_TRUE;

    if (ops->wait_readable && (!request->accept || ops->accept_nonblock))
    {
        struct timeval tv = {0};
        socklen_t length = sizeof(tv);
        if (ops->getsockopt && ops->getsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
                &tv, &length) == 0 && length == sizeof(tv) &&
                tv.tv_sec >= 0 && tv.tv_usec >= 0 && (tv.tv_sec || tv.tv_usec))
        {
            rt_uint64_t ticks = (rt_uint64_t)tv.tv_sec * RT_TICK_PER_SECOND +
                ((rt_uint64_t)tv.tv_usec * RT_TICK_PER_SECOND + 999999) / 1000000;
            timeout = ticks > (RT_TICK_MAX >> 1) ? RT_TICK_MAX >> 1 : ticks;
        }
        for (;;)
        {
            int result, error;
            rt_int32_t remaining = socket_rx_remaining(started, timeout);
            rt_err_t locked = rt_mutex_take_interruptible(&sock->rx_lock,
                    flags & MSG_DONTWAIT ? 0 : remaining);
            if (locked != RT_EOK)
            {
                rt_set_errno(locked == -RT_ETIMEOUT ? -EAGAIN : -EINTR);
                return -1;
            }
            if (first && ops->ioctlsocket)
            {
                int mode = ops->ioctlsocket(socket, F_GETFL, RT_NULL);
                if (mode >= 0 && (mode & O_NONBLOCK))
                    flags |= MSG_DONTWAIT;
            }
            first = RT_FALSE;
            if (request->accept)
                result = ops->accept_nonblock(socket, request->addr, request->addrlen);
            else if (request->message)
                result = ops->recvmsg(socket, request->message, flags | MSG_DONTWAIT);
            else
                result = ops->recvfrom(socket, request->mem, request->len,
                        flags | MSG_DONTWAIT, request->addr, request->addrlen);
            error = rt_get_errno();
            rt_mutex_release(&sock->rx_lock);
            rt_set_errno(error);
            if (result >= 0 || (error != EAGAIN && error != -EAGAIN &&
                    error != EWOULDBLOCK && error != -EWOULDBLOCK) ||
                    (flags & MSG_DONTWAIT))
                return result;

            remaining = socket_rx_remaining(started, timeout);
            if (!remaining)
            {
                rt_set_errno(-EAGAIN);
                return -1;
            }
            /* Round up when converting ticks; keep one deadline across retries. */
            if (remaining > 0)
            {
                rt_uint64_t ms = ((rt_uint64_t)remaining * 1000 +
                        RT_TICK_PER_SECOND - 1) / RT_TICK_PER_SECOND;
                remaining = ms > 0x7fffffff ? 0x7fffffff : (rt_int32_t)ms;
            }
            result = ops->wait_readable(socket, remaining);
            if (result <= 0)
            {
                rt_set_errno(result ? result : -EAGAIN);
                return -1;
            }
        }
    }
    /* Backends without nonblocking/readiness support retain their own waits. */
    if (request->accept)
        return ops->accept(socket, request->addr, request->addrlen);
    if (request->message)
        return ops->recvmsg(socket, request->message, flags);
    return ops->recvfrom(socket, request->mem, request->len, flags,
                         request->addr, request->addrlen);
}

static struct sal_socket *sal_socket_tx_lock(int socket, int flags, rt_tick_t started)
{
    struct sal_socket *sock;
    rt_int32_t wait = RT_WAITING_FOREVER;
    rt_err_t result;

    sal_lock();
    sock = sal_get_socket(socket);
    if (sock == RT_NULL)
    {
        sal_unlock();
        rt_set_errno(-EBADF);
        return RT_NULL;
    }

    /* Pin before releasing the table lock. A blocked sender must not prevent
     * unrelated sockets or netdev cleanup from acquiring sal_core_lock. */
    sock->io_users++;
    sal_unlock();

    /* Preserve both per-call and per-socket nonblocking behavior before
     * waiting for another sender. The pin keeps the backend alive here. */
    if (!(flags & MSG_DONTWAIT) && sock->pf && sock->pf->skt_ops &&
        sock->pf->skt_ops->ioctlsocket)
    {
        int saved_errno = rt_get_errno();
        int file_flags = sock->pf->skt_ops->ioctlsocket(
                (int)(size_t)sock->user_data, F_GETFL, RT_NULL);

        if (file_flags >= 0 && (file_flags & O_NONBLOCK))
            flags |= MSG_DONTWAIT;
        /* Protocol backends need not implement F_GETFL. */
        rt_set_errno(saved_errno);
    }
    if (flags & MSG_DONTWAIT)
    {
        wait = 0;
    }
    else if (sock->pf && sock->pf->skt_ops && sock->pf->skt_ops->getsockopt)
    {
        /* Read the backend's current setting, including accepted sockets and
         * concurrent setsockopt changes, without a second cached value. */
        struct timeval timeout = {0};
        socklen_t len = sizeof(timeout);
        int saved_errno = rt_get_errno();

        if (sock->pf->skt_ops->getsockopt((int)(size_t)sock->user_data,
                SOL_SOCKET, SO_SNDTIMEO, &timeout, &len) == 0 &&
            len == sizeof(timeout) && timeout.tv_sec >= 0 && timeout.tv_usec >= 0 &&
            (timeout.tv_sec || timeout.tv_usec))
        {
            rt_uint64_t ticks = (rt_uint64_t)timeout.tv_sec * RT_TICK_PER_SECOND +
                ((rt_uint64_t)timeout.tv_usec * RT_TICK_PER_SECOND + 999999) / 1000000;
            rt_tick_t elapsed = rt_tick_get() - started;

            if (ticks > (RT_TICK_MAX >> 1))
                ticks = RT_TICK_MAX >> 1;
            wait = elapsed < ticks ? (rt_int32_t)(ticks - elapsed) : 0;
        }
        /* Unsupported timeout options leave the original infinite wait. */
        rt_set_errno(saved_errno);
    }
    result = rt_mutex_take_interruptible(&sock->tx_lock, wait);
    if (result != RT_EOK)
    {
        sal_socket_put(sock);
        rt_set_errno(result == -RT_ETIMEOUT ? -EAGAIN : -EINTR);
        return RT_NULL;
    }
    return sock;
}

static void sal_socket_tx_unlock(struct sal_socket *sock)
{
    int saved_errno = rt_get_errno();

    rt_mutex_release(&sock->tx_lock);
    sal_socket_put(sock);
    rt_set_errno(saved_errno);
}

/* Claim teardown once and drain active I/O without holding sal_core_lock. */
static struct sal_socket *socket_close_begin(int socket)
{
    struct sal_socket *sock;
    rt_err_t result;

    RT_DEBUG_IN_THREAD_CONTEXT;
    sal_lock();
    sock = sal_get_socket(socket);
    if (sock == RT_NULL)
    {
        sal_unlock();
        rt_set_errno(-EBADF);
        return RT_NULL;
    }

    sock->closing = RT_TRUE;
    while (sock->io_users != 0)
    {
        sal_unlock();
        result = rt_sem_take_interruptible(&sock->io_drained, RT_WAITING_FOREVER);
        sal_lock();
        if (result != RT_EOK)
        {
            sock->closing = RT_FALSE;
            sal_unlock();
            rt_set_errno(result == -RT_EINTR ? -EINTR : -EIO);
            return RT_NULL;
        }
    }
    sal_unlock();
    return sock;
}

/**
 * This function will clean the netdev.
 *
 * @note please don't invoke it on ISR.
 */
int sal_netdev_cleanup(struct netdev *netdev)
{
    int idx;

    sal_lock();
    for (idx = 0; idx < socket_table.max_socket; idx++)
    {
        struct sal_socket *sock = socket_table.sockets[idx];

        if (sock && sock->netdev == netdev)
        {
            /* The protocol backend remains valid after an lwIP netdev is
             * removed. Drop only the interface reference so unplug cannot
             * block forever on an explicitly bound socket. */
            sock->netdev = RT_NULL;
            sock->netdev_bound = RT_FALSE;
        }
    }
    sal_unlock();

    return 0;
}

/**
 * This function will initialize sal socket object and set socket options
 *
 * @param family    protocol family
 * @param type      socket type
 * @param protocol  transfer Protocol
 * @param res       sal socket object address
 *
 * @return  0 : socket initialize success
 *         -1 : input the wrong family
 *         -2 : input the wrong socket type
 *         -3 : get network interface failed
 */
static int socket_init(int family, int type, int protocol, struct sal_socket **res)
{

    struct sal_socket *sock;
    struct sal_proto_family *pf = RT_NULL;
    struct netdev *netdv_def = netdev_default;
    struct netdev *netdev = RT_NULL;
    rt_bool_t flag = RT_FALSE;

    if (family < 0 || family > AF_MAX)
    {
        return -1;
    }

    if (type < 0 || type > SOCK_MAX)
    {
        return -2;
    }

    sock = *res;
    sock->domain = family;
    sock->type = type;
    sock->protocol = protocol;

    if (netdv_def && netdev_is_up(netdv_def))
    {
        /* check default network interface device protocol family */
        pf = (struct sal_proto_family *) netdv_def->sal_user_data;
        if (pf != RT_NULL && pf->skt_ops && (pf->family == family || pf->sec_family == family))
        {
            sock->netdev = netdv_def;
            flag = RT_TRUE;
        }
    }

    if (flag == RT_FALSE)
    {
        /* get network interface device by protocol family */
        netdev = netdev_get_by_family(family);
        if (netdev != RT_NULL)
        {
            sock->netdev = netdev;
            pf = (struct sal_proto_family *)netdev->sal_user_data;
            flag = RT_TRUE;
        }
    }

    if (flag == RT_FALSE)
    {
        LOG_E("not find network interface device by protocol family(%d).", family);
        return -3;
    }

    sock->pf = pf;
    sock->netdev_bound = RT_FALSE;

    return 0;
}

static int socket_alloc(struct sal_socket_table *st, int f_socket)
{
    int idx;

    /* find an empty socket entry */
    for (idx = f_socket; idx < (int) st->max_socket; idx++)
    {
        if (st->sockets[idx] == RT_NULL)
        {
            break;
        }
    }

    /* allocate a larger sockte container */
    if (idx == (int) st->max_socket &&  st->max_socket < SAL_SOCKETS_NUM)
    {
        int cnt, index;
        struct sal_socket **sockets;

        /* increase the number of socket with 4 step length */
        cnt = st->max_socket + SOCKET_TABLE_STEP_LEN;
        cnt = cnt > SAL_SOCKETS_NUM ? SAL_SOCKETS_NUM : cnt;

        sockets = rt_realloc(st->sockets, cnt * sizeof(struct sal_socket *));
        if (sockets == RT_NULL)
            goto __result; /* return st->max_socket */

        /* clean the new allocated fds */
        for (index = st->max_socket; index < cnt; index++)
        {
            sockets[index] = RT_NULL;
        }

        st->sockets = sockets;
        st->max_socket = cnt;
    }

    /* allocate  'struct sal_socket' */
    if (idx < (int) st->max_socket && st->sockets[idx] == RT_NULL)
    {
        st->sockets[idx] = rt_calloc(1, sizeof(struct sal_socket));
        if (st->sockets[idx] == RT_NULL)
        {
            idx = st->max_socket;
        }
    }

__result:
    return idx;
}

static void socket_free(struct sal_socket_table *st, int idx)
{
    struct sal_socket *sock;

    sock = st->sockets[idx];
    RT_ASSERT(sock->closing && sock->io_users == 0);
    st->sockets[idx] = RT_NULL;
    sock->magic = 0;
    sock->netdev = RT_NULL;
    sock->pf = RT_NULL;
    rt_mutex_detach(&sock->tx_lock);
    rt_mutex_detach(&sock->rx_lock);
    rt_sem_detach(&sock->io_drained);
    rt_free(sock);
}

static int socket_new(void)
{
    struct sal_socket *sock;
    struct sal_socket_table *st = &socket_table;
    int idx;

    sal_lock();

    /* find an empty sal socket entry */
    idx = socket_alloc(st, 0);

    /* can't find an empty sal socket entry */
    if (idx == (int) st->max_socket)
    {
        idx = -(1 + SAL_SOCKET_OFFSET);
        goto __result;
    }

    sock = st->sockets[idx];
    sock->socket = idx + SAL_SOCKET_OFFSET;
    sock->magic = SAL_SOCKET_MAGIC;
    sock->netdev = RT_NULL;
    sock->pf = RT_NULL;
    sock->netdev_bound = RT_FALSE;
    sock->user_data = RT_NULL;
    rt_mutex_init(&sock->tx_lock, "sal_tx", RT_IPC_FLAG_FIFO);
    rt_mutex_init(&sock->rx_lock, "sal_rx", RT_IPC_FLAG_FIFO);
    rt_sem_init(&sock->io_drained, "sal_drain", 0, RT_IPC_FLAG_FIFO);
#ifdef SAL_USING_TLS
    sock->user_data_tls = RT_NULL;
#endif

__result:
    sal_unlock();
    return idx + SAL_SOCKET_OFFSET;
}

static void socket_delete(int socket)
{
    struct sal_socket *sock;
    struct sal_socket_table *st = &socket_table;
    int idx = socket - SAL_SOCKET_OFFSET;
    int saved_errno = rt_get_errno();

    /* Roll back socket creation through the same teardown ownership and
     * I/O drain used by normal close. Netdev cleanup does not delete. */
    sock = socket_close_begin(socket);
    if (sock == RT_NULL)
    {
        rt_set_errno(saved_errno);
        return;
    }
    sal_lock();
    socket_free(st, idx);
    sal_unlock();
    rt_set_errno(saved_errno);
}

int sal_accept(int socket, struct sockaddr *addr, socklen_t *addrlen)
{
    int new_socket;
    struct sal_socket *sock;
    struct sal_proto_family *pf;

    /* get the socket object by socket descriptor */
    sock = sal_socket_get(socket);
    if (!sock)
        return -1;

    /* check the network interface socket operations */
    pf = sock->pf;
    if (!pf || !pf->skt_ops || !pf->skt_ops->accept)
    {
        rt_set_errno(-EOPNOTSUPP);
        return sal_socket_result(sock, -1);
    }

    struct sal_rx_request request = { .addr = addr, .addrlen = addrlen, .accept = RT_TRUE };
    new_socket = socket_receive(sock, &request);
    if (new_socket != -1)
    {
        int new_sal_socket;
        struct sal_socket *new_sock;

        /* allocate a new socket structure and registered socket options */
        new_sal_socket = socket_new();
        new_sock = sal_get_socket(new_sal_socket);
        if (new_sock == RT_NULL)
        {
            pf->skt_ops->closesocket(new_socket);
            return sal_socket_result(sock, -1);
        }

        /* An accepted socket belongs to the listener's interface. Re-selecting
         * the current default interface is incorrect when several lwIP netifs
         * (STA, AP, and LAN) are active at the same time. */
        new_sock->domain = sock->domain;
        new_sock->type = sock->type;
        new_sock->protocol = sock->protocol;
        new_sock->netdev = sock->netdev;
        new_sock->pf = sock->pf;
        new_sock->netdev_bound = sock->netdev_bound;
        new_sock->user_data = (void *)(size_t)new_socket;

        return sal_socket_result(sock, new_sal_socket);
    }

    return sal_socket_result(sock, -1);
}

static void sal_sockaddr_to_ipaddr(const struct sockaddr *name, ip_addr_t *local_ipaddr)
{
    const struct sockaddr_in *svr_addr = (const struct sockaddr_in *) name;

#if NETDEV_IPV4 && NETDEV_IPV6
    local_ipaddr->u_addr.ip4.addr = svr_addr->sin_addr.s_addr;
    local_ipaddr->type = IPADDR_TYPE_V4;
#elif NETDEV_IPV4
    local_ipaddr->addr = svr_addr->sin_addr.s_addr;
#elif NETDEV_IPV6
#error "not only support IPV6"
#endif /* NETDEV_IPV4 && NETDEV_IPV6*/
}

int sal_bind(int socket, const struct sockaddr *name, socklen_t namelen)
{
    struct sal_socket *sock;
    struct sal_proto_family *pf;
    struct netdev *bound_netdev = RT_NULL;
    ip_addr_t input_ipaddr;

    RT_ASSERT(name);

    /* get the socket object by socket descriptor */
    SAL_SOCKET_OBJ_GET(sock, socket);

    /* bind network interface by ip address */
    sal_sockaddr_to_ipaddr(name, &input_ipaddr);

    /* A wildcard address is stack-wide; a loopback address belongs to the
     * local protocol stack rather than to a physical netdev. */
    if (!ip_addr_isany_val(input_ipaddr))
    {
        struct sal_proto_family *input_pf = RT_NULL;

        if (!ip_addr_isloopback(&input_ipaddr))
        {
            bound_netdev = netdev_get_by_ipaddr(&input_ipaddr);
            if (bound_netdev == RT_NULL)
            {
                rt_set_errno(-EADDRNOTAVAIL);
                return -1;
            }
            SAL_NETDEV_SOCKETOPS_VALID(bound_netdev, input_pf, bind);
            if (input_pf != sock->pf)
            {
                rt_set_errno(-EAFNOSUPPORT);
                return -1;
            }
        }
    }

    SAL_SOCKETOPS_VALID(sock, pf, bind);
    if (pf->skt_ops->bind((int)(size_t)sock->user_data, name, namelen) < 0)
    {
        return -1;
    }
    sock->netdev = bound_netdev;
    sock->netdev_bound = bound_netdev != RT_NULL;
    return 0;
}

int sal_shutdown(int socket, int how)
{
    struct sal_socket *sock;
    struct sal_proto_family *pf;
    int error = 0;

    /* get the socket object by socket descriptor */
    SAL_SOCKET_OBJ_GET(sock, socket);

    /* shutdown operation not need to check network interface status */
    /* check the network interface socket opreation */
    SAL_SOCKETOPS_VALID(sock, pf, shutdown);

    if (pf->skt_ops->shutdown((int)(size_t)sock->user_data, how) == 0)
    {
#ifdef SAL_USING_TLS
        if (SAL_SOCKOPS_PROTO_TLS_VALID(sock, closesocket))
        {
            if (proto_tls->ops->closesocket(sock->user_data_tls) < 0)
            {
                return -1;
            }
        }
#endif
        error = 0;
    }
    else
    {
        error = -1;
    }


    return error;
}

int sal_getpeername(int socket, struct sockaddr *name, socklen_t *namelen)
{
    struct sal_socket *sock;
    struct sal_proto_family *pf;

    /* get the socket object by socket descriptor */
    SAL_SOCKET_OBJ_GET(sock, socket);

    /* check the network interface socket opreation */
    SAL_SOCKETOPS_VALID(sock, pf, getpeername);

    return pf->skt_ops->getpeername((int)(size_t)sock->user_data, name, namelen);
}

int sal_getsockname(int socket, struct sockaddr *name, socklen_t *namelen)
{
    struct sal_socket *sock;
    struct sal_proto_family *pf;

    /* get socket object by socket descriptor */
    SAL_SOCKET_OBJ_GET(sock, socket);

    /* check the network interface socket opreation */
    SAL_SOCKETOPS_VALID(sock, pf, getsockname);

    return pf->skt_ops->getsockname((int)(size_t)sock->user_data, name, namelen);
}

int sal_getsockopt(int socket, int level, int optname, void *optval, socklen_t *optlen)
{
    struct sal_socket *sock;
    struct sal_proto_family *pf;

    /* get the socket object by socket descriptor */
    SAL_SOCKET_OBJ_GET(sock, socket);

    /* check the network interface socket opreation */
    SAL_SOCKETOPS_VALID(sock, pf, getsockopt);

    return pf->skt_ops->getsockopt((int)(size_t)sock->user_data, level, optname, optval, optlen);
}

int sal_setsockopt(int socket, int level, int optname, const void *optval, socklen_t optlen)
{
    struct sal_socket *sock;
    struct sal_proto_family *pf;

    /* get the socket object by socket descriptor */
    SAL_SOCKET_OBJ_GET(sock, socket);

    /* check the network interface socket opreation */
    SAL_SOCKETOPS_VALID(sock, pf, setsockopt);

#ifdef SAL_USING_TLS
    if (level == SOL_TLS)
    {
        switch (optname)
        {
        case TLS_CRET_LIST:
            SAL_SOCKOPT_PROTO_TLS_EXEC(sock, set_cret_list, optval, optlen);
            break;

        case TLS_CIPHERSUITE_LIST:
            SAL_SOCKOPT_PROTO_TLS_EXEC(sock, set_ciphersurite, optval, optlen);
            break;

        case TLS_PEER_VERIFY:
            SAL_SOCKOPT_PROTO_TLS_EXEC(sock, set_peer_verify, optval, optlen);
            break;

        case TLS_DTLS_ROLE:
            SAL_SOCKOPT_PROTO_TLS_EXEC(sock, set_dtls_role, optval, optlen);
            break;

        default:
            return -1;
        }

        return 0;
    }
    else
    {
        return pf->skt_ops->setsockopt((int) sock->user_data, level, optname, optval, optlen);
    }
#else
    return pf->skt_ops->setsockopt((int)(size_t)sock->user_data, level, optname, optval, optlen);
#endif /* SAL_USING_TLS */
}

int sal_connect(int socket, const struct sockaddr *name, socklen_t namelen)
{
    struct sal_socket *sock;
    struct sal_proto_family *pf;
    int ret;

    /* get the socket object by socket descriptor */
    SAL_SOCKET_OBJ_GET(sock, socket);

    /* check the network interface socket opreation */
    SAL_SOCKETOPS_VALID(sock, pf, connect);

    ret = pf->skt_ops->connect((int)(size_t)sock->user_data, name, namelen);
#ifdef SAL_USING_TLS
    if (ret >= 0 && SAL_SOCKOPS_PROTO_TLS_VALID(sock, connect))
    {
        if (proto_tls->ops->connect(sock->user_data_tls) < 0)
        {
            return -1;
        }

        return ret;
    }
#endif

    return ret;
}

int sal_listen(int socket, int backlog)
{
    struct sal_socket *sock;
    struct sal_proto_family *pf;

    /* get the socket object by socket descriptor */
    SAL_SOCKET_OBJ_GET(sock, socket);

    /* check the network interface socket opreation */
    SAL_SOCKETOPS_VALID(sock, pf, listen);

    return pf->skt_ops->listen((int)(size_t)sock->user_data, backlog);
}

int sal_recvfrom(int socket, void *mem, size_t len, int flags,
                 struct sockaddr *from, socklen_t *fromlen)
{
    struct sal_socket *sock = sal_socket_get(socket);
    struct sal_rx_request request = { .mem = mem, .len = len, .flags = flags,
                                     .addr = from, .addrlen = fromlen };
    int result;

    if (!sock)
        return -1;
    if (!sock->pf || !sock->pf->skt_ops || !sock->pf->skt_ops->recvfrom)
    {
        rt_set_errno(-EOPNOTSUPP);
        return sal_socket_result(sock, -1);
    }
#ifdef SAL_USING_TLS
    if (SAL_SOCKOPS_PROTO_TLS_VALID(sock, recv))
        result = proto_tls->ops->recv(sock->user_data_tls, mem, len);
    else
#endif
        result = socket_receive(sock, &request);
    return sal_socket_result(sock, result);
}

int sal_recvmsg(int socket, struct sal_msghdr *message, int flags)
{
    struct sal_socket *sock = sal_socket_get(socket);
    struct sal_rx_request request = { .message = message, .flags = flags };
    int result;

    if (!sock)
        return -1;
    if (!message)
    {
        rt_set_errno(-EFAULT);
        return sal_socket_result(sock, -1);
    }
    if (!sock->pf || !sock->pf->skt_ops || !sock->pf->skt_ops->recvmsg)
    {
        rt_set_errno(-EOPNOTSUPP);
        return sal_socket_result(sock, -1);
    }
#ifdef SAL_USING_TLS
    if (SAL_SOCKOPS_PROTO_TLS_VALID(sock, recv))
    {
        rt_set_errno(-EOPNOTSUPP);
        return sal_socket_result(sock, -1);
    }
#endif
    result = socket_receive(sock, &request);
    return sal_socket_result(sock, result);
}

int sal_sendto(int socket, const void *dataptr, size_t size, int flags,
               const struct sockaddr *to, socklen_t tolen)
{
    struct sal_socket *sock;
    struct sal_proto_family *pf;
    rt_tick_t started = rt_tick_get();
    int ret;

    sock = sal_socket_tx_lock(socket, flags, started);
    if (sock == RT_NULL)
    {
        return -1;
    }

    pf = sock->pf;
    if (pf == RT_NULL || pf->skt_ops == RT_NULL || pf->skt_ops->sendto == RT_NULL)
    {
        rt_set_errno(-EOPNOTSUPP);
        ret = -1;
        goto __exit;
    }

#ifdef SAL_USING_TLS
    if (SAL_SOCKOPS_PROTO_TLS_VALID(sock, send))
    {
        if ((ret = proto_tls->ops->send(sock->user_data_tls, dataptr, size)) < 0)
        {
            ret = -1;
        }
    }
    else
#endif
    if (pf->skt_ops->sendto_started)
        ret = pf->skt_ops->sendto_started((int)(size_t)sock->user_data,
                dataptr, size, flags, to, tolen, started);
    else
        ret = pf->skt_ops->sendto((int)(size_t)sock->user_data, dataptr, size, flags, to, tolen);

__exit:
    sal_socket_tx_unlock(sock);
    return ret;
}

int sal_sendmsg(int socket, const struct sal_msghdr *message, int flags)
{
    struct sal_socket *sock;
    struct sal_proto_family *pf;
    rt_tick_t started = rt_tick_get();
    int ret;

    sock = sal_socket_tx_lock(socket, flags, started);
    if (sock == RT_NULL)
    {
        return -1;
    }

    pf = sock->pf;
    if (pf == RT_NULL || pf->skt_ops == RT_NULL || pf->skt_ops->sendmsg == RT_NULL)
    {
        rt_set_errno(-EOPNOTSUPP);
        ret = -1;
        goto __exit;
    }

#ifdef SAL_USING_TLS
    if (SAL_SOCKOPS_PROTO_TLS_VALID(sock, send))
    {
        rt_set_errno(-EOPNOTSUPP);
        ret = -1;
        goto __exit;
    }
#endif

    if (pf->skt_ops->sendmsg_started)
        ret = pf->skt_ops->sendmsg_started((int)(size_t)sock->user_data, message, flags, started);
    else
        ret = pf->skt_ops->sendmsg((int)(size_t)sock->user_data, message, flags);

__exit:
    sal_socket_tx_unlock(sock);
    return ret;
}

int sal_socket(int domain, int type, int protocol)
{
    int retval;
    int socket, proto_socket;
    struct sal_socket *sock;
    struct sal_proto_family *pf;

    /* allocate a new socket and registered socket options */
    socket = socket_new();
    if (socket < 0)
    {
        return -1;
    }

    /* get sal socket object by socket descriptor */
    sock = sal_get_socket(socket);
    if (sock == RT_NULL)
    {
        socket_delete(socket);
        return -1;
    }

    /* Initialize sal socket object */
    retval = socket_init(domain, type, protocol, &sock);
    if (retval < 0)
    {
        LOG_E("SAL socket protocol family input failed, return error %d.", retval);
        socket_delete(socket);
        return -1;
    }

    /* valid the network interface socket opreation */
    SAL_SOCKETOPS_VALID(sock, pf, socket);

    proto_socket = pf->skt_ops->socket(domain, type, protocol);
    if (proto_socket >= 0)
    {
#ifdef SAL_USING_TLS
        if (SAL_SOCKOPS_PROTO_TLS_VALID(sock, socket))
        {
            sock->user_data_tls = proto_tls->ops->socket(socket);
            if (sock->user_data_tls == RT_NULL)
            {
                socket_delete(socket);
                return -1;
            }
        }
#endif
        sock->user_data = (void *)(size_t)proto_socket;
        return sock->socket;
    }
    socket_delete(socket);
    return -1;
}

int sal_closesocket_ex(int socket, rt_bool_t *closed)
{
    struct sal_socket *sock;
    struct sal_proto_family *pf;
    struct sal_socket_table *st = &socket_table;
    int idx = socket - SAL_SOCKET_OFFSET;
    int error = 0;
    int saved_errno;

    if (closed)
        *closed = RT_FALSE;
    sock = socket_close_begin(socket);
    if (sock == RT_NULL)
    {
        return -1;
    }

    pf = sock->pf;
    if (pf == RT_NULL || pf->skt_ops == RT_NULL || pf->skt_ops->closesocket == RT_NULL)
    {
        rt_set_errno(-EOPNOTSUPP);
        error = -1;
        goto __exit;
    }

    if (pf->skt_ops->closesocket((int)(size_t)sock->user_data) == 0)
    {
#ifdef SAL_USING_TLS
        if (SAL_SOCKOPS_PROTO_TLS_VALID(sock, closesocket))
        {
            if (proto_tls->ops->closesocket(sock->user_data_tls) < 0)
            {
                error = -1;
            }
        }
#else
        error = 0;
#endif
    }
    else
    {
        error = -1;
    }

__exit:
    saved_errno = rt_get_errno();
    sal_lock();
    socket_free(st, idx);
    sal_unlock();
    if (closed)
        *closed = RT_TRUE;
    rt_set_errno(saved_errno);

    return error;
}

int sal_closesocket(int socket)
{
    return sal_closesocket_ex(socket, RT_NULL);
}

int sal_ioctlsocket(int socket, long cmd, void *arg)
{
    struct sal_socket *sock;
    struct sal_proto_family *pf;
    struct sockaddr_in *addr_in = RT_NULL;
    struct sockaddr *addr = RT_NULL;
    ip_addr_t input_ipaddr;
    if (cmd == F_GETFL || cmd == F_SETFL || cmd == FIONBIO)
    {
        int result, error;
        rt_err_t locked;
        sock = sal_socket_get(socket);
        if (!sock)
            return -1;
        pf = sock->pf;
        if (!pf || !pf->skt_ops || !pf->skt_ops->ioctlsocket)
        {
            rt_set_errno(-EOPNOTSUPP);
            return sal_socket_result(sock, -1);
        }
        locked = rt_mutex_take_interruptible(&sock->rx_lock, RT_WAITING_FOREVER);
        if (locked != RT_EOK)
        {
            rt_set_errno(-EINTR);
            return sal_socket_result(sock, -1);
        }
        result = pf->skt_ops->ioctlsocket((int)(size_t)sock->user_data, cmd, arg);
        error = rt_get_errno();
        rt_mutex_release(&sock->rx_lock);
        rt_set_errno(error);
        return sal_socket_result(sock, result);
    }

    SAL_SOCKET_OBJ_GET(sock, socket);
    SAL_SOCKETOPS_VALID(sock, pf, ioctlsocket);

    struct sal_ifreq *ifr = (struct sal_ifreq *)arg;

    if((sock->domain == AF_INET)&&(sock->netdev)&&(ifr != RT_NULL))
    {
        switch (cmd)
        {
        case SIOCGIFADDR:
            addr_in = (struct sockaddr_in *)&(ifr->ifr_ifru.ifru_addr);
#if NETDEV_IPV4 && NETDEV_IPV6
            addr_in->sin_addr.s_addr = sock->netdev->ip_addr.u_addr.ip4.addr;
#elif NETDEV_IPV4
            addr_in->sin_addr.s_addr = sock->netdev->ip_addr.addr;
#elif NETDEV_IPV6
#error "not only support IPV6"
#endif /* NETDEV_IPV4 && NETDEV_IPV6*/
            return 0;

        case SIOCSIFADDR:
            addr = (struct sockaddr *)&(ifr->ifr_ifru.ifru_addr);
            sal_sockaddr_to_ipaddr(addr,&input_ipaddr);
            netdev_set_ipaddr(sock->netdev,&input_ipaddr);
            return 0;    

        case SIOCGIFNETMASK:
            addr_in = (struct sockaddr_in *)&(ifr->ifr_ifru.ifru_netmask);
#if NETDEV_IPV4 && NETDEV_IPV6
            addr_in->sin_addr.s_addr = sock->netdev->netmask.u_addr.ip4.addr;
#elif NETDEV_IPV4
            addr_in->sin_addr.s_addr = sock->netdev->netmask.addr;
#elif NETDEV_IPV6
#error "not only support IPV6"
#endif /* NETDEV_IPV4 && NETDEV_IPV6*/
            return 0;

        case SIOCSIFNETMASK:
            addr = (struct sockaddr *)&(ifr->ifr_ifru.ifru_netmask);
            sal_sockaddr_to_ipaddr(addr,&input_ipaddr);
            netdev_set_netmask(sock->netdev,&input_ipaddr);
            return 0;            

        case SIOCGIFHWADDR:
            addr = (struct sockaddr *)&(ifr->ifr_ifru.ifru_hwaddr);
            rt_memcpy(addr->sa_data,sock->netdev->hwaddr,sock->netdev->hwaddr_len);
            return 0;

        case SIOCGIFMTU:
            ifr->ifr_ifru.ifru_mtu = sock->netdev->mtu;
            return 0;            

        default:
            break;
        }
    }
    return pf->skt_ops->ioctlsocket((int)(size_t)sock->user_data, cmd, arg);
}

#ifdef SAL_USING_POSIX
int sal_poll(struct dfs_fd *file, struct rt_pollreq *req)
{
    struct sal_socket *sock;
    struct sal_proto_family *pf;
    int socket = (int)(size_t)file->fnode->data;

    /* get the socket object by socket descriptor */
    SAL_SOCKET_OBJ_GET(sock, socket);

    /* check the network interface socket opreation */
    SAL_SOCKETOPS_VALID(sock, pf, poll);

    return pf->skt_ops->poll(file, req);
}
#endif

struct hostent *sal_gethostbyname(const char *name)
{
    struct netdev *netdev = netdev_default;
    struct sal_proto_family *pf;

    if (SAL_NETDEV_NETDBOPS_VALID(netdev, pf, gethostbyname))
    {
        return pf->netdb_ops->gethostbyname(name);
    }
    else
    {
        /* get the first network interface device with up status */
        netdev = netdev_get_first_by_flags(NETDEV_FLAG_UP);
        if (SAL_NETDEV_NETDBOPS_VALID(netdev, pf, gethostbyname))
        {
            return pf->netdb_ops->gethostbyname(name);
        }
    }

    return RT_NULL;
}

int sal_gethostbyname_r(const char *name, struct hostent *ret, char *buf,
                        size_t buflen, struct hostent **result, int *h_errnop)
{
    struct netdev *netdev = netdev_default;
    struct sal_proto_family *pf;

    if (SAL_NETDEV_NETDBOPS_VALID(netdev, pf, gethostbyname_r))
    {
        return pf->netdb_ops->gethostbyname_r(name, ret, buf, buflen, result, h_errnop);
    }
    else
    {
        /* get the first network interface device with up status */
        netdev = netdev_get_first_by_flags(NETDEV_FLAG_UP);
        if (SAL_NETDEV_NETDBOPS_VALID(netdev, pf, gethostbyname_r))
        {
            return pf->netdb_ops->gethostbyname_r(name, ret, buf, buflen, result, h_errnop);
        }
    }

    return -1;
}

int sal_getaddrinfo(const char *nodename,
                    const char *servname,
                    const struct addrinfo *hints,
                    struct addrinfo **res)
{
    struct netdev *netdev = netdev_default;
    struct sal_proto_family *pf;

    if (SAL_NETDEV_NETDBOPS_VALID(netdev, pf, getaddrinfo))
    {
        return pf->netdb_ops->getaddrinfo(nodename, servname, hints, res);
    }
    else
    {
        /* get the first network interface device with up status */
        netdev = netdev_get_first_by_flags(NETDEV_FLAG_UP);
        if (SAL_NETDEV_NETDBOPS_VALID(netdev, pf, getaddrinfo))
        {
            return pf->netdb_ops->getaddrinfo(nodename, servname, hints, res);
        }
    }

    return -1;
}

void sal_freeaddrinfo(struct addrinfo *ai)
{
    struct netdev *netdev = netdev_default;
    struct sal_proto_family *pf;

    if (SAL_NETDEV_NETDBOPS_VALID(netdev, pf, freeaddrinfo))
    {
        pf->netdb_ops->freeaddrinfo(ai);
    }
    else
    {
        /* get the first network interface device with up status */
        netdev = netdev_get_first_by_flags(NETDEV_FLAG_UP);
        if (SAL_NETDEV_NETDBOPS_VALID(netdev, pf, freeaddrinfo))
        {
            pf->netdb_ops->freeaddrinfo(ai);
        }
    }
}

/*
 * Copyright (c) 2006-2018, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2015-02-17     Bernard      First version
 * 2018-05-17     ChenYong     Add socket abstraction layer
 */

#include <dfs.h>
#include <dfs_file.h>
#include <dfs_poll.h>
#include <dfs_net.h>

#include <sys/socket.h>

/* Keep the DFS object and its backend socket alive across every socket call. */
#define SOCKET_GET(s) \
    struct dfs_fd *socket_file; \
    int socket = dfs_net_getsocket_ref((s), &socket_file); \
    if (socket < 0) \
    { \
        return -1; \
    }

static int socket_result(struct dfs_fd *file, int result)
{
    fd_put(file);
    return result;
}

/* A socket descriptor's fnode, complete before anyone can see it. */
static struct dfs_fnode *socket_fnode_new(int socket)
{
    struct dfs_fnode *fnode = rt_calloc(1, sizeof(struct dfs_fnode));

    if (!fnode)
        return RT_NULL;
    rt_list_init(&fnode->list);
    fnode->type = FT_SOCKET;
    fnode->path = NULL;
    fnode->fullpath = NULL;
    fnode->ref_count = 1;
    fnode->fops = dfs_net_get_fops();
    fnode->size = 0;
    /* set socket to the data of dfs_fd */
    fnode->data = (void *)(size_t)socket;
    return fnode;
}

/* fd_new()'s slot has no fnode until here, so other threads' close(),
 * dup(), dup2() and fork() skip it (fd_unsettled_locked()). */
static void socket_fd_publish(struct dfs_fd *d, struct dfs_fnode *fnode)
{
    dfs_fd_lock();
    d->flags = O_RDWR; /* set flags as read and write */
    d->pos = 0;
    d->fnode = fnode;
    dfs_fd_unlock();
}

int accept(int s, struct sockaddr *addr, socklen_t *addrlen)
{
    int new_socket = -1;
    struct dfs_fnode *fnode;
    SOCKET_GET(s);

    new_socket = sal_accept(socket, addr, addrlen);
    fd_put(socket_file);
    if (new_socket != -1)
    {
        /* this is a new socket, create it in file system fd */
        int fd;
        struct dfs_fd *d;

        /* allocate a fd */
        fd = fd_new();
        if (fd < 0)
        {
            sal_closesocket(new_socket);
            rt_set_errno(-ENOMEM);
            return -1;
        }

        d = fd_get(fd);
        /* Built aside and published whole: the slot is in the table
         * already, and close(), dup2() or fork() from another thread must
         * not find a half-built fnode (fops NULL). */
        fnode = d ? socket_fnode_new(new_socket) : RT_NULL;
        if (fnode)
        {
            socket_fd_publish(d, fnode);
            return fd;
        }

        fd_release(fd);
        sal_closesocket(new_socket);
        rt_set_errno(-ENOMEM);
        return -1;
    }

    return -1;
}
RTM_EXPORT(accept);

int bind(int s, const struct sockaddr *name, socklen_t namelen)
{
    SOCKET_GET(s);

#ifdef SAL_USING_AF_UNIX
    struct sockaddr_in server_addr = {0};
    if (name->sa_family == AF_UNIX)
    {
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(514);
        server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        return socket_result(socket_file,
            sal_bind(socket, (struct sockaddr *)&server_addr, namelen));
    }
#endif /* SAL_USING_AF_UNIX */

    return socket_result(socket_file, sal_bind(socket, name, namelen));
}
RTM_EXPORT(bind);

int shutdown(int s, int how)
{
    SOCKET_GET(s);
    return socket_result(socket_file, sal_shutdown(socket, how));
}
RTM_EXPORT(shutdown);

int getpeername(int s, struct sockaddr *name, socklen_t *namelen)
{
    SOCKET_GET(s);

    return socket_result(socket_file, sal_getpeername(socket, name, namelen));
}
RTM_EXPORT(getpeername);

int getsockname(int s, struct sockaddr *name, socklen_t *namelen)
{
    SOCKET_GET(s);

    return socket_result(socket_file, sal_getsockname(socket, name, namelen));
}
RTM_EXPORT(getsockname);

int getsockopt(int s, int level, int optname, void *optval, socklen_t *optlen)
{
    SOCKET_GET(s);

    return socket_result(socket_file, sal_getsockopt(socket, level, optname, optval, optlen));
}
RTM_EXPORT(getsockopt);

int setsockopt(int s, int level, int optname, const void *optval, socklen_t optlen)
{
    SOCKET_GET(s);

    return socket_result(socket_file, sal_setsockopt(socket, level, optname, optval, optlen));
}
RTM_EXPORT(setsockopt);

int connect(int s, const struct sockaddr *name, socklen_t namelen)
{
    SOCKET_GET(s);

#ifdef SAL_USING_AF_UNIX
    struct sockaddr_in server_addr = {0};
    if (name->sa_family == AF_UNIX)
    {
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(514);
        server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        return socket_result(socket_file,
            sal_connect(socket, (struct sockaddr *)&server_addr, namelen));
    }
#endif /* SAL_USING_AF_UNIX */

    return socket_result(socket_file, sal_connect(socket, name, namelen));
}
RTM_EXPORT(connect);

int listen(int s, int backlog)
{
    SOCKET_GET(s);

    return socket_result(socket_file, sal_listen(socket, backlog));
}
RTM_EXPORT(listen);

int recv(int s, void *mem, size_t len, int flags)
{
    SOCKET_GET(s);

    return socket_result(socket_file, sal_recvfrom(socket, mem, len, flags, NULL, NULL));
}
RTM_EXPORT(recv);

int recvfrom(int s, void *mem, size_t len, int flags,
             struct sockaddr *from, socklen_t *fromlen)
{
    SOCKET_GET(s);

    return socket_result(socket_file, sal_recvfrom(socket, mem, len, flags, from, fromlen));
}
RTM_EXPORT(recvfrom);

int send(int s, const void *dataptr, size_t size, int flags)
{
    SOCKET_GET(s);

    return socket_result(socket_file, sal_sendto(socket, dataptr, size, flags, NULL, 0));
}
RTM_EXPORT(send);

int sendto(int s, const void *dataptr, size_t size, int flags,
           const struct sockaddr *to, socklen_t tolen)
{
    SOCKET_GET(s);

    return socket_result(socket_file, sal_sendto(socket, dataptr, size, flags, to, tolen));
}
RTM_EXPORT(sendto);

int socket(int domain, int type, int protocol)
{
    /* create a BSD socket */
    int fd;
    int socket;
    struct dfs_fd *d;
    struct dfs_fnode *fnode;

    /* allocate a fd */
    fd = fd_new();
    if (fd < 0)
    {
        rt_set_errno(-ENOMEM);

        return -1;
    }
    d = fd_get(fd);
    if (!d)
    {
        fd_release(fd);
        rt_set_errno(-ENOMEM);
        return -1;
    }

#ifdef SAL_USING_AF_UNIX
    if (domain == AF_UNIX)
    {
        domain = AF_INET;
    }
#endif /* SAL_USING_AF_UNIX */

    /* create socket and then put it to the dfs_fd. The fnode is built aside
     * and published whole once the socket exists: sal_socket() may sleep
     * with the slot already in the table, and close(), dup2() or fork()
     * from another thread must not find a half-built fnode (fops NULL). */
    socket = sal_socket(domain, type, protocol);
    if (socket < 0)
    {
        /* sal_socket() set errno; fd_release() takes a lock, which resets
         * it. */
        int err = rt_get_errno();

        fd_release(fd);
        rt_set_errno(err ? err : -ENOMEM);
        return -1;
    }
    fnode = socket_fnode_new(socket);
    if (!fnode)
    {
        fd_release(fd);
        sal_closesocket(socket);
        rt_set_errno(-ENOMEM);
        return -1;
    }
    socket_fd_publish(d, fnode);

    return fd;
}
RTM_EXPORT(socket);

int closesocket(int s)
{
    int error;

    error = fdt_fd_close(dfs_fdtable_get(), s, FT_SOCKET);

    if (error < 0)
    {
        rt_set_errno(error);
        return -1;
    }

    return 0;
}
RTM_EXPORT(closesocket);

int ioctlsocket(int s, long cmd, void *arg)
{
    SOCKET_GET(s);

    return socket_result(socket_file, sal_ioctlsocket(socket, cmd, arg));
}
RTM_EXPORT(ioctlsocket);

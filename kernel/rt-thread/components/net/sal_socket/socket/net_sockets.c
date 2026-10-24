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

int accept(int s, struct sockaddr *addr, socklen_t *addrlen)
{
    int new_socket = -1;
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
        if(d)
        {
            /* this is a socket fd */
            d->fnode = (struct dfs_fnode *)rt_calloc(1, sizeof(struct dfs_fnode));
            if (!d->fnode)
            {
                /* release fd */
                fd_release(fd);
                sal_closesocket(new_socket);
                rt_set_errno(-ENOMEM);
                return -1;
            }
            rt_list_init(&d->fnode->list);

            d->fnode->type = FT_SOCKET;
            d->fnode->path = NULL;
            d->fnode->fullpath = NULL;
            d->fnode->ref_count = 1;
            d->fnode->fops = dfs_net_get_fops();
            d->flags = O_RDWR; /* set flags as read and write */
            d->fnode->size = 0;
            d->pos = 0;

            /* set socket to the data of dfs_fd */
            d->fnode->data = (void *)(size_t)new_socket;

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
    d->fnode = (struct dfs_fnode *)rt_calloc(1, sizeof(struct dfs_fnode));
    if (!d->fnode)
    {
        /* release fd */
        fd_release(fd);
        rt_set_errno(-ENOMEM);
        return -1;
    }
    /* fd_release() also owns this fnode if sal_socket() fails. */
    d->fnode->ref_count = 1;

#ifdef SAL_USING_AF_UNIX
    if (domain == AF_UNIX)
    {
        domain = AF_INET;
    }
#endif /* SAL_USING_AF_UNIX */

    /* create socket  and then put it to the dfs_fd */
    socket = sal_socket(domain, type, protocol);
    if (socket >= 0)
    {
        rt_list_init(&d->fnode->list);
        /* this is a socket fd */
        d->fnode->type = FT_SOCKET;
        d->fnode->path = NULL;
        d->fnode->fullpath = NULL;
        d->fnode->fops = dfs_net_get_fops();

        d->flags = O_RDWR; /* set flags as read and write */
        d->fnode->size = 0;
        d->pos = 0;

        /* set socket to the data of dfs_fd */
        d->fnode->data = (void *)(size_t)socket;
    }
    else
    {
        /* release fd */
        fd_release(fd);
        rt_set_errno(-ENOMEM);
        return -1;
    }

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

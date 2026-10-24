/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2005-02-22     Bernard      The first version.
 * 2017-12-11     Bernard      Use rt_free to instead of free in fd_is_open().
 * 2018-03-20     Heyuanjie    dynamic allocation FD
 */

#include <dfs.h>
#include <dfs_fs.h>
#include <dfs_file.h>
#include "dfs_private.h"

#ifdef RT_USING_LWP
#include <lwp.h>
#endif

/* Global variables */
const struct dfs_filesystem_ops *filesystem_operation_table[DFS_FILESYSTEM_TYPES_MAX];
struct dfs_filesystem filesystem_table[DFS_FILESYSTEMS_MAX];

/* device filesystem lock */
static struct rt_mutex fslock;
static struct rt_mutex fdlock;

#ifdef DFS_USING_WORKDIR
char working_directory[DFS_PATH_MAX] = {"/"};
#endif

static struct dfs_fdtable _fdtab;
static int  fd_alloc(struct dfs_fdtable *fdt, int startfd);
static struct dfs_fd *failed_closes;
static rt_bool_t retrying_closes;
static void fd_retry_failed_closes(void);

/**
 * @addtogroup DFS
 */

/*@{*/

/**
 * this function will initialize device file system.
 */
int dfs_init(void)
{
    static rt_bool_t init_ok = RT_FALSE;

    if (init_ok)
    {
        rt_kprintf("dfs already init.\n");
        return 0;
    }

    /* init fnode hash table */
    dfs_fnode_mgr_init();

    /* clear filesystem operations table */
    memset((void *)filesystem_operation_table, 0, sizeof(filesystem_operation_table));
    /* clear filesystem table */
    memset(filesystem_table, 0, sizeof(filesystem_table));
    /* clean fd table */
    memset(&_fdtab, 0, sizeof(_fdtab));

    /* create device filesystem lock */
    rt_mutex_init(&fslock, "fslock", RT_IPC_FLAG_FIFO);
    rt_mutex_init(&fdlock, "fdlock", RT_IPC_FLAG_FIFO);

#ifdef DFS_USING_WORKDIR
    /* set current working directory */
    memset(working_directory, 0, sizeof(working_directory));
    working_directory[0] = '/';
#endif

#ifdef RT_USING_DFS_DEVFS
    {
        extern int devfs_init(void);

        /* if enable devfs, initialize and mount it as soon as possible */
        devfs_init();

        dfs_mount(NULL, "/dev", "devfs", 0, 0);
    }
#endif

#ifdef RT_USING_DFS_PROCFS
    {
        extern int procfs_init(void);

        procfs_init();

        init_ok = dfs_mount(NULL, "/proc", "procfs", 0, 0);
    }
#endif

    init_ok = RT_TRUE;

    return 0;
}
INIT_PREV_EXPORT(dfs_init);

/**
 * this function will lock device file system.
 *
 * @note please don't invoke it on ISR.
 */
void dfs_lock(void)
{
    rt_err_t result = -RT_EBUSY;

    while (result == -RT_EBUSY)
    {
        result = rt_mutex_take(&fslock, RT_WAITING_FOREVER);
    }

    if (result != RT_EOK)
    {
        RT_ASSERT(0);
    }
}

void dfs_fd_lock(void)
{
    rt_err_t result = -RT_EBUSY;

    while (result == -RT_EBUSY)
    {
        result = rt_mutex_take(&fdlock, RT_WAITING_FOREVER);
    }

    if (result != RT_EOK)
    {
        RT_ASSERT(0);
    }
}

/**
 * this function will lock device file system.
 *
 * @note please don't invoke it on ISR.
 */
void dfs_unlock(void)
{
    rt_mutex_release(&fslock);
}

void dfs_fd_unlock(void)
{
    rt_mutex_release(&fdlock);
}

static int fd_slot_expand(struct dfs_fdtable *fdt, int fd)
{
    int nr;
    int index;
    struct dfs_fd **fds = NULL;

    if (fd < fdt->maxfd)
    {
        return fd;
    }
    if (fd >= DFS_FD_MAX)
    {
        return -1;
    }

    nr = ((fd + 4) & ~3);
    if (nr > DFS_FD_MAX)
    {
        nr = DFS_FD_MAX;
    }
    fds = (struct dfs_fd **)rt_realloc(fdt->fds, nr * sizeof(struct dfs_fd *));
    if (!fds)
    {
        return -1;
    }

    /* clean the new allocated fds */
    for (index = fdt->maxfd; index < nr; index++)
    {
        fds[index] = NULL;
    }
    fdt->fds   = fds;
    fdt->maxfd = nr;

    return fd;
}

static int fd_slot_alloc(struct dfs_fdtable *fdt, int startfd)
{
    int idx;

    /* find an empty fd slot */
    for (idx = startfd; idx < (int)fdt->maxfd; idx++)
    {
        if (fdt->fds[idx] == RT_NULL)
        {
            return idx;
        }
    }

    idx = fdt->maxfd;
    if (idx < startfd)
    {
        idx = startfd;
    }
    if (fd_slot_expand(fdt, idx) < 0)
    {
        return -1;
    }
    return idx;
}

static int fd_alloc(struct dfs_fdtable *fdt, int startfd)
{
    int idx;
    struct dfs_fd *fd = NULL;

    idx = fd_slot_alloc(fdt, startfd);

    /* allocate  'struct dfs_fd' */
    if (idx < 0)
    {
        return -1;
    }
    fd = (struct dfs_fd *)rt_calloc(1, sizeof(struct dfs_fd));
    if (!fd)
    {
        return -1;
    }
    fd->ref_count = 1;
    fd->magic = DFS_FD_MAGIC;
    fd->fnode = NULL;
    fdt->fds[idx] = fd;

    return idx;
}

/**
 * @ingroup Fd
 * This function will allocate a file descriptor.
 *
 * @return -1 on failed or the allocated file descriptor.
 */
int fdt_fd_new(struct dfs_fdtable *fdt)
{
    int idx;

    /* Retry retired handles before allocating more, without holding fdlock
     * across backend operations. Persistent failures remain owned by DFS. */
    fd_retry_failed_closes();

    /* lock filesystem */
    dfs_fd_lock();

    /* find an empty fd entry */
    idx = fd_alloc(fdt, DFS_STDIO_OFFSET);

    /* can't find an empty fd entry */
    if (idx < 0)
    {
        LOG_E("DFS fd new is failed! Could not found an empty fd entry.");
    }

    dfs_fd_unlock();
    return idx;
}

int fd_new(void)
{
    struct dfs_fdtable *fdt = NULL;

    fdt = dfs_fdtable_get();
    return fdt_fd_new(fdt);
}

/**
 * @ingroup Fd
 *
 * This function will return a file descriptor structure according to file
 * descriptor.
 *
 * @return NULL on on this file descriptor or the file descriptor structure
 * pointer.
 */

/* Caller holds fdlock. */
static struct dfs_fd *fdt_fd_get_locked(struct dfs_fdtable *fdt, int fd)
{
    struct dfs_fd *d;

    if (fd < 0 || fd >= (int)fdt->maxfd)
    {
        return NULL;
    }

    d = fdt->fds[fd];

    /* check dfs_fd valid or not */
    if ((d == NULL) || (d->magic != DFS_FD_MAGIC) ||
        (d->flags & DFS_F_CLOSING))
    {
        return NULL;
    }

    return d;
}

struct dfs_fd *fdt_fd_get(struct dfs_fdtable *fdt, int fd)
{
    struct dfs_fd *d;

    dfs_fd_lock();
    d = fdt_fd_get_locked(fdt, fd);
    dfs_fd_unlock();

    return d;
}

struct dfs_fd *fd_get(int fd)
{
    struct dfs_fdtable *fdt;

    fdt = dfs_fdtable_get();
    return fdt_fd_get(fdt, fd);
}

struct dfs_fd *fd_get_ref(int fd)
{
    struct dfs_fd *d;

    dfs_fd_lock();
    d = fdt_fd_get_locked(dfs_fdtable_get(), fd);
    if (d && d->fnode)
    {
        d->ref_count++;
    }
    else
    {
        d = RT_NULL;
    }
    dfs_fd_unlock();
    return d;
}

/* Caller holds fdlock and has removed the last reference. */
static void fd_free(struct dfs_fd *d)
{
    struct dfs_fnode *fnode = d->fnode;

    if (fnode && --fnode->ref_count == 0)
    {
        rt_free(fnode);
    }
    d->magic = 0;
    rt_free(d);
}

/* Release an operation pin or detached table reference. The last reference
 * may close the backend after close() has already returned success. Retryable
 * failures, including interrupted socket draining, are logged and retained
 * for a later allocation/close to retry; they cannot change that earlier
 * close result. Preserve the current operation's errno in either case. */
void fd_put(struct dfs_fd *d)
{
    /* Taking fdlock resets thread->error; releasing a reference must preserve
     * the preceding file or socket operation's errno on every path. */
    int saved_errno = rt_get_errno();
    int result;

    if (!d)
    {
        return;
    }
    dfs_fd_lock();
    RT_ASSERT(d->ref_count > 0);
    if (d->ref_count > 1)
    {
        d->ref_count--;
        dfs_fd_unlock();
        rt_set_errno(saved_errno);
        return;
    }
    /* No table entry or other operation owns this descriptor now. Keep its
     * final reference until the backend has consumed the private state. */
    dfs_fd_unlock();
    result = dfs_file_close(d);
    if (result < 0 && !(d->flags & DFS_F_CLOSE_TERMINAL))
    {
        LOG_E("deferred file close failed: %d", result);
        /* The backend still owns private state, and fnode is still hashed.
         * Retain the final reference until a later allocation or close retries it;
         * freeing either object here would leave a dangling cache entry. */
        dfs_fd_lock();
        d->flags |= DFS_F_CLOSING;
        d->close_next = failed_closes;
        failed_closes = d;
        dfs_fd_unlock();
        rt_set_errno(saved_errno);
        return;
    }
    dfs_fd_lock();
    d->ref_count--;
    fd_free(d);
    dfs_fd_unlock();
    rt_set_errno(saved_errno);
}

static void fd_retry_failed_closes(void)
{
    struct dfs_fd *pending;
    int saved_errno = rt_get_errno();

    dfs_fd_lock();
    if (retrying_closes || !failed_closes)
    {
        dfs_fd_unlock();
        rt_set_errno(saved_errno);
        return;
    }
    retrying_closes = RT_TRUE;
    pending = failed_closes;
    failed_closes = RT_NULL;
    dfs_fd_unlock();

    /* One attempt per retained descriptor. A failed retry is requeued by
     * fd_put(), never retried in this pass. Backends may themselves open a
     * file; retrying_closes prevents recursion and concurrent drainers. */
    while (pending)
    {
        struct dfs_fd *next = pending->close_next;

        pending->close_next = RT_NULL;
        fd_put(pending);
        pending = next;
    }
    dfs_fd_lock();
    retrying_closes = RT_FALSE;
    dfs_fd_unlock();
    rt_set_errno(saved_errno);
}

int fdt_fd_close(struct dfs_fdtable *fdt, int fd, int type)
{
    struct dfs_fd *d;
    int result;

    /* Explicit file and socket closes also retry retired handles once.
     * The retry guard allows a backend close to reenter DFS safely. */
    fd_retry_failed_closes();

    dfs_fd_lock();
    d = fdt_fd_get_locked(fdt, fd);
    if (!d || !d->fnode)
    {
        dfs_fd_unlock();
        return -EBADF;
    }
    if (type >= 0 && d->fnode->type != type)
    {
        dfs_fd_unlock();
        return -ENOTSOCK;
    }
    if (d->ref_count > 1)
    {
        /* Success here only removes this table entry. Other aliases or I/O
         * pins keep the backend alive. A later fd_put() may fail to close
         * it (e.g. EINTR while draining sends) and queue a logged retry. */
        fdt->fds[fd] = RT_NULL;
        d->ref_count--;
        dfs_fd_unlock();
        return 0;
    }
    /* Reserve the slot while a retryable close is in progress. New lookups
     * reject it, and no global descriptor lock spans a backend operation. */
    d->flags |= DFS_F_CLOSING;
    dfs_fd_unlock();
    result = dfs_file_close(d);
    dfs_fd_lock();
    if (result >= 0 || (d->flags & DFS_F_CLOSE_TERMINAL))
    {
        fdt->fds[fd] = RT_NULL;
        d->ref_count--;
        fd_free(d);
    }
    else
    {
        d->flags &= ~DFS_F_CLOSING;
    }
    dfs_fd_unlock();
    return result;
}

/**
 * @ingroup Fd
 *
 * This function will put the file descriptor.
 */
void fdt_fd_release(struct dfs_fdtable* fdt, int fd)
{
    struct dfs_fd *fd_slot = NULL;

    RT_ASSERT(fdt != NULL);

    dfs_fd_lock();

    if ((fd < 0) || (fd >= fdt->maxfd))
    {
        dfs_fd_unlock();
        return;
    }

    fd_slot = fdt->fds[fd];
    if (fd_slot == NULL)
    {
        dfs_fd_unlock();
        return;
    }
    fdt->fds[fd] = NULL;

    /* check fd */
    RT_ASSERT(fd_slot->magic == DFS_FD_MAGIC);

    fd_slot->ref_count--;

    /* clear this fd entry */
    if (fd_slot->ref_count == 0)
    {
        fd_free(fd_slot);
    }
    dfs_fd_unlock();
}

void fd_release(int fd)
{
    struct dfs_fdtable *fdt;

    fdt = dfs_fdtable_get();
    fdt_fd_release(fdt, fd);
}

int sys_dup(int oldfd)
{
    int newfd = -1;
    struct dfs_fdtable *fdt = NULL;

    dfs_fd_lock();
    /* check old fd */
    fdt = dfs_fdtable_get();
    if ((oldfd < 0) || (oldfd >= fdt->maxfd))
    {
        goto exit;
    }
    if (!fdt->fds[oldfd] || (fdt->fds[oldfd]->flags & DFS_F_CLOSING))
    {
        goto exit;
    }
    /* get a new fd */
    newfd = fd_slot_alloc(fdt, DFS_STDIO_OFFSET);
    if (newfd >= 0)
    {
        fdt->fds[newfd] = fdt->fds[oldfd];
        /* inc ref_count */
        fdt->fds[newfd]->ref_count++;
    }
exit:
    dfs_fd_unlock();
    return newfd;
}

int sys_dup2(int oldfd, int newfd)
{
    struct dfs_fdtable *fdt = NULL;
    struct dfs_fd *replaced = RT_NULL;
    int retfd = -1;

    dfs_fd_lock();
    /* check old fd */
    fdt = dfs_fdtable_get();
    if ((oldfd < 0) || (oldfd >= fdt->maxfd))
    {
        goto exit;
    }
    if (!fdt->fds[oldfd] || (fdt->fds[oldfd]->flags & DFS_F_CLOSING))
    {
        goto exit;
    }
    if (newfd < 0)
    {
        goto exit;
    }
    if (newfd >= fdt->maxfd)
    {
        newfd = fd_slot_expand(fdt, newfd);
        if (newfd < 0)
        {
            goto exit;
        }
    }
    if (fdt->fds[newfd] == fdt->fds[oldfd])
    {
        /* ok, return newfd */
        retfd = newfd;
        goto exit;
    }

    if (fdt->fds[newfd])
    {
        if (fdt->fds[newfd]->flags & DFS_F_CLOSING)
            goto exit;
        replaced = fdt->fds[newfd];
    }

    fdt->fds[newfd] = fdt->fds[oldfd];
    /* inc ref_count */
    fdt->fds[newfd]->ref_count++;
    retfd = newfd;
exit:
    dfs_fd_unlock();
    /* dup2 atomically replaces the slot, then releases the old ownership.
     * A writer holding a reference continues on the original descriptor. */
    fd_put(replaced);
    return retfd;
}

static int fd_get_fd_index_form_fdt(struct dfs_fdtable *fdt, struct dfs_fd *file)
{
    int fd = -1;

    if (file == RT_NULL)
    {
        return -1;
    }

    dfs_fd_lock();

    for(int index = 0; index < (int)fdt->maxfd; index++)
    {
        if(fdt->fds[index] == file)
        {
            fd = index;
            break;
        }
    }

    dfs_fd_unlock();

    return fd;
}

int fd_get_fd_index(struct dfs_fd *file)
{
    struct dfs_fdtable *fdt;

    fdt = dfs_fdtable_get();
    return fd_get_fd_index_form_fdt(fdt, file);
}

int fd_associate(struct dfs_fdtable *fdt, int fd, struct dfs_fd *file)
{
    int retfd = -1;

    if (!file)
    {
        return retfd;
    }
    if (!fdt)
    {
        return retfd;
    }

    dfs_fd_lock();
    /* check old fd */
    if ((fd < 0) || (fd >= fdt->maxfd))
    {
        goto exit;
    }

    /* The destination must be empty. Independently, file is the source
     * descriptor, possibly shared with another table: do not add a reference
     * while its backend close is in progress or queued for retry. An occupied
     * destination is rejected regardless of that destination's flags. */
    if (fdt->fds[fd] || (file->flags & DFS_F_CLOSING))
    {
        goto exit;
    }
    /* inc ref_count */
    file->ref_count++;
    fdt->fds[fd] = file;
    retfd = fd;
exit:
    dfs_fd_unlock();
    return retfd;
}

void fd_init(struct dfs_fd *fd)
{
    if (fd)
    {
        fd->magic = DFS_FD_MAGIC;
        fd->ref_count = 1;
        fd->pos = 0;
        fd->fnode = NULL;
        fd->data = NULL;
    }
}

/**
 * this function will return a sub-path name under directory.
 *
 * @param directory the parent directory.
 * @param filename the filename.
 *
 * @return the subdir pointer in filename
 */
const char *dfs_subdir(const char *directory, const char *filename)
{
    const char *dir;

    if (strlen(directory) == strlen(filename)) /* it's a same path */
        return NULL;

    dir = filename + strlen(directory);
    if ((*dir != '/') && (dir != filename))
    {
        dir--;
    }

    return dir;
}
RTM_EXPORT(dfs_subdir);

/**
 * this function will normalize a path according to specified parent directory
 * and file name.
 *
 * @param directory the parent path
 * @param filename the file name
 *
 * @return the built full file path (absolute path)
 */
char *dfs_normalize_path(const char *directory, const char *filename)
{
    char *fullpath;
    char *dst0, *dst, *src;

    /* check parameters */
    RT_ASSERT(filename != NULL);

#ifdef DFS_USING_WORKDIR
    if (directory == NULL) /* shall use working directory */
    {
#ifdef RT_USING_LWP
        directory = lwp_getcwd();
#else
        directory = &working_directory[0];
#endif
    }
#else
    if ((directory == NULL) && (filename[0] != '/'))
    {
        rt_kprintf(NO_WORKING_DIR);

        return NULL;
    }
#endif

    if (filename[0] != '/') /* it's a absolute path, use it directly */
    {
        fullpath = (char *)rt_malloc(strlen(directory) + strlen(filename) + 2);

        if (fullpath == NULL)
            return NULL;

        /* join path and file name */
        rt_snprintf(fullpath, strlen(directory) + strlen(filename) + 2,
                    "%s/%s", directory, filename);
    }
    else
    {
        fullpath = rt_strdup(filename); /* copy string */

        if (fullpath == NULL)
            return NULL;
    }

    src = fullpath;
    dst = fullpath;

    dst0 = dst;
    while (1)
    {
        char c = *src;

        if (c == '.')
        {
            if (!src[1]) src++; /* '.' and ends */
            else if (src[1] == '/')
            {
                /* './' case */
                src += 2;

                while ((*src == '/') && (*src != '\0'))
                    src++;
                continue;
            }
            else if (src[1] == '.')
            {
                if (!src[2])
                {
                    /* '..' and ends case */
                    src += 2;
                    goto up_one;
                }
                else if (src[2] == '/')
                {
                    /* '../' case */
                    src += 3;

                    while ((*src == '/') && (*src != '\0'))
                        src++;
                    goto up_one;
                }
            }
        }

        /* copy up the next '/' and erase all '/' */
        while ((c = *src++) != '\0' && c != '/')
            *dst++ = c;

        if (c == '/')
        {
            *dst++ = '/';
            while (c == '/')
                c = *src++;

            src--;
        }
        else if (!c)
            break;

        continue;

up_one:
        /* keep the topmost root directory */
        if (dst - dst0 != 1 || dst[-1] != '/')
        {
            dst--;

            if (dst < dst0)
            {
                rt_free(fullpath);
                return NULL;
            }
        }
        while (dst0 < dst && dst[-1] != '/')
            dst--;
    }

    *dst = '\0';

    /* remove '/' in the end of path if exist */
    dst--;
    if ((dst != fullpath) && (*dst == '/'))
        *dst = '\0';

    /* final check fullpath is not empty, for the special path of lwext "/.." */
    if ('\0' == fullpath[0])
    {
        fullpath[0] = '/';
        fullpath[1] = '\0';
    }

    return fullpath;
}
RTM_EXPORT(dfs_normalize_path);

/**
 * This function will get the file descriptor table of current process.
 */
struct dfs_fdtable *dfs_fdtable_get(void)
{
    struct dfs_fdtable *fdt;
#ifdef RT_USING_LWP
    struct rt_lwp *lwp;

    lwp = (struct rt_lwp *)rt_thread_self()->lwp;
    if (lwp)
        fdt = &lwp->fdt;
    else
        fdt = &_fdtab;
#else
    fdt = &_fdtab;
#endif

    return fdt;
}

#ifdef RT_USING_LWP
struct dfs_fdtable *dfs_fdtable_get_pid(int pid)
{
    struct rt_lwp *lwp = RT_NULL;
    struct dfs_fdtable *fdt = RT_NULL;

    lwp = lwp_from_pid(pid);
    if (lwp)
    {
        fdt = &lwp->fdt;
    }

    return fdt;
}
#endif

struct dfs_fdtable *dfs_fdtable_get_global(void)
{
    return &_fdtab;
}

#ifdef RT_USING_FINSH
#include <finsh.h>
int list_fd(void)
{
    int index;
    struct dfs_fdtable *fd_table;

    fd_table = dfs_fdtable_get();
    if (!fd_table) return -1;

    rt_enter_critical();

    rt_kprintf("fd type    ref magic  path\n");
    rt_kprintf("-- ------  --- ----- ------\n");
    for (index = 0; index < (int)fd_table->maxfd; index++)
    {
        struct dfs_fd *fd = fd_table->fds[index];

        if (fd && fd->fnode->fops)
        {
            rt_kprintf("%2d ", index);
            if (fd->fnode->type == FT_DIRECTORY)    rt_kprintf("%-7.7s ", "dir");
            else if (fd->fnode->type == FT_REGULAR) rt_kprintf("%-7.7s ", "file");
            else if (fd->fnode->type == FT_SOCKET)  rt_kprintf("%-7.7s ", "socket");
            else if (fd->fnode->type == FT_USER)    rt_kprintf("%-7.7s ", "user");
            else if (fd->fnode->type == FT_DEVICE)  rt_kprintf("%-7.7s ", "device");
            else rt_kprintf("%-8.8s ", "unknown");
            rt_kprintf("%3d ", fd->fnode->ref_count);
            rt_kprintf("%04x  ", fd->magic);
            if (fd->fnode->path)
            {
                rt_kprintf("%s\n", fd->fnode->path);
            }
            else
            {
                rt_kprintf("\n");
            }
        }
    }
    rt_exit_critical();

    return 0;
}
MSH_CMD_EXPORT(list_fd, list file descriptor);

#ifdef RT_USING_LWP
static int lsofp(int pid)
{
    int index;
    struct dfs_fdtable *fd_table = RT_NULL;

    if (pid == (-1))
    {
        fd_table = dfs_fdtable_get();
        if (!fd_table) return -1;
    }
    else
    {
        fd_table = dfs_fdtable_get_pid(pid);
        if (!fd_table)
        {
            rt_kprintf("PID %s is not a applet(lwp)\n", pid);
            return -1;
        }
    }

    rt_kprintf("--- -- ------  ------ ----- ---------- ---------- ---------- ------\n");

    rt_enter_critical();
    for (index = 0; index < (int)fd_table->maxfd; index++)
    {
        struct dfs_fd *fd = fd_table->fds[index];

        if (fd && fd->fnode->fops)
        {
            if(pid == (-1))
            {
                rt_kprintf("  K ");
            }
            else
            {
                rt_kprintf("%3d ", pid);
            }

            rt_kprintf("%2d ", index);
            if (fd->fnode->type == FT_DIRECTORY)    rt_kprintf("%-7.7s ", "dir");
            else if (fd->fnode->type == FT_REGULAR) rt_kprintf("%-7.7s ", "file");
            else if (fd->fnode->type == FT_SOCKET)  rt_kprintf("%-7.7s ", "socket");
            else if (fd->fnode->type == FT_USER)    rt_kprintf("%-7.7s ", "user");
            else if (fd->fnode->type == FT_DEVICE)  rt_kprintf("%-7.7s ", "device");
            else rt_kprintf("%-8.8s ", "unknown");
            rt_kprintf("%6d ", fd->fnode->ref_count);
            rt_kprintf("%04x  0x%.8x ", fd->magic, (int)(size_t)fd->fnode);

            if(fd->fnode == RT_NULL)
            {
                rt_kprintf("0x%.8x 0x%.8x ", (int)0x00000000, (int)(size_t)fd);
            }
            else
            {
                rt_kprintf("0x%.8x 0x%.8x ", (int)(size_t)(fd->fnode->data), (int)(size_t)fd);
            }

            if (fd->fnode->path)
            {
                rt_kprintf("%s \n", fd->fnode->path);
            }
            else
            {
                rt_kprintf("\n");
            }
        }
    }
    rt_exit_critical();

    return 0;
}

int lsof(int argc, char *argv[])
{
    rt_kprintf("PID fd type    fd-ref magic fnode      fnode/data addr       path  \n");

    if (argc == 1)
    {
        struct rt_list_node *node, *list;
        struct lwp_avl_struct *pids = lwp_get_pid_ary();

        lsofp(-1);

        for (int index = 0; index < RT_LWP_MAX_NR; index++)
        {
            struct rt_lwp *lwp = (struct rt_lwp *)pids[index].data;

            if (lwp)
            {
                list = &lwp->t_grp;
                for (node = list->next; node != list; node = node->next)
                {
                    lsofp(lwp_to_pid(lwp));
                }
            }
        }
    }
    else if (argc == 3)
    {
        if (argv[1][0] == '-' && argv[1][1] == 'p')
        {
            int pid = atoi(argv[2]);
            lsofp(pid);
        }
    }

    return 0;
}
MSH_CMD_EXPORT(lsof, list open files);
#endif /* RT_USING_LWP */

/*
 * If no argument is specified, display the mount history;
 * If there are 3 arguments, mount the filesystem.
 * The order of the arguments is:
 * argv[1]: device name
 * argv[2]: mountpoint path
 * argv[3]: filesystem type
 */
int mount(int argc, char *argv[])
{
    int ret = -1;
    if (argc == 1) /* display the mount history */
    {
        struct dfs_filesystem *iter;

        rt_kprintf("filesystem  device  mountpoint\n");
        rt_kprintf("----------  ------  ----------\n");
        for (iter = &filesystem_table[0];
                iter < &filesystem_table[DFS_FILESYSTEMS_MAX]; iter++)
        {
            if ((iter != NULL) && (iter->path != NULL))
            {
                rt_kprintf("%-10s  %-6s  %-s\n",
                        iter->ops->name, iter->dev_id->parent.name, iter->path);
            }
        }
        return 0;
    }
    else if (argc == 4)
    { /* mount a filesystem to the specified directory */
        char *device = argv[1];
        char *path = argv[2];
        char *fstype = argv[3];

        rt_kprintf("mount device %s(%s) onto %s ... ", device, fstype, path);
#ifdef RT_USING_DFS_NFS
        if(strcmp(fstype, "nfs") == 0)
        {
            ret = dfs_mount(RT_NULL, path, fstype, 0, device);
        } else
#endif
        ret = dfs_mount(device, path, fstype, 0, 0);
        if (ret == 0)
        {
            rt_kprintf("succeed!\n");
            return 0;
        }
        else
        {
            rt_kprintf("failed!\n");
            return -1;
        }
    }
    else
    {
        rt_kprintf("Usage: mount <device> <mountpoint> <fstype>.\n");
        return -1;
    }
}
MSH_CMD_EXPORT(mount, mount<device><mountpoint><fstype>);

/* unmount the filesystem from the specified mountpoint */
int unmount(int argc, char *argv[])
{
    if (argc != 2)
    {
        rt_kprintf("Usage: unmount <mountpoint>.\n");
        return -1;
    }

    char *path = argv[1];

    rt_kprintf("unmount %s ... ", path);
    if (dfs_unmount(path) < 0)
    {
        rt_kprintf("failed!\n");
        return -1;
    } else {
        rt_kprintf("succeed!\n");
        return 0;
    }
}
MSH_CMD_EXPORT(unmount, unmount the mountpoint);

#endif
/*@}*/

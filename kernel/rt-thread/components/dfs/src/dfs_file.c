/*
 * Copyright (c) 2006-2018, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2005-02-22     Bernard      The first version.
 * 2011-12-08     Bernard      Merges rename patch from iamcacy.
 * 2015-05-27     Bernard      Fix the fd clear issue.
 * 2019-01-24     Bernard      Remove file repeatedly open check.
 */

#include <dfs.h>
#include <dfs_file.h>
#include <dfs_private.h>
#ifdef RT_USING_LWP
/* Not <lwp.h>: it declares dfs_posix.h's read()/write(), which conflict
 * with <unistd.h> below. */
struct rt_lwp;
struct rt_lwp *lwp_self(void);
pid_t lwp_to_pid(struct rt_lwp *lwp);
#endif
#include <unistd.h>

#define DFS_FNODE_HASH_NR 128

/*
 * The fnode lock guards the hash, each fnode's ref_count, busy, opening and
 * close_waiting, and the waiter and opener lists below. It is never held
 * across a backend call: a backend open or close may sleep (draining
 * hardware, flushing a disk, a socket), and holding the lock would stall
 * every open and close in the system. Instead, per path:
 *
 * - the first open of a path and every close run alone (fnode->busy);
 * - later opens are counted (fnode->opening) and also run one at a time,
 *   except that a thread may open the path again from inside its own
 *   later open. Drivers' open callbacks assume opens of their path do not
 *   overlap (tty_open(), rt_device_open()'s unlocked ref_count). A close
 *   waits for them, so the backend's ref_count test for its last close
 *   sees only completed opens. While a close waits (fnode->close_waiting),
 *   new later opens wait behind it, so a stream of opens cannot starve it.
 *
 * A thread that has to wait queues a waiter record of its own, with its own
 * semaphore, and sleeps on it; whoever changes an fnode's state wakes every
 * queued waiter, and each looks its fnode up again. Waits are rare
 * (contention on one path), so waking them all is cheap, and no waiter can
 * take another's wakeup. Unlike the old global mutex this gives no priority
 * inheritance to the thread running the backend.
 *
 * A thread waiting for its own first open or close of a path, or for an open
 * of the path it has in flight (a backend that reaches the path again, for
 * example through a deferred close retried by fd_new()), gets -EDEADLK.
 * Cycles across paths are not detected: a backend's open or close must not
 * open or close another path whose backend can, in turn, reach its own
 * (none in the tree does).
 */
struct dfs_fnode_waiter
{
    rt_list_t node;
    struct rt_semaphore sem;
};

/* A later open of fnode in flight in thread, for the -EDEADLK check. */
struct dfs_fnode_opener
{
    rt_list_t node;
    struct dfs_fnode *fnode;
    rt_thread_t thread;
};

struct dfs_fnode_mgr
{
    struct rt_mutex lock;
    rt_list_t head[DFS_FNODE_HASH_NR];
    rt_list_t waiters;
    rt_list_t openers;
};

static struct dfs_fnode_mgr dfs_fm;

#if defined (CHERRY_USB_DEVICE_ENABLE_CLASS_MTP)
#include "inotify.h"
#define DFS_F_MTP_DIRTY 0x10000000U
#endif

void dfs_fm_lock(void)
{
    rt_mutex_take(&dfs_fm.lock, RT_WAITING_FOREVER);
}

void dfs_fm_unlock(void)
{
    rt_mutex_release(&dfs_fm.lock);
}

void dfs_fnode_mgr_init(void)
{
    int i = 0;

    rt_mutex_init(&dfs_fm.lock, "dfs_mgr", RT_IPC_FLAG_PRIO);
    rt_list_init(&dfs_fm.waiters);
    rt_list_init(&dfs_fm.openers);
    for (i = 0; i < DFS_FNODE_HASH_NR; i++)
    {
        rt_list_init(&dfs_fm.head[i]);
    }
}

/* Caller holds the fnode lock. Wakes every thread waiting for an fnode; the
 * waiter's record leaves the list here, before its semaphore is released. */
static void dfs_fnode_wake_locked(void)
{
    while (!rt_list_isempty(&dfs_fm.waiters))
    {
        struct dfs_fnode_waiter *w =
            rt_list_entry(dfs_fm.waiters.next, struct dfs_fnode_waiter, node);

        rt_list_remove(&w->node);
        rt_sem_release(&w->sem);
    }
}

/* Caller holds the fnode lock. Whether this thread has a later open of
 * @fnode in flight. */
static rt_bool_t dfs_fnode_opening_here_locked(struct dfs_fnode *fnode)
{
    rt_list_t *n;
    rt_thread_t self = rt_thread_self();

    for (n = dfs_fm.openers.next; n != &dfs_fm.openers; n = n->next)
    {
        struct dfs_fnode_opener *o = rt_list_entry(n, struct dfs_fnode_opener, node);

        if (o->fnode == fnode && o->thread == self)
        {
            return RT_TRUE;
        }
    }
    return RT_FALSE;
}

/* Caller holds the fnode lock, and @fnode is busy, has opens in flight or a
 * close waiting. Drops the lock, sleeps until some fnode changes state, and
 * takes the lock again; the caller checks again. @fnode may be freed
 * meanwhile unless the caller holds a reference on it. A thread that would
 * wait for itself gets -EDEADLK. */
static int dfs_fnode_wait_locked(struct dfs_fnode *fnode)
{
    struct dfs_fnode_waiter w;
    rt_err_t taken;

    if ((fnode->busy && fnode->busy_owner == rt_thread_self()) ||
        ((fnode->opening || fnode->close_waiting) && dfs_fnode_opening_here_locked(fnode)))
    {
        return -EDEADLK;
    }
    rt_sem_init(&w.sem, "dfs_fwt", 0, RT_IPC_FLAG_PRIO);
    rt_list_insert_before(&dfs_fm.waiters, &w.node);
    dfs_fm_unlock();
    /* Uninterruptible, and nobody else detaches w.sem: it cannot fail,
     * which would leave w linked after this frame is gone. */
    taken = rt_sem_take(&w.sem, RT_WAITING_FOREVER);
    RT_ASSERT(taken == RT_EOK);
    (void)taken;
    dfs_fm_lock();
    /* dfs_fnode_wake_locked() unlinked w before the release, and has left
     * the semaphore: it held the lock we just took. */
    rt_sem_detach(&w.sem);
    return 0;
}

static void dfs_fnode_set_busy_locked(struct dfs_fnode *fnode, rt_uint8_t state)
{
    fnode->busy = state;
    fnode->busy_owner = rt_thread_self();
}

static void dfs_fnode_clear_busy_locked(struct dfs_fnode *fnode)
{
    fnode->busy = 0;
    fnode->busy_owner = RT_NULL;
    dfs_fnode_wake_locked();
}

static void dfs_flock_free_all_locked(struct dfs_fnode *fnode);

/* Caller holds the fnode lock; @fnode is out of the hash or about to be. */
static void dfs_fnode_free_locked(struct dfs_fnode *fnode)
{
    dfs_flock_free_all_locked(fnode);
    rt_list_remove(&fnode->list);
    if (fnode->path != fnode->fullpath)
    {
        rt_free(fnode->fullpath);
    }
    rt_free(fnode->path);
    rt_free(fnode);
}

/* BKDR Hash Function */
static unsigned int bkdr_hash(const char *str)
{
    unsigned int seed = 131; // 31 131 1313 13131 131313 etc..
    unsigned int hash = 0;

    while (*str)
    {
        hash = hash * seed + (*str++);
    }

    return (hash % DFS_FNODE_HASH_NR);
}

static struct dfs_fnode *dfs_fnode_find(const char *path, rt_list_t **hash_head)
{
    struct dfs_fnode *fnode = NULL;
    int hash = bkdr_hash(path);
    rt_list_t *hh;

    hh = dfs_fm.head[hash].next;

    if (hash_head)
    {
        *hash_head = &dfs_fm.head[hash];
    }

    while (hh != &dfs_fm.head[hash])
    {
        fnode = rt_container_of(hh, struct dfs_fnode, list);
        if (rt_strcmp(path, fnode->fullpath) == 0)
        {
            /* found */
            return fnode;
        }
        hh = hh->next;
    }
    return NULL;
}

/**
 * @addtogroup FileApi
 */

/*@{*/

/**
 * This function will return whether this file has been opend.
 *
 * @param pathname the file path name.
 *
 * @return 0 on file has been open successfully, -1 on open failed.
 */
int dfs_file_is_open(const char *pathname)
{
    char *fullpath = NULL;
    struct dfs_fnode *fnode = NULL;
    int ret = 0;

    fullpath = dfs_normalize_path(NULL, pathname);

    dfs_fm_lock();
    /* A path whose close or first open is running is neither open nor
     * closed yet: wait for it, as the fnode lock used to. */
    while ((fnode = dfs_fnode_find(fullpath, NULL)) != NULL && fnode->busy)
    {
        if (dfs_fnode_wait_locked(fnode) < 0)
        {
            break;
        }
    }
    if (fnode)
    {
        ret = 1;
    }
    dfs_fm_unlock();

    rt_free(fullpath);
    return ret;
}


/**
 * this function will open a file which specified by path with specified flags.
 *
 * @param fd the file descriptor pointer to return the corresponding result.
 * @param path the specified file path.
 * @param flags the flags for open operator.
 *
 * @return 0 on successful, -1 on failed.
 */
static int dfs_file_do_open(struct dfs_fd *fd, const char *path, int flags);

/* The descriptor is hidden from other threads while it opens: a close()
 * of its number from another thread used to reach it with the fnode set
 * but the backend open still running, and read the fnode after a failed
 * open had freed it. fdt_fd_get_locked() refuses DFS_F_OPENING. */
int dfs_file_open(struct dfs_fd *fd, const char *path, int flags)
{
    int result;

    if (fd == NULL)
        return -EINVAL;
    dfs_fd_lock();
    fd->flags |= DFS_F_OPENING;
    dfs_fd_unlock();
    result = dfs_file_do_open(fd, path, flags);
    dfs_fd_lock();
    fd->flags &= ~DFS_F_OPENING;
    dfs_fd_unlock();
    return result;
}

static int dfs_file_do_open(struct dfs_fd *fd, const char *path, int flags)
{
    struct dfs_filesystem *fs;
    char *fullpath;
    int result;
    int first_open = 0;
    int revive = 0;
    struct dfs_fnode *fnode = NULL;
    struct dfs_fnode_opener opener;
    rt_list_t *hash_head;

#if defined (CHERRY_USB_DEVICE_ENABLE_CLASS_MTP)
    int notify_type = 0;
    char *notify_path = RT_NULL;
#endif // CHERRY_USB_DEVICE_ENABLE_CLASS_MTP

    /* parameter check */
    if (fd == NULL)
        return -EINVAL;

#ifdef O_TMPFILE
    /* No filesystem here has unnamed files: this opened the directory
     * itself. Linux answers EOPNOTSUPP for such a filesystem. */
    if ((flags & O_TMPFILE) == O_TMPFILE)
        return -EOPNOTSUPP;
#endif

    /* make sure we have an absolute path */
    fullpath = dfs_normalize_path(NULL, path);
    if (fullpath == NULL)
    {
        return -ENOMEM;
    }

    LOG_D("open file:%s", fullpath);

    dfs_fm_lock();

    /* fnode find; wait out a first open or a close of the path, and a close
     * waiting for the opens in flight */
    while ((fnode = dfs_fnode_find(fullpath, &hash_head)) != NULL &&
           (fnode->busy ||
            /* Later opens of a path run one at a time too: drivers' open
             * callbacks (tty_open(), rt_device_open()'s ref_count) assume
             * it. A thread with an open of the path in flight opening it
             * again goes ahead, as under the old recursive lock; a waiting
             * close waits for that open anyway. */
            ((fnode->opening || fnode->close_waiting) &&
             !dfs_fnode_opening_here_locked(fnode))))
    {
        result = dfs_fnode_wait_locked(fnode);
        if (result < 0)
        {
            dfs_fm_unlock();
            rt_free(fullpath);
            return result;
        }
    }
    if (fnode && (fnode->flags & DFS_FNODE_F_UNLINKED))
    {
        if (!(flags & O_CREAT))
        {
            /* Unlinked while open: the name is gone. */
            dfs_fm_unlock();
            rt_free(fullpath);
            return -ENOENT;
        }
        /* Creating the name again: the backend still has the old file under
         * it, so take that back (O_TRUNC empties it) and cancel the delete.
         * "unlink(); fopen(name, "w")" keeps working. */
        revive = 1;
    }
    if (fnode)
    {
        fnode->ref_count++;
        fnode->opening++;
        opener.fnode = fnode;
        opener.thread = rt_thread_self();
        rt_list_insert_before(&dfs_fm.openers, &opener.node);
        fd->pos   = 0;
        fd->fnode = fnode;
        dfs_fm_unlock();
        rt_free(fullpath); /* release path */
        fullpath = NULL;
    }
    else
    {
        /* find filesystem */
        fs = dfs_filesystem_lookup(fullpath);
        if (fs == NULL)
        {
            dfs_fm_unlock();
            rt_free(fullpath); /* release path */
            return -ENOENT;
        }

        fnode = rt_calloc(1, sizeof(struct dfs_fnode));
        if (!fnode)
        {
            dfs_fm_unlock();
            rt_free(fullpath); /* release path */
            return -ENOMEM;
        }
        fnode->ref_count = 1;

        LOG_D("open in filesystem:%s", fs->ops->name);
        fnode->fs    = fs;             /* set file system */
        fnode->fops  = fs->ops->fops;  /* set file ops */

        /* initialize the fd item */
        fnode->type  = FT_REGULAR;
        fnode->flags = 0;

        if (!(fs->ops->flags & DFS_FS_FLAG_FULLPATH))
        {
            if (dfs_subdir(fs->path, fullpath) == NULL)
                fnode->path = rt_strdup("/");
            else
                fnode->path = rt_strdup(dfs_subdir(fs->path, fullpath));
            LOG_D("Actual file path: %s", fnode->path);
        }
        else
        {
            fnode->path = fullpath;
        }
        fnode->fullpath = fullpath;

        /* specific file system open routine */
        if (fnode->fops->open == NULL)
        {
            dfs_fm_unlock();
            /* clear fd */
            if (fnode->path != fnode->fullpath)
            {
                rt_free(fnode->fullpath);
            }
            rt_free(fnode->path);
            rt_free(fnode);

            return -ENOSYS;
        }

        fd->pos   = 0;
        fd->fnode = fnode;

        /* insert fnode to hash; other opens and closes of the path wait
         * until the backend's first open is done */
        rt_list_insert_after(hash_head, &fnode->list);
        dfs_fnode_set_busy_locked(fnode, DFS_FNODE_OPENING);
        first_open = 1;
        dfs_fm_unlock();
    }

    /* User flags cannot set DFS's own bits (DFS_F_CLOSING would hide the
     * descriptor from close() for good). */
    fd->flags = (flags & ~DFS_F_INTERNAL_MASK) | DFS_F_OPENING;

    /* For an open device this is the driver's fops->open: devfs only sees
     * the first open of a path. */
    if ((result = fnode->fops->open(fd)) < 0)
    {
        result = dfs_open_errno(result);
        LOG_D("%s open failed", path);

        dfs_fm_lock();
        if (first_open)
        {
            dfs_fnode_clear_busy_locked(fnode);
        }
        else
        {
            rt_list_remove(&opener.node);
            if (--fnode->opening == 0)
            {
                dfs_fnode_wake_locked();
            }
        }
        fnode->ref_count--;
        if (fnode->ref_count == 0)
        {
            dfs_fnode_free_locked(fnode);
        }
        fd->fnode = NULL;
        dfs_fm_unlock();

        return result;
    }

    if (revive)
    {
        dfs_fm_lock();
        fnode->flags &= ~DFS_FNODE_F_UNLINKED;
        dfs_fm_unlock();
    }
    fd->flags |= DFS_F_OPEN;
    if (flags & O_DIRECTORY)
    {
        fd->fnode->type = FT_DIRECTORY;
        fd->flags |= DFS_F_DIRECTORY;
    }

#if defined (CHERRY_USB_DEVICE_ENABLE_CLASS_MTP)
    if (flags & O_TRUNC)
    {
        notify_type = NTY_FILE_CHG;
        notify_path = rt_strdup(fnode->fullpath);
    }
    else if (flags & O_CREAT)
    {
        notify_type = NTY_FILE_ADD;
        notify_path = rt_strdup(fnode->fullpath);
    }
#endif // CHERRY_USB_DEVICE_ENABLE_CLASS_MTP

    dfs_fm_lock();
    if (first_open)
    {
        dfs_fnode_clear_busy_locked(fnode);
    }
    else
    {
        rt_list_remove(&opener.node);
        if (--fnode->opening == 0)
        {
            dfs_fnode_wake_locked();
        }
    }
    dfs_fm_unlock();

#if defined (CHERRY_USB_DEVICE_ENABLE_CLASS_MTP)
    if (notify_type && notify_path)
    {
        inotify_handler_filechange(notify_type, notify_path);
        rt_free(notify_path);
    }
#endif // CHERRY_USB_DEVICE_ENABLE_CLASS_MTP

    LOG_D("open successful");
    return 0;
}

/**
 * this function will close a file descriptor.
 *
 * @param fd the file descriptor to be closed.
 *
 * @return 0 on successful, -1 on failed.
 */
static int dfs_file_do_close(struct dfs_fd *fd, rt_bool_t nowait);

int dfs_file_close(struct dfs_fd *fd)
{
    return dfs_file_do_close(fd, RT_FALSE);
}

/* As dfs_file_close(), but -EBUSY instead of waiting for an open or close of
 * the path in flight. For retrying deferred closes from fd_new() and close():
 * an open of an unrelated path must not wait behind a busy one, and a retry
 * reached from inside a backend's open of the same path must not wait for
 * itself. */
int dfs_file_close_nowait(struct dfs_fd *fd)
{
    return dfs_file_do_close(fd, RT_TRUE);
}

static int dfs_file_do_close(struct dfs_fd *fd, rt_bool_t nowait)
{
    struct dfs_fnode *fnode = NULL;
    int result = 0;

#if defined (CHERRY_USB_DEVICE_ENABLE_CLASS_MTP)
    int notify_type = 0;
    char *notify_path = RT_NULL;
#endif // CHERRY_USB_DEVICE_ENABLE_CLASS_MTP

    if (fd == NULL || fd->fnode == NULL)
    {
        return -ENXIO;
    }

    if (fd->ref_count == 1)
    {
        dfs_fm_lock();

        fnode = fd->fnode;

        if (fnode->ref_count <= 0)
        {
            dfs_fm_unlock();
            return -ENXIO;
        }

        /* One close or first open of a path at a time, and no close while a
         * later open is in flight: the backend tells its last close from
         * fnode->ref_count, which must count completed opens only. This
         * descriptor's reference keeps fnode while we wait. */
        while (fnode->busy || fnode->opening)
        {
            /* New later opens wait behind a close waiting for opens. */
            rt_bool_t for_opens = fnode->opening != 0;

            if (nowait)
            {
                dfs_fm_unlock();
                return -EBUSY;
            }

            if (for_opens)
            {
                fnode->close_waiting++;
            }
            result = dfs_fnode_wait_locked(fnode);
            if (for_opens && --fnode->close_waiting == 0)
            {
                dfs_fnode_wake_locked();
            }
            if (result < 0)
            {
                dfs_fm_unlock();
                return result;
            }
        }
        dfs_fnode_set_busy_locked(fnode, DFS_FNODE_CLOSING);
        dfs_fm_unlock();

        if (fnode->fops->close != NULL)
        {
            result = fnode->fops->close(fd);
        }

        dfs_fm_lock();

        /* Most DFS backends retain their private handle on close failure and
         * expect the caller to retry. Only tear down a failed close when the
         * backend explicitly reports that its private state was consumed. */
        if (result < 0 && !(fd->flags & DFS_F_CLOSE_TERMINAL))
        {
            dfs_fnode_clear_busy_locked(fnode);
            dfs_fm_unlock();
            return result;
        }
        if (fnode->ref_count == 1 && (fnode->flags & DFS_FNODE_F_UNLINKED))
        {
            /* The last close of a file unlinked while open: delete it now,
             * with the path still busy, so no open can reach the old file
             * meanwhile. */
            fd->fnode = NULL;
            fnode->ref_count = 0;
            dfs_fm_unlock();
            if (fnode->fs && fnode->fs->ops->unlink)
            {
                int err = fnode->fs->ops->unlink(fnode->fs, fnode->path);

                if (err < 0)
                    LOG_E("deferred unlink of %s failed: %d", fnode->fullpath, err);
            }
            dfs_fm_lock();
            dfs_fnode_clear_busy_locked(fnode);
            dfs_fnode_free_locked(fnode);
            dfs_fm_unlock();
            return result;
        }
        dfs_fnode_clear_busy_locked(fnode);

#if defined (CHERRY_USB_DEVICE_ENABLE_CLASS_MTP)
        if (result >= 0 && (fd->flags & DFS_F_MTP_DIRTY))
        {
            notify_type = NTY_FILE_CHG;
            notify_path = rt_strdup(fnode->fullpath);
            fd->flags &= ~DFS_F_MTP_DIRTY;
        }
#endif // CHERRY_USB_DEVICE_ENABLE_CLASS_MTP

        /* The descriptor's reference goes here, in the same lock section
         * that ends the close, so the next close of the path reads the new
         * count. fd_free() no longer sees the fnode; descriptors that are
         * never passed to fd_free() (on the stack) drop theirs too. */
        fd->fnode = NULL;
        if (--fnode->ref_count == 0)
        {
            dfs_fnode_free_locked(fnode);
        }
        dfs_fm_unlock();

#if defined (CHERRY_USB_DEVICE_ENABLE_CLASS_MTP)
        if (notify_type && notify_path)
        {
            inotify_handler_filechange(notify_type, notify_path);
            rt_free(notify_path);
        }
#endif // CHERRY_USB_DEVICE_ENABLE_CLASS_MTP
    }

    return result;
}

/**
 * this function will perform a io control on a file descriptor.
 *
 * @param fd the file descriptor.
 * @param cmd the command to send to file descriptor.
 * @param args the argument to send to file descriptor.
 *
 * @return 0 on successful, -1 on failed.
 */
int dfs_file_ioctl(struct dfs_fd *fd, int cmd, void *args)
{
    if (fd == NULL)
    {
        return -EINVAL;
    }

    if (cmd == RT_FIOPREALLOCATE && fd->fnode->type == FT_REGULAR &&
        (fd->flags & O_ACCMODE) == O_RDONLY)
        return -EBADF;

    /* regular file system fd. Sockets and channels handle these in their
     * own ioctl (O_NONBLOCK on the socket). */
    if (fd->fnode->type == FT_REGULAR || fd->fnode->type == FT_DEVICE ||
        fd->fnode->type == FT_DIRECTORY)
    {
        switch (cmd)
        {
        case F_GETFL:
            /* Not DFS's own bits (DFS_F_OPEN would read as an open flag);
             * musl's fdopendir() tests F_GETFL of a directory. */
            return (fd->flags & ~DFS_F_INTERNAL_MASK) |
                   (fd->fnode->type == FT_DIRECTORY ? O_DIRECTORY : 0);
        case F_SETFL:
            {
                int flags = (int)(rt_base_t)args;
                int mask  = O_NONBLOCK | O_APPEND;

                flags &= mask;
                fd->flags &= ~mask;
                fd->flags |= flags;
            }
            return 0;
        }
    }

    if (fd->fnode->fops->ioctl != NULL)
    {
        return fd->fnode->fops->ioctl(fd, cmd, args);
    }

    return -ENOSYS;
}

/**
 * this function will read specified length data from a file descriptor to a
 * buffer.
 *
 * @param fd the file descriptor.
 * @param buf the buffer to save the read data.
 * @param len the length of data buffer to be read.
 *
 * @return the actual read data bytes or 0 on end of file or failed.
 */
int dfs_file_read(struct dfs_fd *fd, void *buf, size_t len)
{
    int result = 0;

    if (fd == NULL)
    {
        return -EINVAL;
    }

    if (fd->fnode->type == FT_REGULAR && (fd->flags & O_ACCMODE) == O_WRONLY)
        return -EBADF;

    if (fd->fnode->fops->read == NULL)
    {
        return -ENOSYS;
    }

    if ((result = fd->fnode->fops->read(fd, buf, len)) < 0)
    {
        fd->flags |= DFS_F_EOF;
    }

    return result;
}

/**
 * this function will fetch directory entries from a directory descriptor.
 *
 * @param fd the directory descriptor.
 * @param dirp the dirent buffer to save result.
 * @param nbytes the available room in the buffer.
 *
 * @return the read dirent, others on failed.
 */
int dfs_file_getdents(struct dfs_fd *fd, struct dirent *dirp, size_t nbytes)
{
    /* parameter check */
    if (fd == NULL)
    {
        return -EINVAL;
    }

    if (fd->fnode->type != FT_DIRECTORY)
    {
        return -ENOTDIR;
    }

    if (dirp == NULL)
    {
        return -EINVAL;
    }

    rt_memset(dirp, 0, nbytes);

    if (fd->fnode->fops->getdents != NULL)
    {
        return fd->fnode->fops->getdents(fd, dirp, nbytes);
    }

    return -ENOSYS;
}

/**
 * this function will unlink (remove) a specified path file from file system.
 *
 * @param path the specified path file to be unlinked.
 *
 * @return 0 on successful, -1 on failed.
 */
/* Whether @fullpath names a file unlinked while still open. */
static int dfs_path_unlinked(const char *fullpath)
{
    struct dfs_fnode *fnode;
    int gone;

    dfs_fm_lock();
    fnode = dfs_fnode_find(fullpath, NULL);
    gone = fnode && (fnode->flags & DFS_FNODE_F_UNLINKED);
    dfs_fm_unlock();
    return gone;
}

/* @defer: an open regular file is unlinked at its last close instead of
 * failing with EBUSY (POSIX unlink(); musl's tmpfile() unlinks the file it
 * keeps open). rename() replacing a file passes 0: it must not leave the
 * target to vanish later if the rename then fails. */
static int dfs_file_do_unlink(const char *path, int defer)
{
    int result;
    int writable;
    char *fullpath;
    struct dfs_filesystem *fs;
    struct dfs_fnode *fnode;

    /* Make sure we have an absolute path */
    fullpath = dfs_normalize_path(NULL, path);
    if (fullpath == NULL)
    {
        return -EINVAL;
    }

    /* Whether the backend could delete it later: FAT refuses read-only files,
     * so refuse them now rather than "succeed" and fail at the last close. */
    {
        struct stat st;

        writable = dfs_file_stat(fullpath, &st) != 0 ||
                   (st.st_mode & (S_IWUSR | S_IWGRP | S_IWOTH)) != 0;
    }

    /* Check whether file is already open */
    dfs_fm_lock();
    while ((fnode = dfs_fnode_find(fullpath, NULL)) != NULL && fnode->busy)
    {
        if (dfs_fnode_wait_locked(fnode) < 0)
            break;
    }
    if (fnode)
    {
        /* Deferred only for a file a filesystem deletes: not a device or a
         * procfs entry (their names must not vanish until a close that may
         * never come, e.g. the console), nor on one without unlink (romfs). */
        int deferrable = fnode->type != FT_DIRECTORY && fnode->fs && fnode->fs->ops &&
                         fnode->fs->ops->unlink &&
                         rt_strcmp(fnode->fs->ops->name, "devfs") != 0 &&
                         rt_strcmp(fnode->fs->ops->name, "procfs") != 0;

        if (fnode->flags & DFS_FNODE_F_UNLINKED)
            result = -ENOENT;
        else if (!defer || !deferrable || fnode->busy)
            result = -EBUSY;
        else if (!writable)
            result = -EACCES;
        else
        {
            fnode->flags |= DFS_FNODE_F_UNLINKED;
            result = 0;
        }
        dfs_fm_unlock();
        goto __exit;
    }
    dfs_fm_unlock();

    /* get filesystem */
    if ((fs = dfs_filesystem_lookup(fullpath)) == NULL)
    {
        result = -ENOENT;
        goto __exit;
    }

    if (fs->ops->unlink != NULL)
    {
        if (!(fs->ops->flags & DFS_FS_FLAG_FULLPATH))
        {
            if (dfs_subdir(fs->path, fullpath) == NULL)
                result = fs->ops->unlink(fs, "/");
            else
                result = fs->ops->unlink(fs, dfs_subdir(fs->path, fullpath));
        }
        else
            result = fs->ops->unlink(fs, fullpath);
    }
    else result = -ENOSYS;

__exit:
#if defined (CHERRY_USB_DEVICE_ENABLE_CLASS_MTP)
    if (result == 0)
        inotify_handler_filechange(NTY_FILE_RM, fullpath);
#endif // CHERRY_USB_DEVICE_ENABLE_CLASS_MTP

    rt_free(fullpath);
    return result;
}

int dfs_file_unlink(const char *path)
{
    return dfs_file_do_unlink(path, 1);
}

/**
 * this function will write some specified length data to file system.
 *
 * @param fd the file descriptor.
 * @param buf the data buffer to be written.
 * @param len the data buffer length
 *
 * @return the actual written data length.
 */
int dfs_file_write(struct dfs_fd *fd, const void *buf, size_t len)
{
    int result;

    if (fd == NULL)
    {
        return -EINVAL;
    }

    if (fd->fnode->type == FT_REGULAR && (fd->flags & O_ACCMODE) == O_RDONLY)
        return -EBADF;

    if (fd->fnode->fops->write == NULL)
    {
        return -ENOSYS;
    }

    result = fd->fnode->fops->write(fd, buf, len);

#if defined (CHERRY_USB_DEVICE_ENABLE_CLASS_MTP)
    if ((result > 0) && (fd->fnode->type == FT_REGULAR))
        fd->flags |= DFS_F_MTP_DIRTY;
#endif // CHERRY_USB_DEVICE_ENABLE_CLASS_MTP

    return result;
}

/**
 * this function will flush buffer on a file descriptor.
 *
 * @param fd the file descriptor.
 *
 * @return 0 on successful, -1 on failed.
 */
int dfs_file_flush(struct dfs_fd *fd)
{
    int result;

    if (fd == NULL)
        return -EINVAL;

    if (fd->fnode->fops->flush == NULL)
        return -ENOSYS;

    result = fd->fnode->fops->flush(fd);

#if defined (CHERRY_USB_DEVICE_ENABLE_CLASS_MTP)
    if ((result == 0) && (fd->flags & DFS_F_MTP_DIRTY))
    {
        fd->flags &= ~DFS_F_MTP_DIRTY;
        inotify_handler_filechange(NTY_FILE_CHG, fd->fnode->fullpath);
    }
#endif // CHERRY_USB_DEVICE_ENABLE_CLASS_MTP

    return result;
}

/**
 * this function will seek the offset for specified file descriptor.
 *
 * @param fd the file descriptor.
 * @param offset the offset to be sought.
 *
 * @return the current position after seek.
 */
off_t dfs_file_lseek(struct dfs_fd *fd, off_t offset)
{
    off_t result;

    if (fd == NULL)
        return -EINVAL;

    if (fd->fnode->fops->lseek == NULL)
        return -ESPIPE;     /* pipes, sockets, ttys: not seekable */

    result = fd->fnode->fops->lseek(fd, offset);
    if (result >= 0 && fd->fnode->fs && fd->fnode->fs->ops &&
        (fd->fnode->fs->ops->flags & DFS_FS_FLAG_LSEEK_POS))
        return fd->pos;     /* the backend set it, full width */

    /* update current position */
    if (result >= 0)
        fd->pos = result;

    return result;
}

/**
 * this function will get file information.
 *
 * @param path the file path.
 * @param buf the data buffer to save stat description.
 *
 * @return 0 on successful, -1 on failed.
 */
int dfs_file_stat(const char *path, struct stat *buf)
{
    int result;
    char *fullpath;
    struct dfs_filesystem *fs;

    /* Filesystems fill only some fields: the rest went to user space as
     * kernel stack contents. */
    rt_memset(buf, 0, sizeof(*buf));
    buf->st_nlink = 1;

    fullpath = dfs_normalize_path(NULL, path);
    if (fullpath == NULL)
    {
        return -ENOMEM;
    }

    {
        struct dfs_fnode *fnode;
        int gone;

        /* Unlinked while open: the name is gone already. */
        dfs_fm_lock();
        fnode = dfs_fnode_find(fullpath, NULL);
        gone = fnode && (fnode->flags & DFS_FNODE_F_UNLINKED);
        dfs_fm_unlock();
        if (gone)
        {
            rt_free(fullpath);
            return -ENOENT;
        }
    }

    if ((fs = dfs_filesystem_lookup(fullpath)) == NULL)
    {
        LOG_E("can't find mounted filesystem on this path:%s", fullpath);
        rt_free(fullpath);

        return -ENOENT;
    }

    if ((fullpath[0] == '/' && fullpath[1] == '\0') ||
        (dfs_subdir(fs->path, fullpath) == NULL))
    {
        /* it's the root directory */
        buf->st_dev   = 0;

        buf->st_mode  = S_IRUSR | S_IRGRP | S_IROTH |
                        S_IWUSR | S_IWGRP | S_IWOTH;
        buf->st_mode |= S_IFDIR | S_IXUSR | S_IXGRP | S_IXOTH;

        buf->st_size    = 0;
        buf->st_mtime   = 0;

        /* release full path */
        rt_free(fullpath);

        return RT_EOK;
    }
    else
    {
        if (fs->ops->stat == NULL)
        {
            rt_free(fullpath);
            LOG_E("the filesystem didn't implement this function");

            return -ENOSYS;
        }

        /* get the real file path and get file stat */
        if (fs->ops->flags & DFS_FS_FLAG_FULLPATH)
            result = fs->ops->stat(fs, fullpath, buf);
        else
            result = fs->ops->stat(fs, dfs_subdir(fs->path, fullpath), buf);
    }

    rt_free(fullpath);

    return result;
}

/**
 * this function will rename an old path name to a new path name.
 *
 * @param oldpath the old path name.
 * @param newpath the new path name.
 *
 * @return 0 on successful, -1 on failed.
 */
int dfs_file_rename(const char *oldpath, const char *newpath)
{
    int result = RT_EOK;
    struct dfs_filesystem *oldfs = NULL, *newfs = NULL;
    char *oldfullpath = NULL, *newfullpath = NULL;

    newfullpath = NULL;
    oldfullpath = NULL;

    oldfullpath = dfs_normalize_path(NULL, oldpath);
    if (oldfullpath == NULL)
    {
        result = -ENOENT;
        goto __exit;
    }

    /* A name unlinked while open is gone (ENOENT); one being renamed onto
     * cannot be replaced until its last close deletes it (EBUSY). */
    if (dfs_path_unlinked(oldfullpath))
    {
        result = -ENOENT;
        goto __exit;
    }
    if (dfs_file_is_open((const char *)oldfullpath))
    {
        result = -EBUSY;
        goto __exit;
    }

    newfullpath = dfs_normalize_path(NULL, newpath);
    if (newfullpath == NULL)
    {
        result = -ENOENT;
        goto __exit;
    }
    if (dfs_path_unlinked(newfullpath))
    {
        result = -EBUSY;
        goto __exit;
    }

    oldfs = dfs_filesystem_lookup(oldfullpath);
    newfs = dfs_filesystem_lookup(newfullpath);

    if ((oldfs == NULL) || (newfs == NULL))
    {
        result = -ENOENT;
        goto __exit;
    }

    if (oldfs == newfs)
    {
        if (oldfs->ops->rename == NULL)
        {
            result = -ENOSYS;
        }
        else
        {
            int attempt;

            for (attempt = 0; attempt < 2; attempt++)
            {
                if (oldfs->ops->flags & DFS_FS_FLAG_FULLPATH)
                    result = oldfs->ops->rename(oldfs, oldfullpath, newfullpath);
                else
                    /* use sub directory to rename in file system */
                    result = oldfs->ops->rename(oldfs,
                                                dfs_subdir(oldfs->path, oldfullpath),
                                                dfs_subdir(newfs->path, newfullpath));
                if (result != -EEXIST || attempt)
                    break;
                /* POSIX rename() replaces an existing file; FAT and tmpfs
                 * refuse, which broke "write tmp, rename over" saves. Not
                 * atomic: the target is unlinked first (refused while it is
                 * open), so a crash in between leaves only the source. */
                {
                    struct stat ost, nst;

                    if (rt_strcmp(oldfullpath, newfullpath) == 0 ||
                        dfs_file_stat(oldfullpath, &ost) < 0 ||
                        dfs_file_stat(newfullpath, &nst) < 0 ||
                        S_ISDIR(ost.st_mode) || S_ISDIR(nst.st_mode))
                        break;
                    result = dfs_file_do_unlink(newfullpath, 0);
                    if (result < 0)
                        break;
                }
            }
        }
    }
    else
    {
        result = -EXDEV;
    }

__exit:
#if defined (CHERRY_USB_DEVICE_ENABLE_CLASS_MTP)
    if (result == 0) {
        inotify_handler_filechange(NTY_FILE_RM, oldfullpath);
        inotify_handler_filechange(NTY_FILE_ADD, newfullpath);
    }
#endif // CHERRY_USB_DEVICE_ENABLE_CLASS_MTP

    if (oldfullpath)
    {
        rt_free(oldfullpath);
    }
    if (newfullpath)
    {
        rt_free(newfullpath);
    }

    /* not at same file system, return EXDEV */
    return result;
}

/**
 * this function is will cause the regular file referenced by fd
 * to be truncated to a size of precisely length bytes.
 *
 * @param fd the file descriptor.
 * @param length the length to be truncated.
 *
 * @return the status of truncated.
 */
int dfs_file_ftruncate(struct dfs_fd *fd, off_t length)
{
    int result;

    /* fd is null or not a regular file system fd, or length is invalid */
    if (fd == NULL || fd->fnode->type != FT_REGULAR || length < 0)
        return -EINVAL;

    if ((fd->flags & O_ACCMODE) == O_RDONLY)
        return -EBADF;

    if (fd->fnode->fops->ioctl == NULL)
        return -ENOSYS;

    result = fd->fnode->fops->ioctl(fd, RT_FIOFTRUNCATE, (void*)&length);

    /* update current size */
    if (result == 0)
    {
        fd->fnode->size = length;
#if defined (CHERRY_USB_DEVICE_ENABLE_CLASS_MTP)
        inotify_handler_filechange(NTY_FILE_CHG, fd->fnode->fullpath);
#endif // CHERRY_USB_DEVICE_ENABLE_CLASS_MTP
    }

    return result;
}

int dfs_file_mmap2(struct dfs_fd *fd, struct dfs_mmap2_args *mmap2)
{
    int ret;
    if (!fd || !mmap2) return -EINVAL;
    if ((fd->fnode->type != FT_DEVICE && fd->fnode->type != FT_REGULAR) ||
        !fd->fnode->fops->ioctl) return -ENODEV;
    /* tmpfs regular files provide mmap through this backend operation. */
    ret = fd->fnode->fops->ioctl(fd, RT_FIOMMAP2, mmap2);
    if (ret == -ENOSYS || ret == -EOPNOTSUPP) return -ENODEV;
    return ret > 0 ? -ret : ret;
}

#ifdef RT_USING_FINSH
#include <finsh.h>

void ls(const char *pathname)
{
    struct dfs_fd fd;
    struct dirent dirent;
    struct stat stat;
    int length;
    char *fullpath, *path;

    fullpath = NULL;
    if (pathname == NULL)
    {
#ifdef DFS_USING_WORKDIR
        /* open current working directory */
        path = rt_strdup(working_directory);
#else
        path = rt_strdup("/");
#endif
        if (path == NULL)
            return ; /* out of memory */
    }
    else
    {
        path = (char *)pathname;
    }

    fd_init(&fd);
    /* list directory */
    if (dfs_file_open(&fd, path, O_DIRECTORY) == 0)
    {
        rt_kprintf("Directory %s:\n", path);
        do
        {
            memset(&dirent, 0, sizeof(struct dirent));
            length = dfs_file_getdents(&fd, &dirent, sizeof(struct dirent));
            if (length > 0)
            {
                memset(&stat, 0, sizeof(struct stat));

                /* build full path for each file */
                fullpath = dfs_normalize_path(path, dirent.d_name);
                if (fullpath == NULL)
                    break;

                if (dfs_file_stat(fullpath, &stat) == 0)
                {
                    rt_kprintf("%-40s", dirent.d_name);
                    if (S_ISDIR(stat.st_mode))
                    {
                        rt_kprintf("%-25s\n", "<DIR>");
                    }
                    else
                    {
                        rt_kprintf("%-25lu\n", (unsigned long)stat.st_size);
                    }
                }
                else
                    rt_kprintf("BAD file: %s\n", dirent.d_name);
                rt_free(fullpath);
            }
        } while (length > 0);

        dfs_file_close(&fd);
    }
    else
    {
        rt_kprintf("No such directory\n");
    }
    if (pathname == NULL)
        rt_free(path);
}
FINSH_FUNCTION_EXPORT(ls, list directory contents);

void rm(const char *filename)
{
    if (dfs_file_unlink(filename) < 0)
    {
        rt_kprintf("Delete %s failed\n", filename);
    }
}
FINSH_FUNCTION_EXPORT(rm, remove files or directories);

void cat(const char *filename)
{
    struct dfs_fd fd;
    int length = 0;
    char buffer[81];

    fd_init(&fd);
    if (dfs_file_open(&fd, filename, O_RDONLY) < 0)
    {
        rt_kprintf("Open %s failed\n", filename);

        return;
    }

    do
    {
        rt_memset(buffer, 0x0, sizeof(buffer));
        length = dfs_file_read(&fd, (void *)buffer, sizeof(buffer) - 1);
        if (length > 0)
        {
            buffer[length] = '\0';
            write(STDOUT_FILENO, (void *)buffer, sizeof(buffer));
        }
    } while (length > 0);

    dfs_file_close(&fd);
}
FINSH_FUNCTION_EXPORT(cat, print file);

#define BUF_SZ  4096
static void copyfile(const char *src, const char *dst)
{
    struct dfs_fd fd;
    struct dfs_fd src_fd;
    rt_uint8_t *block_ptr;
    rt_int32_t read_bytes;

    block_ptr = (rt_uint8_t *)rt_malloc(BUF_SZ);
    if (block_ptr == NULL)
    {
        rt_kprintf("out of memory\n");

        return;
    }

    fd_init(&src_fd);
    if (dfs_file_open(&src_fd, src, O_RDONLY) < 0)
    {
        rt_free(block_ptr);
        rt_kprintf("Read %s failed\n", src);

        return;
    }
    fd_init(&fd);
    if (dfs_file_open(&fd, dst, O_WRONLY | O_CREAT) < 0)
    {
        rt_free(block_ptr);
        dfs_file_close(&src_fd);

        rt_kprintf("Write %s failed\n", dst);

        return;
    }

    do
    {
        read_bytes = dfs_file_read(&src_fd, block_ptr, BUF_SZ);
        if (read_bytes > 0)
        {
            int length;

            length = dfs_file_write(&fd, block_ptr, read_bytes);
            if (length != read_bytes)
            {
                /* write failed. */
                rt_kprintf("Write file data failed, errno=%d\n", length);
                break;
            }
        }
    } while (read_bytes > 0);

    dfs_file_close(&src_fd);
    dfs_file_close(&fd);
    rt_free(block_ptr);
}

extern int mkdir(const char *path, mode_t mode);
static void copydir(const char *src, const char *dst)
{
    struct dirent dirent;
    struct stat stat;
    int length;
    struct dfs_fd cpfd;

    /* dfs_file_close() acts only on a descriptor with one reference. */
    fd_init(&cpfd);
    if (dfs_file_open(&cpfd, src, O_DIRECTORY) < 0)
    {
        rt_kprintf("open %s failed\n", src);
        return ;
    }

    do
    {
        memset(&dirent, 0, sizeof(struct dirent));

        length = dfs_file_getdents(&cpfd, &dirent, sizeof(struct dirent));
        if (length > 0)
        {
            char *src_entry_full = NULL;
            char *dst_entry_full = NULL;

            if (strcmp(dirent.d_name, "..") == 0 || strcmp(dirent.d_name, ".") == 0)
                continue;

            /* build full path for each file */
            if ((src_entry_full = dfs_normalize_path(src, dirent.d_name)) == NULL)
            {
                rt_kprintf("out of memory!\n");
                break;
            }
            if ((dst_entry_full = dfs_normalize_path(dst, dirent.d_name)) == NULL)
            {
                rt_kprintf("out of memory!\n");
                rt_free(src_entry_full);
                break;
            }

            memset(&stat, 0, sizeof(struct stat));
            if (dfs_file_stat(src_entry_full, &stat) != 0)
            {
                rt_kprintf("open file: %s failed\n", dirent.d_name);
                continue;
            }

            if (S_ISDIR(stat.st_mode))
            {
                mkdir(dst_entry_full, 0);
                copydir(src_entry_full, dst_entry_full);
            }
            else
            {
                copyfile(src_entry_full, dst_entry_full);
            }
            rt_free(src_entry_full);
            rt_free(dst_entry_full);
        }
    } while (length > 0);

    dfs_file_close(&cpfd);
}

static const char *_get_path_lastname(const char *path)
{
    char *ptr;
    if ((ptr = (char *)strrchr(path, '/')) == NULL)
        return path;

    /* skip the '/' then return */
    return ++ptr;
}
void copy(const char *src, const char *dst)
{
#define FLAG_SRC_TYPE      0x03
#define FLAG_SRC_IS_DIR    0x01
#define FLAG_SRC_IS_FILE   0x02
#define FLAG_SRC_NON_EXSIT 0x00

#define FLAG_DST_TYPE      0x0C
#define FLAG_DST_IS_DIR    0x04
#define FLAG_DST_IS_FILE   0x08
#define FLAG_DST_NON_EXSIT 0x00

    struct stat stat;
    uint32_t flag = 0;

    /* check the staus of src and dst */
    if (dfs_file_stat(src, &stat) < 0)
    {
        rt_kprintf("copy failed, bad %s\n", src);
        return;
    }
    if (S_ISDIR(stat.st_mode))
        flag |= FLAG_SRC_IS_DIR;
    else
        flag |= FLAG_SRC_IS_FILE;

    if (dfs_file_stat(dst, &stat) < 0)
    {
        flag |= FLAG_DST_NON_EXSIT;
    }
    else
    {
        if (S_ISDIR(stat.st_mode))
            flag |= FLAG_DST_IS_DIR;
        else
            flag |= FLAG_DST_IS_FILE;
    }

    //2. check status
    if ((flag & FLAG_SRC_IS_DIR) && (flag & FLAG_DST_IS_FILE))
    {
        rt_kprintf("cp faild, cp dir to file is not permitted!\n");
        return ;
    }

    //3. do copy
    if (flag & FLAG_SRC_IS_FILE)
    {
        if (flag & FLAG_DST_IS_DIR)
        {
            char *fdst;
            fdst = dfs_normalize_path(dst, _get_path_lastname(src));
            if (fdst == NULL)
            {
                rt_kprintf("out of memory\n");
                return;
            }
            copyfile(src, fdst);
            rt_free(fdst);
        }
        else
        {
            copyfile(src, dst);
        }
    }
    else //flag & FLAG_SRC_IS_DIR
    {
        if (flag & FLAG_DST_IS_DIR)
        {
            char *fdst;
            fdst = dfs_normalize_path(dst, _get_path_lastname(src));
            if (fdst == NULL)
            {
                rt_kprintf("out of memory\n");
                return;
            }
            mkdir(fdst, 0);
            copydir(src, fdst);
            rt_free(fdst);
        }
        else if ((flag & FLAG_DST_TYPE) == FLAG_DST_NON_EXSIT)
        {
            mkdir(dst, 0);
            copydir(src, dst);
        }
        else
        {
            copydir(src, dst);
        }
    }
}
FINSH_FUNCTION_EXPORT(copy, copy file or dir)

#endif
/* @} */

/*
 * POSIX advisory record locks: fcntl(F_GETLK, F_SETLK, F_SETLKW).
 *
 * A lock belongs to a process (the calling lwp; a kernel thread outside one
 * owns its own) and covers [start, end] of one fnode. Conflicts are between
 * different owners when either lock is a write lock. A process loses its
 * locks on a file when it closes any descriptor of that file (POSIX), on
 * dup2() replacement and on exit. Waiters for F_SETLKW sleep interruptibly
 * and are woken whenever a lock goes; a wait that would close a two-process
 * cycle fails with EDEADLK. FAT used to answer F_SETLK with success and lock
 * nothing.
 */
#define DFS_OFF_MAX ((off_t)0x7fffffffffffffffLL)

struct dfs_flock
{
    struct dfs_flock *next;
    void *owner;
    pid_t pid;
    short type;                 /* F_RDLCK or F_WRLCK */
    off_t start, end;           /* inclusive; end DFS_OFF_MAX: to EOF and on */
};

struct dfs_flock_waiter
{
    rt_list_t node;
    struct rt_semaphore sem;
    void *owner;
    void *blocker;
};

static rt_list_t dfs_flock_waiters = { &dfs_flock_waiters, &dfs_flock_waiters };

void *dfs_file_lock_owner(void)
{
#ifdef RT_USING_LWP
    void *lwp = lwp_self();

    if (lwp)
        return lwp;
#endif
    return rt_thread_self();
}

static pid_t dfs_flock_pid(void *owner)
{
#ifdef RT_USING_LWP
    if (owner && owner == (void *)lwp_self())
        return lwp_to_pid((struct rt_lwp *)owner);
#endif
    return 0;
}

/* Caller holds the fnode lock. */
static void dfs_flock_wake_locked(void)
{
    while (!rt_list_isempty(&dfs_flock_waiters))
    {
        struct dfs_flock_waiter *w =
            rt_list_entry(dfs_flock_waiters.next, struct dfs_flock_waiter, node);

        rt_list_remove(&w->node);
        rt_sem_release(&w->sem);
    }
}

static void dfs_flock_free_all_locked(struct dfs_fnode *fnode)
{
    struct dfs_flock *l = fnode->locks;

    if (!l)
        return;
    fnode->locks = RT_NULL;
    while (l)
    {
        struct dfs_flock *next = l->next;

        rt_free(l);
        l = next;
    }
    dfs_flock_wake_locked();
}

static int dfs_flock_range(struct dfs_fd *fd, const struct flock *fl, off_t *start, off_t *end)
{
    off_t base, s, e;

    switch (fl->l_whence)
    {
    case SEEK_SET:
        base = 0;
        break;
    case SEEK_CUR:
        base = fd->pos;
        break;
    case SEEK_END:
        base = (off_t)fd->fnode->size;
        break;
    default:
        return -EINVAL;
    }
    s = base + fl->l_start;
    if (fl->l_len > 0)
        e = s + fl->l_len - 1;
    else if (fl->l_len == 0)
        e = DFS_OFF_MAX;
    else
    {
        e = s - 1;
        s = s + fl->l_len;
    }
    if (s < 0 || e < s)
        return -EINVAL;
    *start = s;
    *end = e;
    return 0;
}

/* Caller holds the fnode lock. A lock of another owner that conflicts. */
static struct dfs_flock *dfs_flock_conflict_locked(struct dfs_fnode *fnode, void *owner,
                                                   short type, off_t s, off_t e)
{
    struct dfs_flock *l;

    for (l = fnode->locks; l; l = l->next)
    {
        if (l->owner != owner && l->start <= e && s <= l->end &&
            (l->type == F_WRLCK || type == F_WRLCK))
            return l;
    }
    return RT_NULL;
}

/* Caller holds the fnode lock. Drop @owner's locks on [s, e]; @spare is a
 * preallocated node for splitting one lock in two (consumed if used). */
static void dfs_flock_clear_locked(struct dfs_fnode *fnode, void *owner, off_t s, off_t e,
                                   struct dfs_flock **spare)
{
    struct dfs_flock **link = &fnode->locks;

    while (*link)
    {
        struct dfs_flock *l = *link;

        if (l->owner != owner || l->end < s || e < l->start)
        {
            link = &l->next;
            continue;
        }
        if (l->start < s && l->end > e)
        {
            struct dfs_flock *tail = *spare;

            *spare = RT_NULL;
            *tail = *l;
            tail->start = e + 1;
            l->end = s - 1;
            tail->next = l->next;
            l->next = tail;
            link = &tail->next;
        }
        else if (l->start < s)
        {
            l->end = s - 1;
            link = &l->next;
        }
        else if (l->end > e)
        {
            l->start = e + 1;
            link = &l->next;
        }
        else
        {
            *link = l->next;
            rt_free(l);
        }
    }
}

/* Caller holds the fnode lock. Whether waiting on @blocker would close a
 * cycle: @blocker itself waits for a lock @owner holds. */
static int dfs_flock_deadlock_locked(void *owner, void *blocker)
{
    rt_list_t *n;

    for (n = dfs_flock_waiters.next; n != &dfs_flock_waiters; n = n->next)
    {
        struct dfs_flock_waiter *w = rt_list_entry(n, struct dfs_flock_waiter, node);

        if (w->owner == blocker && w->blocker == owner)
            return 1;
    }
    return 0;
}

/* fcntl(F_GETLK, F_SETLK, F_SETLKW) on @fd; @fl is a kernel copy. */
int dfs_file_lock(struct dfs_fd *fd, int cmd, struct flock *fl)
{
    void *owner = dfs_file_lock_owner();
    struct dfs_fnode *fnode;
    struct dfs_flock *fresh = RT_NULL, *spare = RT_NULL;
    off_t s, e;
    int ret;

    if (fd == RT_NULL || fl == RT_NULL)
        return -EINVAL;
    fnode = fd->fnode;
    /* Files with a path only: sockets and channels are not lockable. */
    if (fnode == RT_NULL || fnode->fullpath == RT_NULL)
        return -EINVAL;
    ret = dfs_flock_range(fd, fl, &s, &e);
    if (ret < 0)
        return ret;

    if (cmd == F_GETLK)
    {
        struct dfs_flock *l;

        if (fl->l_type != F_RDLCK && fl->l_type != F_WRLCK)
            return -EINVAL;
        dfs_fm_lock();
        l = dfs_flock_conflict_locked(fnode, owner, fl->l_type, s, e);
        if (l)
        {
            fl->l_type = l->type;
            fl->l_whence = SEEK_SET;
            fl->l_start = l->start;
            fl->l_len = l->end == DFS_OFF_MAX ? 0 : l->end - l->start + 1;
            fl->l_pid = l->pid;
        }
        else
            fl->l_type = F_UNLCK;
        dfs_fm_unlock();
        return 0;
    }
    if (cmd != F_SETLK && cmd != F_SETLKW)
        return -EINVAL;

    switch (fl->l_type)
    {
    case F_UNLCK:
        break;
    case F_RDLCK:
        if ((fd->flags & O_ACCMODE) == O_WRONLY)
            return -EBADF;
        break;
    case F_WRLCK:
        if ((fd->flags & O_ACCMODE) == O_RDONLY)
            return -EBADF;
        break;
    default:
        return -EINVAL;
    }

    /* Allocate before taking the lock: a split needs one node, a new lock
     * another. */
    spare = rt_calloc(1, sizeof(*spare));
    if (fl->l_type != F_UNLCK)
        fresh = rt_calloc(1, sizeof(*fresh));
    if (!spare || (fl->l_type != F_UNLCK && !fresh))
    {
        rt_free(spare);
        rt_free(fresh);
        return -ENOLCK;
    }

    for (;;)
    {
        struct dfs_flock *blocking;
        struct dfs_flock_waiter w;
        rt_err_t taken;

        dfs_fm_lock();
        blocking = fl->l_type == F_UNLCK ? RT_NULL :
                   dfs_flock_conflict_locked(fnode, owner, fl->l_type, s, e);
        if (!blocking)
        {
            dfs_flock_clear_locked(fnode, owner, s, e, &spare);
            if (fresh)
            {
                fresh->owner = owner;
                fresh->pid = dfs_flock_pid(owner);
                fresh->type = fl->l_type;
                fresh->start = s;
                fresh->end = e;
                fresh->next = fnode->locks;
                fnode->locks = fresh;
                fresh = RT_NULL;
            }
            /* A conversion or an unlock may let a waiter in. */
            dfs_flock_wake_locked();
            dfs_fm_unlock();
            rt_free(spare);
            return 0;
        }
        if (cmd == F_SETLK)
        {
            dfs_fm_unlock();
            ret = -EAGAIN;
            break;
        }
        if (dfs_flock_deadlock_locked(owner, blocking->owner))
        {
            dfs_fm_unlock();
            ret = -EDEADLK;
            break;
        }
        w.owner = owner;
        w.blocker = blocking->owner;
        rt_sem_init(&w.sem, "dfs_flk", 0, RT_IPC_FLAG_PRIO);
        rt_list_insert_before(&dfs_flock_waiters, &w.node);
        dfs_fm_unlock();
        taken = rt_sem_take_interruptible(&w.sem, RT_WAITING_FOREVER);
        dfs_fm_lock();
        /* A waker unlinks w (under this lock) before releasing it. */
        if (!rt_list_isempty(&w.node))
            rt_list_remove(&w.node);
        dfs_fm_unlock();
        rt_sem_detach(&w.sem);
        if (taken != RT_EOK)
        {
            ret = -EINTR;
            break;
        }
    }
    rt_free(spare);
    rt_free(fresh);
    return ret;
}

/* Drop every lock @owner holds on @fnode (a close of any of its descriptors
 * of the file, dup2() replacing one, exit). */
void dfs_file_lock_release(struct dfs_fnode *fnode, void *owner)
{
    struct dfs_flock **link;
    int dropped = 0;

    if (fnode == RT_NULL)
        return;
    dfs_fm_lock();
    link = &fnode->locks;
    while (*link)
    {
        struct dfs_flock *l = *link;

        if (l->owner == owner)
        {
            *link = l->next;
            rt_free(l);
            dropped = 1;
        }
        else
            link = &l->next;
    }
    if (dropped)
        dfs_flock_wake_locked();
    dfs_fm_unlock();
}

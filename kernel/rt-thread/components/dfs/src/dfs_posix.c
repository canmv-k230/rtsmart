/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2009-05-27     Yi.qiu       The first version
 * 2018-02-07     Bernard      Change the 3rd parameter of open/fcntl/ioctl to '...'
 * 2021-08-26     linzhenxing  add setcwd and modify getcwd\chdir
 */

#include <dfs.h>
#include <dfs_posix.h>
#include "dfs_private.h"

#ifdef RT_USING_LWP
#include <lwp.h>
#endif

/**
 * @addtogroup FsPosixApi
 */

/*@{*/

/**
 * this function is a POSIX compliant version, which will open a file and
 * return a file descriptor according specified flags.
 *
 * @param file the path name of file.
 * @param flags the file open flags.
 *
 * @return the non-negative integer on successful open, others for failed.
 */
int open(const char *file, int flags, ...)
{
    int fd, result;
    struct dfs_fd *d;

    /* allocate a fd */
    fd = fd_new();
    if (fd < 0)
    {
        rt_set_errno(-ENOMEM);

        return -1;
    }
    d = fd_get(fd);

    result = dfs_file_open(d, file, flags);
    if (result < 0)
    {
        /* release the ref-count of fd */
        fd_release(fd);

        rt_set_errno(result);

        return -1;
    }

    return fd;
}
RTM_EXPORT(open);

/**
 * this function is a POSIX compliant version, which will close the open
 * file descriptor.
 *
 * @param fd the file descriptor.
 *
 * @return 0 on successful, -1 on failed.
 */
int close(int fd)
{
    int result;

    result = fdt_fd_close(dfs_fdtable_get(), fd, -1);

    if (result < 0)
    {
        rt_set_errno(result);

        return -1;
    }

    return 0;
}
RTM_EXPORT(close);

/**
 * this function is a POSIX compliant version, which will read specified data
 * buffer length for an open file descriptor.
 *
 * @param fd the file descriptor.
 * @param buf the buffer to save the read data.
 * @param len the maximal length of data buffer
 *
 * @return the actual read data buffer length. If the returned value is 0, it
 * may be reach the end of file, please check errno.
 */
#if defined(RT_USING_NEWLIB) && defined(_EXFUN)
_READ_WRITE_RETURN_TYPE _EXFUN(read, (int fd, void *buf, size_t len))
#else
int read(int fd, void *buf, size_t len)
#endif
{
    int result;
    struct dfs_fd *d;

    /* get the fd */
    d = fd_get_ref(fd);
    if (d == NULL)
    {
        rt_set_errno(-EBADF);

        return -1;
    }

    result = dfs_file_read(d, buf, len);
    fd_put(d);
    if (result < 0)
    {
        rt_set_errno(result);

        return -1;
    }

    return result;
}
RTM_EXPORT(read);

/**
 * this function is a POSIX compliant version, which will write specified data
 * buffer length for an open file descriptor.
 *
 * @param fd the file descriptor
 * @param buf the data buffer to be written.
 * @param len the data buffer length.
 *
 * @return the actual written data buffer length.
 */
#if defined(RT_USING_NEWLIB) && defined(_EXFUN)
_READ_WRITE_RETURN_TYPE _EXFUN(write, (int fd, const void *buf, size_t len))
#else
int write(int fd, const void *buf, size_t len)
#endif
{
    int result;
    struct dfs_fd *d;

    /* get the fd */
    d = fd_get_ref(fd);
    if (d == NULL)
    {
        rt_set_errno(-EBADF);

        return -1;
    }

    result = dfs_file_write(d, buf, len);
    fd_put(d);
    if (result < 0)
    {
        rt_set_errno(result);

        return -1;
    }

    return result;
}
RTM_EXPORT(write);

/**
 * this function is a POSIX compliant version, which will seek the offset for
 * an open file descriptor.
 *
 * @param fd the file descriptor.
 * @param offset the offset to be seeked.
 * @param whence the directory of seek.
 *
 * @return the current read/write position in the file, or -1 on failed.
 */
off_t lseek(int fd, off_t offset, int whence)
{
    off_t result;
    struct dfs_fd *d;

    d = fd_get_ref(fd);
    if (d == NULL)
    {
        rt_set_errno(-EBADF);

        return -1;
    }

    switch (whence)
    {
    case SEEK_SET:
        break;

    case SEEK_CUR:
        offset += d->pos;
        break;

    case SEEK_END:
        offset += d->fnode->size;
        break;

    default:
        fd_put(d);
        rt_set_errno(-EINVAL);

        return -1;
    }

    if (offset < 0)
    {
        fd_put(d);
        rt_set_errno(-EINVAL);

        return -1;
    }
    result = dfs_file_lseek(d, offset);
    fd_put(d);
    if (result < 0)
    {
        rt_set_errno(result);

        return -1;
    }

    /* Where the backend put it (it may clamp). */
    return result;
}
RTM_EXPORT(lseek);

/**
 * this function is a POSIX compliant version, which will rename old file name
 * to new file name.
 *
 * @param old the old file name.
 * @param new the new file name.
 *
 * @return 0 on successful, -1 on failed.
 *
 * note: the old and new file name must be belong to a same file system.
 */
int rename(const char *old_file, const char *new_file)
{
    int result;

    result = dfs_file_rename(old_file, new_file);
    if (result < 0)
    {
        rt_set_errno(result);

        return -1;
    }

    return 0;
}
RTM_EXPORT(rename);

/**
 * this function is a POSIX compliant version, which will unlink (remove) a
 * specified path file from file system.
 *
 * @param pathname the specified path name to be unlinked.
 *
 * @return 0 on successful, -1 on failed.
 */
int unlink(const char *pathname)
{
    int result;

    result = dfs_file_unlink(pathname);
    if (result < 0)
    {
        rt_set_errno(result);

        return -1;
    }

    return 0;
}
RTM_EXPORT(unlink);

#ifndef _WIN32 /* we can not implement these functions */
/**
 * this function is a POSIX compliant version, which will get file information.
 *
 * @param file the file name
 * @param buf the data buffer to save stat description.
 *
 * @return 0 on successful, -1 on failed.
 */
int stat(const char *file, struct stat *buf)
{
    int result;

    result = dfs_file_stat(file, buf);
    if (result < 0)
    {
        rt_set_errno(result);

        return -1;
    }

    return result;
}
RTM_EXPORT(stat);

/**
 * this function is a POSIX compliant version, which will get file status.
 *
 * @param fildes the file description
 * @param buf the data buffer to save stat description.
 *
 * @return 0 on successful, -1 on failed.
 */
int fstat(int fildes, struct stat *buf)
{
    struct dfs_fd *d;

    /* get the fd */
    d = fd_get_ref(fildes);
    if (d == NULL)
    {
        rt_set_errno(-EBADF);

        return -1;
    }

    /* Unset fields used to be left as they were (kernel stack, for
     * sys_fstat()); st_dev/st_ino stay 0 as stat() reports them. */
    rt_memset(buf, 0, sizeof(*buf));
    /* 0 once unlinked while open (dfs_file_unlink()). */
    buf->st_nlink = (d->fnode->flags & DFS_FNODE_F_UNLINKED) ? 0 : 1;
    buf->st_mode = S_IRUSR | S_IRGRP | S_IROTH | S_IWUSR | S_IWGRP | S_IWOTH;
    switch (d->fnode->type)
    {
    case FT_DIRECTORY:
        buf->st_mode |= S_IFDIR | S_IXUSR | S_IXGRP | S_IXOTH;
        break;
    case FT_SOCKET:
        buf->st_mode |= S_IFSOCK;
        break;
    case FT_DEVICE:
        /* As devfs stat() of the path. Other filesystems (procfs) mark
         * files FT_DEVICE without an rt_device behind them. */
        if (d->fnode->fs && d->fnode->fs->ops &&
            rt_strcmp(d->fnode->fs->ops->name, "devfs") == 0 && d->fnode->data)
        {
            rt_device_t dev = (rt_device_t)d->fnode->data;

            if (dev->type == RT_Device_Class_Block)
                buf->st_mode |= S_IFBLK;
            else if (dev->type == RT_Device_Class_Pipe)
                buf->st_mode |= S_IFIFO;
            else
                buf->st_mode |= S_IFCHR;
        }
        else
            buf->st_mode |= S_IFREG;
        break;
    default:
        buf->st_mode |= S_IFREG;
        break;
    }

    buf->st_size    = d->fnode->size;
    fd_put(d);

    return RT_EOK;
}
RTM_EXPORT(fstat);
#endif

/**
 * this function is a POSIX compliant version, which shall request that all data
 * for the open file descriptor named by fildes is to be transferred to the storage
 * device associated with the file described by fildes.
 *
 * @param fildes the file description
 *
 * @return 0 on successful completion. Otherwise, -1 shall be returned and errno
 * set to indicate the error.
 */
int fsync(int fildes)
{
    int ret;
    struct dfs_fd *d;

    /* get the fd */
    d = fd_get_ref(fildes);
    if (d == NULL)
    {
        rt_set_errno(-EBADF);
        return -1;
    }

    ret = dfs_file_flush(d);
    fd_put(d);
    /* As every other call here: -1 and errno (sys_fsync() read a stale
     * errno for the backend's error). Nothing to flush is not an error. */
    if (ret == -ENOSYS)
        ret = 0;
    if (ret < 0)
    {
        rt_set_errno(ret);
        return -1;
    }

    return 0;
}
RTM_EXPORT(fsync);

/**
 * this function is a POSIX compliant version, which shall perform a variety of
 * control functions on devices.
 *
 * @param fildes the file description
 * @param cmd the specified command
 * @param data represents the additional information that is needed by this
 * specific device to perform the requested function.
 *
 * @return 0 on successful completion. Otherwise, -1 shall be returned and errno
 * set to indicate the error.
 */
/* A driver's file: devfs devices, and procfs entries with their own ops.
 * Their ioctl commands share numbers with untagged fcntl commands. tmpfs
 * marks its regular files FT_DEVICE too (for mmap); those are not. */
/* A record-lock command on @d, the descriptor @fildes named. After taking a
 * lock, check @fildes still names @d: a close() from another thread while
 * this waited (F_SETLKW) would otherwise leave a lock that no descriptor
 * releases. As Linux, drop the process's locks on the file and fail. */
int dfs_fd_lock_cmd(int fildes, struct dfs_fd *d, int cmd, struct flock *fl)
{
    int ret = dfs_file_lock(d, cmd, fl);

    if (ret == 0 && cmd != F_GETLK && fl->l_type != F_UNLCK && fd_get(fildes) != d)
    {
        dfs_file_lock_release(d->fnode, dfs_file_lock_owner());
        ret = -EBADF;
    }
    return ret;
}

int dfs_fd_is_driver_file(struct dfs_fd *d)
{
    return d && d->fnode && d->fnode->type == FT_DEVICE && d->fnode->fs && d->fnode->fs->ops &&
           (rt_strcmp(d->fnode->fs->ops->name, "devfs") == 0 ||
            rt_strcmp(d->fnode->fs->ops->name, "procfs") == 0);
}

int fcntl(int fildes, int cmd, ...)
{
    int ret = -1;
    struct dfs_fd *d;
    void *arg;
    va_list ap;

    va_start(ap, cmd);
    arg = va_arg(ap, void *);
    va_end(ap);

    /* Descriptor-slot commands. musl's fcntl() and ioctl() share one system
     * call: a tagged command (DFS_FCNTL_TAG) is always fcntl. An untagged
     * 0, 1, 2 or 1030 is fcntl too, except on a device file, where it is the
     * driver's ioctl command, as it always was. */
    int handled = 1;
    int tagged = ((unsigned int)cmd & DFS_FCNTL_TAG_MASK) == DFS_FCNTL_TAG;

    if (tagged)
        cmd &= ~DFS_FCNTL_TAG_MASK;
    else if (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC || cmd == F_GETFD || cmd == F_SETFD ||
             cmd == F_GETLK || cmd == F_SETLK || cmd == F_SETLKW)
    {
        d = fd_get_ref(fildes);
        if (d && dfs_fd_is_driver_file(d))
        {
            ret = dfs_file_ioctl(d, cmd, arg);
            fd_put(d);
            if (ret < 0)
            {
                rt_set_errno(ret);
                ret = -1;
            }
            return ret;
        }
        fd_put(d);
    }

    switch (cmd)
    {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC:
        ret = fd_dup_from(fildes, (int)(rt_base_t)arg, cmd == F_DUPFD_CLOEXEC);
        break;
    case F_GETFD:
        ret = fd_get_cloexec(fildes);
        if (ret > 0)
            ret = FD_CLOEXEC;
        break;
    case F_SETFD:
        ret = fd_set_cloexec(fildes, ((int)(rt_base_t)arg & FD_CLOEXEC) != 0);
        break;
    case F_GETLK:
    case F_SETLK:
    case F_SETLKW:
        /* Record locks are DFS's own (dfs_file_lock()); the backends' answer
         * was a fake success (FAT) or ENOSYS. @arg was checked for user
         * access by sys_ioctl(). */
        d = fd_get_ref(fildes);
        if (!d)
            ret = -EBADF;
        else if (!arg)
            ret = -EFAULT;
        else
        {
            struct flock kfl = *(struct flock *)arg;

            ret = dfs_fd_lock_cmd(fildes, d, cmd, &kfl);
            if (ret == 0 && cmd == F_GETLK)
                *(struct flock *)arg = kfl;
        }
        fd_put(d);
        break;
    default:
        handled = 0;
        break;
    }
    if (handled)
    {
        if (ret < 0)
        {
            rt_set_errno(ret);
            ret = -1;
        }
        return ret;
    }

    /* get the fd */
    d = fd_get_ref(fildes);
    if (d)
    {
        ret = dfs_file_ioctl(d, cmd, arg);
        fd_put(d);
    }
    else ret = -EBADF;

    if (ret < 0)
    {
        rt_set_errno(ret);
        ret = -1;
    }

    return ret;
}
RTM_EXPORT(fcntl);

/**
 * this function is a POSIX compliant version, which shall perform a variety of
 * control functions on devices.
 *
 * @param fildes the file description
 * @param cmd the specified command
 * @param data represents the additional information that is needed by this
 * specific device to perform the requested function.
 *
 * @return 0 on successful completion. Otherwise, -1 shall be returned and errno
 * set to indicate the error.
 */
int ioctl(int fildes, int cmd, ...)
{
    void *arg;
    va_list ap;

    va_start(ap, cmd);
    arg = va_arg(ap, void *);
    va_end(ap);

    /* we use fcntl for this API. */
    return fcntl(fildes, cmd, arg);
}
RTM_EXPORT(ioctl);

/**
 *
 * this function is a POSIX compliant version, which cause the regular file
 * referenced by fd to be truncated to a size of precisely length bytes.
 * @param fd the file descriptor.
 * @param length the length to be truncated.
 *
 * @return Upon successful completion, ftruncate() shall return 0;
 * otherwise, -1 shall be returned and errno set to indicate the error.
 */
int ftruncate(int fd, off_t length)
{
    int result;
    struct dfs_fd *d;

    d = fd_get_ref(fd);
    if (d == NULL)
    {
        rt_set_errno(-EBADF);

        return -1;
    }

    if (length < 0)
    {
        fd_put(d);
        rt_set_errno(-EINVAL);

        return -1;
    }
    result = dfs_file_ftruncate(d, length);
    fd_put(d);
    if (result < 0)
    {
        rt_set_errno(result);

        return -1;
    }

    return 0;
}
RTM_EXPORT(ftruncate);

/**
 * this function is a POSIX compliant version, which will return the
 * information about a mounted file system.
 *
 * @param path the path which mounted file system.
 * @param buf the buffer to save the returned information.
 *
 * @return 0 on successful, others on failed.
 */
int statfs(const char *path, struct statfs *buf)
{
    int result;

    result = dfs_statfs(path, buf);
    if (result < 0)
    {
        rt_set_errno(result);

        return -1;
    }

    return result;
}
RTM_EXPORT(statfs);

/**
 * this function is a POSIX compliant version, which will make a directory
 *
 * @param path the directory path to be made.
 * @param mode
 *
 * @return 0 on successful, others on failed.
 */
int mkdir(const char *path, mode_t mode)
{
    int fd;
    struct dfs_fd *d;
    int result;

    fd = fd_new();
    if (fd == -1)
    {
        rt_set_errno(-ENOMEM);

        return -1;
    }

    d = fd_get(fd);

    result = dfs_file_open(d, path, O_DIRECTORY | O_CREAT);

    if (result < 0)
    {
        fd_release(fd);
        rt_set_errno(result);

        return -1;
    }

    dfs_file_close(d);
    fd_release(fd);

    return 0;
}
RTM_EXPORT(mkdir);

#ifdef RT_USING_FINSH
#include <finsh.h>
FINSH_FUNCTION_EXPORT(mkdir, create a directory);
#endif

/**
 * this function is a POSIX compliant version, which will remove a directory.
 *
 * @param pathname the path name to be removed.
 *
 * @return 0 on successful, others on failed.
 */
int rmdir(const char *pathname)
{
    int result;
    struct stat st;

    /* dfs_file_unlink() removes files too: rmdir() must not. */
    result = dfs_file_stat(pathname, &st);
    if (result == 0 && !S_ISDIR(st.st_mode))
        result = -ENOTDIR;
    if (result == 0)
        result = dfs_file_unlink(pathname);
    if (result < 0)
    {
        rt_set_errno(result);

        return -1;
    }

    return 0;
}
RTM_EXPORT(rmdir);

/**
 * this function is a POSIX compliant version, which will open a directory.
 *
 * @param name the path name to be open.
 *
 * @return the DIR pointer of directory, NULL on open directory failed.
 */
DIR *opendir(const char *name)
{
    struct dfs_fd *d;
    int fd, result;
    DIR *t;

    t = NULL;

    /* allocate a fd */
    fd = fd_new();
    if (fd == -1)
    {
        rt_set_errno(-ENOMEM);

        return NULL;
    }
    d = fd_get(fd);

    result = dfs_file_open(d, name, O_RDONLY | O_DIRECTORY);
    if (result >= 0)
    {
        /* open successfully */
        t = (DIR *) rt_malloc(sizeof(DIR));
        if (t == NULL)
        {
            dfs_file_close(d);
            fd_release(fd);
        }
        else
        {
            memset(t, 0, sizeof(DIR));

            t->fd = fd;
        }

        return t;
    }

    /* open failed */
    fd_release(fd);
    rt_set_errno(result);

    return NULL;
}
RTM_EXPORT(opendir);

/**
 * this function is a POSIX compliant version, which will return a pointer
 * to a dirent structure representing the next directory entry in the
 * directory stream.
 *
 * @param d the directory stream pointer.
 *
 * @return the next directory entry, NULL on the end of directory or failed.
 */
struct dirent *readdir(DIR *d)
{
    int result;
    struct dfs_fd *fd;

    fd = fd_get(d->fd);
    if (fd == NULL)
    {
        rt_set_errno(-EBADF);
        return NULL;
    }

    if (d->num)
    {
        struct dirent *dirent_ptr;
        dirent_ptr = (struct dirent *)&d->buf[d->cur];
        d->cur += dirent_ptr->d_reclen;
    }

    if (!d->num || d->cur >= d->num)
    {
        /* get a new entry */
        result = dfs_file_getdents(fd,
                                   (struct dirent *)d->buf,
                                   sizeof(d->buf) - 1);
        if (result <= 0)
        {
            rt_set_errno(result);

            return NULL;
        }

        d->num = result;
        d->cur = 0; /* current entry index */
    }

    return (struct dirent *)(d->buf + d->cur);
}
RTM_EXPORT(readdir);

/**
 * this function is a POSIX compliant version, which will return current
 * location in directory stream.
 *
 * @param d the directory stream pointer.
 *
 * @return the current location in directory stream.
 */
long telldir(DIR *d)
{
    struct dfs_fd *fd;
    long result;

    fd = fd_get(d->fd);
    if (fd == NULL)
    {
        rt_set_errno(-EBADF);

        return 0;
    }

    result = fd->pos - d->num + d->cur;

    return result;
}
RTM_EXPORT(telldir);

/**
 * this function is a POSIX compliant version, which will set position of
 * next directory structure in the directory stream.
 *
 * @param d the directory stream.
 * @param offset the offset in directory stream.
 */
void seekdir(DIR *d, off_t offset)
{
    struct dfs_fd *fd;

    fd = fd_get(d->fd);
    if (fd == NULL)
    {
        rt_set_errno(-EBADF);

        return ;
    }

    /* seek to the offset position of directory */
    if (dfs_file_lseek(fd, offset) >= 0)
        d->num = d->cur = 0;
}
RTM_EXPORT(seekdir);

/**
 * this function is a POSIX compliant version, which will reset directory
 * stream.
 *
 * @param d the directory stream.
 */
void rewinddir(DIR *d)
{
    struct dfs_fd *fd;

    fd = fd_get(d->fd);
    if (fd == NULL)
    {
        rt_set_errno(-EBADF);

        return ;
    }

    /* seek to the beginning of directory */
    if (dfs_file_lseek(fd, 0) >= 0)
        d->num = d->cur = 0;
}
RTM_EXPORT(rewinddir);

/**
 * this function is a POSIX compliant version, which will close a directory
 * stream.
 *
 * @param d the directory stream.
 *
 * @return 0 on successful, -1 on failed.
 */
int closedir(DIR *d)
{
    int result;
    struct dfs_fd *fd;

    fd = fd_get(d->fd);
    if (fd == NULL)
    {
        rt_set_errno(-EBADF);

        return -1;
    }

    result = dfs_file_close(fd);
    fd_release(d->fd);

    rt_free(d);

    if (result < 0)
    {
        rt_set_errno(result);

        return -1;
    }
    else
        return 0;
}
RTM_EXPORT(closedir);

#ifdef DFS_USING_WORKDIR
/**
 * this function is a POSIX compliant version, which will change working
 * directory.
 *
 * @param path the path name to be changed to.
 *
 * @return 0 on successful, -1 on failed.
 */
int chdir(const char *path)
{
    char *fullpath;
    DIR *d;

    if (path == NULL)
    {
        dfs_lock();
#ifdef DFS_USING_WORKDIR
        rt_kprintf("%s\n", working_directory);
#endif
        dfs_unlock();

        return 0;
    }

    if (strlen(path) > DFS_PATH_MAX)
    {
        rt_set_errno(-ENOTDIR);

        return -1;
    }

    fullpath = dfs_normalize_path(NULL, path);
    if (fullpath == NULL)
    {
        rt_set_errno(-ENOTDIR);

        return -1; /* build path failed */
    }

    dfs_lock();
    d = opendir(fullpath);
    if (d == NULL)
    {
        rt_free(fullpath);
        /* this is a not exist directory */
        dfs_unlock();

        return -1;
    }

    /* close directory stream */
    closedir(d);
#ifdef RT_USING_LWP
    /* copy full path to working directory */
    lwp_setcwd(fullpath);
#else
    rt_strncpy(working_directory, fullpath, DFS_PATH_MAX);
#endif
    /* release normalize directory path name */
    rt_free(fullpath);

    dfs_unlock();

    return 0;
}
RTM_EXPORT(chdir);

#ifdef RT_USING_FINSH
FINSH_FUNCTION_EXPORT_ALIAS(chdir, cd, change current working directory);
#endif
#endif

/**
 * this function is a POSIX compliant version, which shall check the file named
 * by the pathname pointed to by the path argument for accessibility according
 * to the bit pattern contained in amode.
 *
 * @param path the specified file/dir path.
 * @param amode the value is either the bitwise-inclusive OR of the access
 * permissions to be checked (R_OK, W_OK, X_OK) or the existence test (F_OK).
 */
int access(const char *path, int amode)
{
    struct stat sb;

    if (amode & ~(R_OK | W_OK | X_OK))
    {
        rt_set_errno(-EINVAL);
        return -1;
    }
    if (stat(path, &sb) < 0)
        return -1; /* already sets errno */

    /* W_OK from the mode stat() reports (FAT's read-only attribute clears
     * the write bits). Everything is readable. X_OK is not checked: FAT and
     * tmpfs files carry no execute bits, yet any file can be executed. */
    if ((amode & W_OK) && !(sb.st_mode & (S_IWUSR | S_IWGRP | S_IWOTH)))
    {
        rt_set_errno(-EACCES);
        return -1;
    }
    return 0;
}
/**
 * this function is a POSIX compliant version, which will set current
 * working directory.
 *
 * @param buf the current directory.
 *
 * @return null.
 */
void setcwd(char *buf)
{
#ifdef DFS_USING_WORKDIR
    dfs_lock();
#ifdef RT_USING_LWP
    lwp_setcwd(buf);
#else
    rt_strncpy(working_directory, buf, DFS_PATH_MAX);
#endif
    dfs_unlock();
#else
    rt_kprintf(NO_WORKING_DIR);
#endif

    return ;
}
RTM_EXPORT(setcwd);

/**
 * this function is a POSIX compliant version, which will return current
 * working directory.
 *
 * @param buf the returned current directory.
 * @param size the buffer size.
 *
 * @return the returned current directory.
 */
char *getcwd(char *buf, size_t size)
{
#ifdef DFS_USING_WORKDIR
    char *dir_buf = RT_NULL;

    dfs_lock();

#ifdef RT_USING_LWP
    dir_buf = lwp_getcwd();
#else
    dir_buf = &working_directory[0];
#endif

    /* copy to buf parameter: whole and terminated, or ERANGE (strncpy left
     * a too-long path unterminated) */
    if (buf)
    {
        size_t len = rt_strlen(dir_buf);

        if (len + 1 > size)
        {
            dfs_unlock();
            rt_set_errno(-ERANGE);
            return RT_NULL;
        }
        rt_memcpy(buf, dir_buf, len + 1);
    }

    dfs_unlock();
#else
    rt_kprintf(NO_WORKING_DIR);
#endif

    return buf;
}
RTM_EXPORT(getcwd);

/* @} */

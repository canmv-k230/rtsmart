/*
 * Copyright (c) 2006-2018, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2005-01-26     Bernard      The first version.
 */

#ifndef __DFS_FILE_H__
#define __DFS_FILE_H__

#include <dfs.h>
#include <dfs_fs.h>

#ifdef __cplusplus
extern "C" {
#endif

struct rt_pollreq;

struct dfs_file_ops
{
    int (*open)     (struct dfs_fd *fd);
    int (*close)    (struct dfs_fd *fd);
    int (*ioctl)    (struct dfs_fd *fd, int cmd, void *args);
    int (*read)     (struct dfs_fd *fd, void *buf, size_t count);
    int (*write)    (struct dfs_fd *fd, const void *buf, size_t count);
    int (*flush)    (struct dfs_fd *fd);
    int (*lseek)    (struct dfs_fd *fd, off_t offset);
    int (*getdents) (struct dfs_fd *fd, struct dirent *dirp, uint32_t count);

    int (*poll)     (struct dfs_fd *fd, struct rt_pollreq *req);
};

/* file descriptor */
#define DFS_FD_MAGIC     0xfdfd

struct dfs_fnode
{
    uint16_t type;               /* Type (regular or socket) */

    char *path;                  /* Name (below mount point) */
    char *fullpath;              /* Full path is hash key */
    int ref_count;               /* Descriptor reference count */
    rt_list_t list;              /* The node of fnode hash table */

    struct dfs_filesystem *fs;
    const struct dfs_file_ops *fops;
    uint32_t flags;              /* self flags, is dir etc.. */

    size_t   size;               /* Size in bytes */
    void *data;                  /* Specific file system data */

    /* Backend opens and closes run without the fnode lock, serialized per
     * path by these fields, which the fnode lock guards (dfs_file.c). busy is
     * DFS_FNODE_OPENING while the first open of the path runs and
     * DFS_FNODE_CLOSING while a close runs, in busy_owner; opens and closes
     * wait for it. opening counts later opens in flight, at most one thread's
     * (nested opens of the path by that thread); other opens and closes
     * wait for them, as a backend tells its last close from ref_count, and
     * close_waiting counts those closes, which new later opens wait behind.
     * All zero for fnodes that are not hashed (sockets, channels). */
    uint8_t busy;
    uint16_t opening;
    uint16_t close_waiting;
    struct rt_thread *busy_owner;
    /* POSIX record locks (fcntl F_SETLK...), guarded by the fnode lock.
     * Last: every fnode is allocated zeroed, by DFS, sockets and channels. */
    struct dfs_flock *locks;
};

/* fnode->flags: unlink()ed while open. The name is gone for opens and
 * stat(); the backend unlink runs at the last close (FAT cannot delete an
 * open file). Guarded by the fnode lock. */
#define DFS_FNODE_F_UNLINKED 0x1

#define DFS_FNODE_OPENING 1
#define DFS_FNODE_CLOSING 2

struct dfs_fd
{
    uint16_t magic;              /* file descriptor magic number */
    uint32_t flags;              /* Descriptor flags */
    int ref_count;               /* Descriptor reference count */
    off_t    pos;                /* Current file position */
    struct dfs_fnode *fnode;     /* file node struct */
    void *data;                  /* Specific fd data */
    struct dfs_fd *close_next;   /* Last-reference closes awaiting retry */
};

struct dfs_mmap2_args
{
    void *addr;
    size_t length;
    int prot;
    int flags;
    off_t pgoffset;

    void *ret;
};

struct dfs_preallocate_args
{
    off_t offset;
    off_t len;
};

/*
 * dfs_file_ops.open returns 0 or a negative POSIX errno, which reaches user
 * space unchanged; -ENXIO is a failure even though -RT_ENOSYS has the same
 * number. musl reads a syscall result below -4095 as a success, so other
 * negative codes, such as MPP's K_ERR_*, are reported as -EIO.
 */
#define DFS_ERRNO_MAX 4095

rt_inline int dfs_open_errno(int result)
{
    return result < -DFS_ERRNO_MAX ? -EIO : result;
}

void dfs_fnode_mgr_init(void);
int dfs_file_is_open(const char *pathname);
int dfs_file_open(struct dfs_fd *fd, const char *path, int flags);
int dfs_file_close(struct dfs_fd *fd);
int dfs_file_close_nowait(struct dfs_fd *fd);
struct flock;
int dfs_file_lock(struct dfs_fd *fd, int cmd, struct flock *fl);
void dfs_file_lock_release(struct dfs_fnode *fnode, void *owner);
void *dfs_file_lock_owner(void);
int dfs_fd_is_driver_file(struct dfs_fd *d);
int dfs_fd_lock_cmd(int fildes, struct dfs_fd *d, int cmd, struct flock *fl);
int dfs_file_ioctl(struct dfs_fd *fd, int cmd, void *args);
int dfs_file_read(struct dfs_fd *fd, void *buf, size_t len);
int dfs_file_getdents(struct dfs_fd *fd, struct dirent *dirp, size_t nbytes);
int dfs_file_unlink(const char *path);
int dfs_file_write(struct dfs_fd *fd, const void *buf, size_t len);
int dfs_file_flush(struct dfs_fd *fd);
off_t dfs_file_lseek(struct dfs_fd *fd, off_t offset);

int dfs_file_stat(const char *path, struct stat *buf);
int dfs_file_rename(const char *oldpath, const char *newpath);
int dfs_file_ftruncate(struct dfs_fd *fd, off_t length);
int dfs_file_mmap2(struct dfs_fd *fd, struct dfs_mmap2_args *mmap2);

/* 0x5254 is just a magic number to make these relatively unique ("RT") */
#define RT_FIOFTRUNCATE  0x52540000U
#define RT_FIOGETADDR    0x52540001U
#define RT_FIOMMAP2      0x52540002U
#define RT_FIOPREALLOCATE 0x52540003U

#ifdef __cplusplus
}
#endif

#endif

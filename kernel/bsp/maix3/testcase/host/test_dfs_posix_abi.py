#!/usr/bin/env python3
"""Check what the DFS POSIX layer gives musl user space, on tmpfs and devfs.

Compiles the production dfs/src/dfs.c, dfs_file.c, dfs_posix.c,
filesystems/devfs/devfs.c and filesystems/tmpfs/dfs_tmpfs.c unmodified
against the RT-Thread stubs of test_dfs_close.py, with tmpfs mounted at /tmp.
The kernel's POSIX entry points are renamed (k_open, k_stat, ...) so they do
not interpose on the host C library.

- open(): O_CREAT|O_EXCL on an existing file is EEXIST; O_DIRECTORY on a
  file is ENOTDIR; a directory opens read-only without O_DIRECTORY and is
  EISDIR for writing; mkdir() of an existing path is EEXIST.
- O_APPEND: every write goes to the end, not only the first.
- fstat(): fields not filled are 0 (not stack contents); devices, pipes and
  sockets have their own S_IF* type; stat() and fstat() agree on st_dev and
  st_ino.
- getdents(): each tmpfs entry's own d_type; devfs devices as DT_CHR.
- lseek(): returns the backend's position; ESPIPE without a seek hook.
- fcntl(): untagged commands 0, 1, 2 on a device reach its driver (they are
  its ioctl commands); tagged ones (DFS_FCNTL_TAG, k230_syscalls' fcntl())
  and untagged ones on other files are fcntl: F_GETFD/F_SETFD per descriptor
  slot (dup() does not copy it), F_DUPFD(_CLOEXEC) at or above the minimum,
  F_GETFL on a directory without DFS's own bits.
- A close-on-exec close that fails at exec is retried later, not dropped.
- dup()/dup2() errors are negative errnos, not -1 (EPERM in user space).
- fdt_close_on_exec() closes exactly the close-on-exec slots.
- rename() replaces an existing file, refuses an open target, and does not
  replace a directory; rmdir() of a file is ENOTDIR.
- getcwd() of a too-small buffer is ERANGE, never an unterminated string.
- unlink() of an open file succeeds; the name is gone at once and the file
  at the last close.
- Directory positions: lseek() to an entry's end position resumes at the next
  entry (seekdir()), per open file, on tmpfs and devfs; getdents of a file is
  ENOTDIR.
- access() checks its mode argument; O_TMPFILE is EOPNOTSUPP.
- POSIX record locks: conflicts (EAGAIN), F_GETLK, F_SETLKW waiting and
  EDEADLK, range splitting, release on any close, EBADF/EINVAL.
Run with python3 -B kernel/bsp/maix3/testcase/host/test_dfs_posix_abi.py.
"""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

import test_dfs_close as base

RENAMED = ("open close read write lseek rename unlink stat fstat fsync fcntl ioctl "
           "ftruncate rmdir chdir getcwd statfs access pipe mkfifo mkdir opendir "
           "readdir telldir seekdir rewinddir closedir").split()

EXTRA_STUBS = r"""
#include <stdarg.h>
#define RT_DEFINE_SPINLOCK(x) int x = 0
#define rt_hw_spin_lock(l) rt_hw_interrupt_disable()
#define rt_hw_spin_unlock(l) rt_hw_interrupt_enable(0)
#ifndef rt_list_for_each
#define rt_list_for_each(pos, head) for (pos = (head)->next; pos != (head); pos = pos->next)
#endif
#ifndef rt_list_for_each_safe
#define rt_list_for_each_safe(pos, n, head) \
    for (pos = (head)->next, n = pos->next; pos != (head); pos = n, n = pos->next)
#endif
"""

ENV_EXTRA = r"""
/* tmpfs at /tmp, devfs at /dev. dfs_filesystem_lookup() is the harness's. */
static struct dfs_filesystem tmpfs_fs;
const struct dfs_filesystem_ops *test_tmpfs_ops;
int dfs_tmpfs_init(void);
int dfs_statfs(const char *path, struct statfs *buffer) { return -ENOSYS; }
struct dfs_filesystem *test_tmpfs(void) { return &tmpfs_fs; }
extern char working_directory[];
void test_set_cwd(const char *p) { strcpy(working_directory, p); }
void test_mount_tmpfs(void)
{
    assert(dfs_tmpfs_init() == 0 && test_tmpfs_ops);
    tmpfs_fs.ops = test_tmpfs_ops;
    tmpfs_fs.path = "/tmp";
    assert(tmpfs_fs.ops->mount(&tmpfs_fs, 0, NULL) == 0);
}
"""

CASES = r"""
#include <dfs_posix.h>
#include <errno.h>
#include <fcntl.h>

void test_mount_tmpfs(void);
void test_set_cwd(const char *p);

static int failures;
#define CHECK(x) do { if (!(x)) { failures++; fprintf(stderr, "%s:%d: CHECK(%s) failed, errno=%d\n", __FILE__, __LINE__, #x, (int)rt_get_errno()); } } while (0)
/* The kernel reports -1 with a negative errno in rt_get_errno(). */
#define ERR(call, e) ((call) == -1 && rt_get_errno() == -(e))

/* Lets the test link against a tree without it (to show the old failures). */
__attribute__((weak)) void fdt_close_on_exec(struct dfs_fdtable *fdt) {}

static int ioctl_calls, last_ioctl_cmd, close_calls, close_fail;
static int dev_ioctl(struct dfs_fd *fd, int cmd, void *args) { ioctl_calls++; last_ioctl_cmd = cmd; return 0; }
static int dev_open(struct dfs_fd *fd) { return 0; }
static int dev_close(struct dfs_fd *fd) { close_calls++; return close_fail ? -EINTR : 0; }
#define TAG(cmd) (DFS_FCNTL_TAG | (cmd))
static const struct dfs_file_ops dev_fops = { .open = dev_open, .close = dev_close, .ioctl = dev_ioctl };
static struct rt_device chardev, pipedev;

static void test_open_flags(void)
{
    int fd = k_open("/tmp/a", O_CREAT | O_RDWR, 0);
    CHECK(fd >= 0);
    CHECK(ERR(k_open("/tmp/a", O_CREAT | O_EXCL | O_RDWR, 0), EEXIST));
    CHECK(k_close(fd) == 0);
    CHECK(ERR(k_open("/tmp/a", O_CREAT | O_EXCL | O_RDWR, 0), EEXIST));
    CHECK(ERR(k_open("/tmp/a", O_RDONLY | O_DIRECTORY, 0), ENOTDIR));
    CHECK(k_mkdir("/tmp/d", 0) == 0);
    CHECK(ERR(k_mkdir("/tmp/d", 0), EEXIST));
    fd = k_open("/tmp/d", O_RDONLY, 0);             /* no O_DIRECTORY */
    CHECK(fd >= 0);
    if (fd >= 0) CHECK(k_close(fd) == 0);
    CHECK(ERR(k_open("/tmp/d", O_RDWR, 0), EISDIR));
}

static void test_append(void)
{
    char buf[16] = {0};
    int a = k_open("/tmp/log", O_CREAT | O_RDWR | O_TRUNC, 0);
    int b = k_open("/tmp/log", O_WRONLY | O_APPEND, 0);
    CHECK(a >= 0 && b >= 0);
    CHECK(k_write(a, "12345", 5) == 5);
    CHECK(k_write(b, "ab", 2) == 2);                /* after "12345" */
    CHECK(k_write(a, "Z", 1) == 1);                 /* a's own position 5 */
    CHECK(k_write(b, "cd", 2) == 2);                /* the end again */
    CHECK(k_lseek(a, 0, SEEK_SET) == 0);
    CHECK(k_read(a, buf, sizeof(buf)) == 9);
    CHECK(memcmp(buf, "12345Zbcd", 9) == 0);
    k_close(a); k_close(b);
}

static void test_fstat_types(void)
{
    struct stat st, st2;
    int fd = k_open("/tmp/a", O_RDWR, 0);

    memset(&st, 0xa5, sizeof(st));
    CHECK(k_fstat(fd, &st) == 0);
    CHECK(S_ISREG(st.st_mode) && st.st_nlink == 1 && st.st_ino == 0 && st.st_uid == 0 &&
          st.st_blocks == 0 && st.st_atime == 0);
    memset(&st2, 0xa5, sizeof(st2));
    CHECK(k_stat("/tmp/a", &st2) == 0);
    CHECK(st.st_dev == st2.st_dev && st.st_ino == st2.st_ino && st2.st_nlink == 1);
    k_close(fd);

    fd = k_open("/dev/chr0", O_RDWR, 0);
    CHECK(fd >= 0);
    CHECK(k_fstat(fd, &st) == 0 && S_ISCHR(st.st_mode));
    k_close(fd);
    fd = k_open("/dev/fifo0", O_RDWR, 0);
    CHECK(fd >= 0);
    CHECK(k_fstat(fd, &st) == 0 && S_ISFIFO(st.st_mode));
    k_close(fd);
    fd = k_open("/tmp/d", O_RDONLY | O_DIRECTORY, 0);
    CHECK(k_fstat(fd, &st) == 0 && S_ISDIR(st.st_mode));
    k_close(fd);
}

static unsigned char dtype_of(const char *dir, const char *name)
{
    struct dirent ents[8];
    int fd = k_open(dir, O_RDONLY | O_DIRECTORY, 0);
    struct dfs_fd *d = fd_get(fd);
    int n = dfs_file_getdents(d, ents, sizeof(ents));
    unsigned char t = 0xff;

    for (int i = 0; i < n / (int)sizeof(struct dirent); i++)
        if (strcmp(ents[i].d_name, name) == 0)
            t = ents[i].d_type;
    k_close(fd);
    return t;
}

static void test_getdents_types(void)
{
    CHECK(dtype_of("/tmp", "a") == DT_REG);
    CHECK(dtype_of("/tmp", "d") == DT_DIR);
    CHECK(dtype_of("/dev", "chr0") == DT_CHR);
    CHECK(dtype_of("/dev", "fifo0") == DT_FIFO);
}

static void test_lseek(void)
{
    int fd = k_open("/tmp/a", O_RDWR, 0);
    CHECK(k_write(fd, "hello", 5) == 5);
    CHECK(k_lseek(fd, 2, SEEK_SET) == 2);
    CHECK(k_lseek(fd, 0, SEEK_END) == 5);
    CHECK(k_ftruncate(fd, 2) == 0);
    CHECK(k_lseek(fd, 0, SEEK_CUR) == 5); /* truncate keeps the position */
    char tail[8];
    CHECK(k_read(fd, tail, 1) == 0);
    CHECK(k_ftruncate(fd, 8) == 0);
    CHECK(k_lseek(fd, 2, SEEK_SET) == 2);
    memset(tail, 1, sizeof(tail));
    CHECK(k_read(fd, tail, 6) == 6);
    CHECK(!memcmp(tail, "\0\0\0\0\0\0", 6));
    CHECK(ERR(k_lseek(fd, -1, SEEK_SET), EINVAL));
    k_close(fd);
    fd = k_open("/dev/chr0", O_RDWR, 0);           /* driver fops without lseek */
    CHECK(ERR(k_lseek(fd, 0, SEEK_CUR), ESPIPE));
    k_close(fd);
}

static void test_fcntl(void)
{
    int fd = k_open("/dev/chr0", O_RDWR, 0), dup1, dup2fd, hi, f;

    /* Untagged 0, 1, 2 on a device are the driver's own ioctl commands (ADC
     * enable/disable, I2C slave buffer size/address): never swallowed. */
    ioctl_calls = 0;
    CHECK(k_ioctl(fd, 0, (void *)5) == 0 && last_ioctl_cmd == 0);
    CHECK(k_ioctl(fd, 1, (void *)5) == 0 && last_ioctl_cmd == 1);
    CHECK(k_ioctl(fd, 2, (void *)5) == 0 && last_ioctl_cmd == 2);
    CHECK(ioctl_calls == 3);

    /* fcntl, tagged (k230_syscalls' fcntl()), on the same device */
    ioctl_calls = 0;
    CHECK(k_fcntl(fd, TAG(F_GETFD)) == 0);
    CHECK(k_fcntl(fd, TAG(F_SETFD), FD_CLOEXEC) == 0);
    CHECK(k_fcntl(fd, TAG(F_GETFD)) == FD_CLOEXEC);
    dup1 = sys_dup(fd);
    CHECK(dup1 >= 0 && k_fcntl(dup1, TAG(F_GETFD)) == 0); /* per slot */
    hi = k_fcntl(fd, TAG(F_DUPFD_CLOEXEC), 10);
    CHECK(hi >= 10 && k_fcntl(hi, TAG(F_GETFD)) == FD_CLOEXEC);
    CHECK(k_fcntl(fd, TAG(F_DUPFD), 10) > hi);
    dup2fd = sys_dup2(fd, 9);
    CHECK(dup2fd == 9 && k_fcntl(9, TAG(F_GETFD)) == 0);
    CHECK(ioctl_calls == 0);                         /* tagged: never the driver */
    CHECK(ERR(k_fcntl(99, TAG(F_GETFD)), EBADF));
    CHECK(sys_dup(99) == -EBADF && sys_dup2(99, 5) == -EBADF);
    CHECK(sys_dup2(fd, -1) == -EBADF);

    /* exec closes the close-on-exec slots only */
    fdt_close_on_exec(dfs_fdtable_get());
    CHECK(fd_get(fd) == NULL && fd_get(hi) == NULL);
    CHECK(fd_get(dup1) != NULL && fd_get(9) != NULL);
    k_close(dup1); k_close(9);
    for (int i = 11; i < DFS_FD_MAX; i++) if (fd_get(i)) k_close(i);

    /* Untagged on a non-device file is fcntl: no driver could mean it. */
    f = k_open("/tmp/a", O_RDWR, 0);
    CHECK(k_fcntl(f, F_SETFD, FD_CLOEXEC) == 0 && k_fcntl(f, F_GETFD) == FD_CLOEXEC);
    CHECK(k_fcntl(f, F_DUPFD, 12) >= 12);
    k_close(f);
    for (int i = 12; i < DFS_FD_MAX; i++) if (fd_get(i)) k_close(i);

    fd = k_open("/tmp/d", O_RDONLY | O_DIRECTORY, 0);
    int fl = k_fcntl(fd, F_GETFL);
    CHECK(fl >= 0 && (fl & O_DIRECTORY) && !(fl & DFS_F_INTERNAL_MASK));
    k_close(fd);
}

/* A close-on-exec descriptor whose close fails at exec is queued and closed
 * on a later retry, not dropped (a socket's would leak). */
static void test_exec_close_retry(void)
{
    int fd = k_open("/dev/chr0", O_RDWR, 0), n;

    CHECK(k_fcntl(fd, TAG(F_SETFD), FD_CLOEXEC) == 0);
    close_calls = 0;
    close_fail = 1;
    fdt_close_on_exec(dfs_fdtable_get());
    CHECK(fd_get(fd) == NULL && close_calls >= 1);
    n = close_calls;
    close_fail = 0;
    int other = fd_new();                           /* retries queued closes */
    CHECK(close_calls == n + 1);
    fd_release(other);

    /* The same for a path-less descriptor (a socket's fnode is not hashed:
     * releasing it used to free it without a close). */
    int s = fd_new();
    struct dfs_fd *d = fd_get(s);
    struct dfs_fnode *fn = calloc(1, sizeof(*fn));
    rt_list_init(&fn->list);
    fn->type = FT_SOCKET; fn->ref_count = 1; fn->fops = &dev_fops;
    d->fnode = fn; d->flags = O_RDWR;
    CHECK(k_fcntl(s, TAG(F_SETFD), FD_CLOEXEC) == 0);
    close_calls = 0;
    close_fail = 1;
    fdt_close_on_exec(dfs_fdtable_get());
    CHECK(fd_get(s) == NULL && close_calls >= 1);
    n = close_calls;
    close_fail = 0;
    other = fd_new();
    CHECK(close_calls == n + 1);
    fd_release(other);
}

static void test_rename_rmdir(void)
{
    char buf[8] = {0};
    int fd = k_open("/tmp/new", O_CREAT | O_WRONLY | O_TRUNC, 0);
    CHECK(k_write(fd, "NEW", 3) == 3);
    k_close(fd);
    fd = k_open("/tmp/old", O_CREAT | O_WRONLY | O_TRUNC, 0);
    CHECK(k_write(fd, "OLD", 3) == 3);
    k_close(fd);
    CHECK(k_rename("/tmp/new", "/tmp/old") == 0);   /* replaces */
    fd = k_open("/tmp/old", O_RDONLY, 0);
    CHECK(k_read(fd, buf, sizeof(buf)) == 3 && memcmp(buf, "NEW", 3) == 0);
    /* an open target is not replaced */
    int src = k_open("/tmp/src", O_CREAT | O_WRONLY, 0);
    k_close(src);
    CHECK(ERR(k_rename("/tmp/src", "/tmp/old"), EBUSY));
    k_close(fd);
    CHECK(ERR(k_rename("/tmp/src", "/tmp/d"), EEXIST)); /* not over a directory */
    CHECK(ERR(k_rmdir("/tmp/src"), ENOTDIR));
    CHECK(k_unlink("/tmp/src") == 0);
}

static void test_getcwd(void)
{
    char small[4], big[32];

    test_set_cwd("/tmp/d");
    memset(small, 'x', sizeof(small));
    CHECK(k_getcwd(small, sizeof(small)) == NULL && rt_get_errno() == -ERANGE);
    CHECK(k_getcwd(big, sizeof(big)) == big && strcmp(big, "/tmp/d") == 0);
    CHECK(k_getcwd(big, 7) == big && strcmp(big, "/tmp/d") == 0);   /* exact fit */
    test_set_cwd("/tmp");
}

/* unlink() of an open file: the name goes now, the file at its last close. */
static void test_unlink_open(void)
{
    char buf[8] = {0};
    struct stat st;
    int fd = k_open("/tmp/u", O_CREAT | O_RDWR | O_TRUNC, 0);

    CHECK(fd >= 0 && k_write(fd, "keep", 4) == 4);
    CHECK(k_unlink("/tmp/u") == 0);                        /* was EBUSY */
    CHECK(ERR(k_unlink("/tmp/u"), ENOENT));
    CHECK(ERR(k_open("/tmp/u", O_RDONLY, 0), ENOENT));
    CHECK(ERR(k_stat("/tmp/u", &st), ENOENT));
    CHECK(k_lseek(fd, 0, SEEK_SET) == 0 && k_read(fd, buf, 8) == 4 && !memcmp(buf, "keep", 4));
    CHECK(k_close(fd) == 0);                               /* deleted here */
    CHECK(ERR(k_stat("/tmp/u", &st), ENOENT));
    fd = k_open("/tmp/u", O_CREAT | O_RDWR, 0);
    CHECK(fd >= 0 && k_fstat(fd, &st) == 0 && st.st_size == 0);
    k_close(fd);
    CHECK(k_unlink("/tmp/u") == 0);

    /* fstat of an unlinked open file: st_nlink 0. Renaming onto the name is
     * EBUSY, from it ENOENT. Creating it again revives the open file and
     * cancels the delete ("unlink; fopen(name, "w")"). */
    fd = k_open("/tmp/u", O_CREAT | O_RDWR | O_TRUNC, 0);
    CHECK(k_write(fd, "old", 3) == 3);
    k_close(k_open("/tmp/src", O_CREAT | O_WRONLY, 0));
    CHECK(k_unlink("/tmp/u") == 0);
    CHECK(k_fstat(fd, &st) == 0 && st.st_nlink == 0);
    CHECK(ERR(k_rename("/tmp/src", "/tmp/u"), EBUSY));
    CHECK(ERR(k_rename("/tmp/u", "/tmp/v"), ENOENT));
    int again = k_open("/tmp/u", O_CREAT | O_WRONLY | O_TRUNC, 0);
    CHECK(again >= 0 && k_write(again, "new", 3) == 3);
    CHECK(k_fstat(fd, &st) == 0 && st.st_nlink == 1);
    k_close(again);
    k_close(fd);
    CHECK(k_stat("/tmp/u", &st) == 0 && st.st_size == 3);  /* not deleted */
    k_unlink("/tmp/u");
    k_unlink("/tmp/src");

    /* A device's name never goes into hiding: EBUSY while it is open. */
    fd = k_open("/dev/chr0", O_RDWR, 0);
    CHECK(ERR(k_unlink("/dev/chr0"), EBUSY));
    k_close(fd);
}

/* Directory positions: lseek() to the position after an entry resumes at the
 * next one (musl's seekdir(), from getdents d_off), on tmpfs and devfs; a
 * second listing of /dev is independent. getdents of a file is ENOTDIR. */
static int list_names(int fd, char names[][32], off_t *after, int max)
{
    struct dirent e;
    struct dfs_fd *d = fd_get(fd);
    int n = 0;

    while (n < max && dfs_file_getdents(d, &e, sizeof(e)) == (int)sizeof(e))
    {
        strncpy(names[n], e.d_name, 31);
        names[n][31] = 0;
        after[n] = d->pos;
        n++;
    }
    return n;
}

static void dir_positions(const char *dir)
{
    char names[16][32], again[1][32];
    off_t after[16], unused[1];
    int fd = k_open(dir, O_RDONLY | O_DIRECTORY, 0), fd2, n;

    n = list_names(fd, names, after, 16);
    CHECK(n >= 2 && n < 16);                               /* the listing ends */
    for (int a = 0; a < n; a++)                            /* and advances */
        for (int b = a + 1; b < n; b++)
            CHECK(strcmp(names[a], names[b]) != 0);
    for (int k = 0; k + 1 < n; k++)
    {
        CHECK(k_lseek(fd, after[k], SEEK_SET) == after[k]);
        CHECK(list_names(fd, again, unused, 1) == 1 && strcmp(again[0], names[k + 1]) == 0);
    }
    CHECK(k_lseek(fd, 0, SEEK_SET) == 0);                  /* rewinddir() */
    CHECK(list_names(fd, again, unused, 1) == 1 && strcmp(again[0], names[0]) == 0);
    fd2 = k_open(dir, O_RDONLY | O_DIRECTORY, 0);          /* its own position */
    CHECK(list_names(fd2, again, unused, 1) == 1 && strcmp(again[0], names[0]) == 0);
    k_close(fd2);
    k_close(fd);
}

static void test_dir_positions(void)
{
    char p[32];

    for (int i = 0; i < 5; i++)
    {
        snprintf(p, sizeof(p), "/tmp/e%d", i);
        k_close(k_open(p, O_CREAT | O_WRONLY, 0));
    }
    dir_positions("/tmp");
    dir_positions("/dev");
    int f = k_open("/tmp/e0", O_RDONLY, 0);
    struct dirent e;
    CHECK(dfs_file_getdents(fd_get(f), &e, sizeof(e)) == -ENOTDIR);
    k_close(f);
    for (int i = 0; i < 5; i++)
    {
        snprintf(p, sizeof(p), "/tmp/e%d", i);
        k_unlink(p);
    }
}

static void test_access_tmpfile(void)
{
    CHECK(k_access("/tmp/a", R_OK | W_OK) == 0);
    CHECK(ERR(k_access("/tmp/a", 0x40), EINVAL));
    CHECK(ERR(k_access("/tmp/nothing", F_OK), ENOENT));
#ifdef O_TMPFILE
    CHECK(ERR(k_open("/tmp", O_TMPFILE | O_RDWR, 0), EOPNOTSUPP));
#endif
}

/* POSIX record locks between two owners (threads stand for processes). */
static int lock_fd;
static volatile int other_result, other_done;
static struct flock want;

static int lk(int fd, int cmd, short type, off_t start, off_t len)
{
    struct flock fl = { .l_type = type, .l_whence = SEEK_SET, .l_start = start, .l_len = len };
    return k_fcntl(fd, TAG(cmd), &fl);
}

static void other_setlkw(void) { other_result = k_fcntl(lock_fd, TAG(F_SETLKW), &want); other_done = 1; }
static void other_setlk(void) { other_result = k_fcntl(lock_fd, TAG(F_SETLK), &want); other_done = 1; }
static void other_unlock_all(void) { other_result = lk(lock_fd, F_SETLK, F_UNLCK, 0, 0); other_done = 1; }

static void run_other(void (*job)(void))
{
    other_done = 0;
    test_run_on_worker(job);
}

static void test_record_locks(void)
{
    struct flock q;
    int fd = k_open("/tmp/lk", O_CREAT | O_RDWR | O_TRUNC, 0), fd2;

    lock_fd = fd;
    /* The other owner write-locks [0, 99]. */
    want = (struct flock){ .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 100 };
    run_other(other_setlk);
    CHECK(test_worker_wait(2000) && other_result == 0);
    /* Conflicts, F_GETLK, and a free range. */
    CHECK(ERR(lk(fd, F_SETLK, F_RDLCK, 50, 1), EAGAIN));
    q = (struct flock){ .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 10, .l_len = 1 };
    CHECK(k_fcntl(fd, TAG(F_GETLK), &q) == 0 && q.l_type == F_WRLCK && q.l_start == 0 && q.l_len == 100);
    CHECK(lk(fd, F_SETLK, F_WRLCK, 100, 10) == 0);
    /* F_SETLKW from main waits until the other owner unlocks. */
    want = (struct flock){ .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 200, .l_len = 1 };
    run_other(other_setlk);                                 /* other also holds [200] */
    CHECK(test_worker_wait(2000) && other_result == 0);
    /* Deadlock: other waits for [100..109] (mine) while I wait for [200] (its). */
    want = (struct flock){ .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 105, .l_len = 1 };
    run_other(other_setlkw);
    CHECK(!test_worker_wait(200));                          /* it blocks */
    CHECK(ERR(lk(fd, F_SETLKW, F_WRLCK, 200, 1), EDEADLK));
    CHECK(lk(fd, F_SETLK, F_UNLCK, 100, 10) == 0);          /* let it in */
    CHECK(test_worker_wait(2000) && other_result == 0);
    /* Unlocking the middle of a range splits it. */
    run_other(other_unlock_all);
    CHECK(test_worker_wait(2000) && other_result == 0);
    CHECK(lk(fd, F_SETLK, F_WRLCK, 0, 100) == 0);
    CHECK(lk(fd, F_SETLK, F_UNLCK, 10, 10) == 0);
    want = (struct flock){ .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 12, .l_len = 2 };
    run_other(other_setlk);
    CHECK(test_worker_wait(2000) && other_result == 0);      /* hole is free */
    want = (struct flock){ .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 5, .l_len = 1 };
    run_other(other_setlk);
    CHECK(test_worker_wait(2000) && other_result == -1);     /* [0..9] still mine */
    /* Closing any descriptor of the file drops my locks on it. */
    fd2 = k_open("/tmp/lk", O_RDONLY, 0);
    CHECK(k_close(fd2) == 0);
    run_other(other_setlk);
    CHECK(test_worker_wait(2000) && other_result == 0);
    /* A lock taken while its descriptor is being closed by another thread
     * is dropped (EBADF), not left with no descriptor to release it. */
    run_other(other_unlock_all);
    CHECK(test_worker_wait(2000));
    CHECK(lk(fd, F_SETLK, F_WRLCK, 300, 1) == 0);           /* mine */
    lock_fd = k_open("/tmp/lk", O_RDWR, 0);
    want = (struct flock){ .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 300, .l_len = 1 };
    run_other(other_setlkw);
    CHECK(!test_worker_wait(200));                          /* waits for mine */
    CHECK(k_close(lock_fd) == 0);                           /* its fd goes */
    CHECK(lk(fd, F_SETLK, F_UNLCK, 300, 1) == 0);           /* let it in */
    CHECK(test_worker_wait(2000) && other_result == -1 && rt_get_errno() != 0);
    q = (struct flock){ .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 300, .l_len = 1 };
    CHECK(k_fcntl(fd, TAG(F_GETLK), &q) == 0 && q.l_type == F_UNLCK);
    lock_fd = fd;

    /* Untagged 5/6/7 on a device are its driver's ioctl commands. */
    int dev = k_open("/dev/chr0", O_RDWR, 0);
    ioctl_calls = 0;
    CHECK(k_ioctl(dev, F_GETLK, (void *)1) == 0 && k_ioctl(dev, F_SETLK, (void *)1) == 0 &&
          k_ioctl(dev, F_SETLKW, (void *)1) == 0 && ioctl_calls == 3);
    k_close(dev);

    /* Wrong mode, bad type, sockets-without-path are refused. */
    run_other(other_unlock_all);
    CHECK(test_worker_wait(2000));
    fd2 = k_open("/tmp/lk", O_RDONLY, 0);
    CHECK(ERR(lk(fd2, F_SETLK, F_WRLCK, 0, 1), EBADF));
    CHECK(ERR(lk(fd2, F_SETLK, 99, 0, 1), EINVAL));
    k_close(fd2);
    k_close(fd);
    k_unlink("/tmp/lk");
}

int main(int argc, char **argv)
{
    test_sched_init();
    dfs_fnode_mgr_init();
    devfs_init();
    test_mount_tmpfs();
    chardev.type = RT_Device_Class_Char; chardev.fops = &dev_fops;
    pipedev.type = RT_Device_Class_Pipe; pipedev.fops = &dev_fops;
    test_register_device(&chardev, "chr0");
    test_register_device(&pipedev, "fifo0");
#define RUN(name) do { unsigned before = failures; test_##name(); \
        printf("%s %s\n", failures == before ? "PASS" : "FAIL", #name); fflush(stdout); } while (0)
    RUN(open_flags);
    RUN(append);
    RUN(fstat_types);
    RUN(getdents_types);
    RUN(lseek);
    RUN(fcntl);
    RUN(exec_close_retry);
    RUN(rename_rmdir);
    RUN(unlink_open);
    RUN(dir_positions);
    RUN(access_tmpfile);
    RUN(record_locks);
    RUN(getcwd);
    if (failures)
    {
        fprintf(stderr, "%u check(s) failed\n", failures);
        _exit(1);
    }
    _exit(0);
}
"""


def build(directory, dfs_root=base.DFS):
    build_dir = Path(directory)
    (build_dir / "rtconfig.h").write_text(base.RTCONFIG + "#define DFS_USING_WORKDIR\n")
    (build_dir / "stubs.h").write_text(base.STUBS + EXTRA_STUBS)
    for name in ("rtthread.h", "rthw.h", "rtdbg.h", "rtdevice.h", "dfs_poll.h", "lwp.h", "finsh.h", "lwp_user_mm.h"):
        (build_dir / name).write_text('#include "stubs.h"\n')
    env = base.ENV.replace(
        'struct dfs_filesystem *dfs_filesystem_lookup(const char *path)\n{\n'
        '    return strncmp(path, "/dev", 4) == 0 ? &devfs : RT_NULL;\n}',
        'struct dfs_filesystem *test_tmpfs(void);\n'
        'struct dfs_filesystem *dfs_filesystem_lookup(const char *path)\n{\n'
        '    if (strncmp(path, "/tmp", 4) == 0) return test_tmpfs();\n'
        '    return strncmp(path, "/dev", 4) == 0 ? &devfs : RT_NULL;\n}')
    assert env != base.ENV, "dfs_filesystem_lookup stub changed"
    reg = ('int dfs_register(const struct dfs_filesystem_ops *ops)\n{\n'
           '    devfs.path = "/dev";\n    devfs.ops = ops;\n    return 0;\n}')
    assert reg in env, "dfs_register stub changed"
    env = env.replace(reg, 'extern const struct dfs_filesystem_ops *test_tmpfs_ops;\n'
                      'int dfs_register(const struct dfs_filesystem_ops *ops)\n{\n'
                      '    if (strcmp(ops->name, "devfs") != 0) { test_tmpfs_ops = ops; return 0; }\n'
                      '    devfs.path = "/dev";\n    devfs.ops = ops;\n    return 0;\n}')
    (build_dir / "env.c").write_text(env + ENV_EXTRA)
    (build_dir / "test.c").write_text('#include "stubs.h"\n' + CASES)
    sources = [dfs_root / "src/dfs.c", dfs_root / "src/dfs_file.c", dfs_root / "src/dfs_posix.c",
               dfs_root / "filesystems/devfs/devfs.c", dfs_root / "filesystems/tmpfs/dfs_tmpfs.c",
               build_dir / "env.c", build_dir / "test.c"]
    cc = [os.environ.get("CC", "cc"), "-std=gnu11", "-D_GNU_SOURCE", "-Wall", "-g",
          "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-but-set-variable",
          "-fsanitize=undefined,address", "-fno-sanitize-recover=all", "-pthread",
          *[f"-D{n}=k_{n}" for n in RENAMED],
          "-I", str(build_dir), "-I", str(dfs_root / "include"),
          "-I", str(dfs_root / "filesystems/devfs"), "-I", str(dfs_root / "filesystems/tmpfs")]
    binary = build_dir / "test"
    subprocess.run([*cc, *map(str, sources), "-o", str(binary)], check=True)
    return binary


def main():
    dfs_root = Path(sys.argv[1]) if len(sys.argv) > 1 else base.DFS
    with tempfile.TemporaryDirectory(prefix="k230-dfs-posix-test-") as directory:
        binary = build(directory, dfs_root)
        result = subprocess.run([str(binary)], timeout=60)
        sys.exit(result.returncode)


if __name__ == "__main__":
    main()

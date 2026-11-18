/* Board-side check of the DFS system calls as musl user space sees them,
 * with the kernel fixes and k230_syscalls overrides of 2026-11-17.
 * Build: make -f .../dfs_musl_board.mk OUT=<dir>; run /sdcard/dfs-musl-board.elf
 * (optionally with a directory to test instead of /sdcard). Prints DFS_PASS
 * lines and "DFS_MUSL failures=0" at the end. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
#define CHECK(x) do { if (!(x)) { failures++; printf("DFS_FAIL %s:%d %s errno=%d\n", __func__, __LINE__, #x, errno); } } while (0)

static char dir[128];
static char path_a[160], path_b[160], path_sub[160];

static int write_file(const char *p, const char *s)
{
    int fd = open(p, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) return -1;
    ssize_t n = write(fd, s, strlen(s));
    close(fd);
    return n == (ssize_t)strlen(s) ? 0 : -1;
}

/* pread/pwrite (k230_syscalls), lseek errors (EINVAL, ESPIPE), O_APPEND. */
static void positional(void)
{
    char buf[16] = {0};
    int fd = open(path_a, O_CREAT | O_RDWR | O_TRUNC, 0644), p[2];
    CHECK(fd >= 0);
    CHECK(write(fd, "0123456789", 10) == 10);
    CHECK(pread(fd, buf, 4, 3) == 4 && !memcmp(buf, "3456", 4));
    CHECK(lseek(fd, 0, SEEK_CUR) == 10);          /* position kept */
    CHECK(pwrite(fd, "AB", 2, 0) == 2);
    CHECK(pread(fd, buf, 3, 0) == 3 && !memcmp(buf, "AB2", 3));
    errno = 0; CHECK(lseek(fd, -1, SEEK_SET) == -1 && errno == EINVAL);
    close(fd);
    CHECK(pipe(p) == 0);
    errno = 0; CHECK(lseek(p[0], 0, SEEK_CUR) == -1 && errno == ESPIPE);
    errno = 0; CHECK(pread(p[0], buf, 1, 0) == -1 && errno == ESPIPE);
    close(p[0]); close(p[1]);

    int a = open(path_a, O_WRONLY | O_TRUNC), b = open(path_a, O_WRONLY | O_APPEND);
    CHECK(write(a, "12345", 5) == 5 && write(b, "ab", 2) == 2);
    CHECK(write(a, "Z", 1) == 1 && write(b, "cd", 2) == 2);
    close(a); close(b);
    fd = open(path_a, O_RDONLY);
    memset(buf, 0, sizeof(buf));
    CHECK(read(fd, buf, sizeof(buf)) == 9 && !memcmp(buf, "12345Zbcd", 9));
    close(fd);
    printf("DFS_PASS positional pread pwrite lseek_errno espipe o_append\n");
}

/* Separate opens share backend data, never their logical positions or mode. */
static void shared_handles(void)
{
    int rw = open(path_a, O_CREAT | O_RDWR | O_TRUNC, 0644);
    int ro = open(path_a, O_RDONLY), wo = open(path_a, O_WRONLY);
    char data[8] = {0};
    CHECK(rw >= 0 && ro >= 0 && wo >= 0);
    CHECK(write(rw, "abcdef", 6) == 6);
    CHECK(read(ro, data, 3) == 3 && !memcmp(data, "abc", 3));
    errno = 0; CHECK(write(ro, "x", 1) == -1 && errno == EBADF);
    errno = 0; CHECK(read(wo, data, 1) == -1 && errno == EBADF);
    errno = 0; CHECK(ftruncate(ro, 0) == -1 && errno == EBADF);
    CHECK(posix_fallocate(ro, 0, 4096) == EBADF);
    CHECK(ftruncate(rw, 2) == 0);
    CHECK(read(ro, data, 1) == 0); /* ro is still at 3, past the new EOF */
    CHECK(lseek(ro, 0, SEEK_SET) == 0);
    CHECK(read(ro, data, sizeof(data)) == 2 && !memcmp(data, "ab", 2));
    CHECK(lseek(rw, 0, SEEK_CUR) == 6);
    close(wo); close(ro); close(rw);
    printf("DFS_PASS shared_handles offsets access_modes truncate_cache\n");
}

/* readv stops at a short read (stdio's readv is not reachable from here). */
static void vectors(void)
{
    char a[8], b[8];
    struct iovec v[2] = { { a, sizeof(a) }, { b, sizeof(b) } };
    int p[2];
    CHECK(pipe(p) == 0);
    CHECK(write(p[1], "xyz", 3) == 3);
    CHECK(readv(p[0], v, 2) == 3 && !memcmp(a, "xyz", 3));   /* did not block in b */
    struct iovec w[2] = { { "he", 2 }, { "llo", 3 } };
    CHECK(writev(p[1], w, 2) == 5);
    CHECK(read(p[0], a, sizeof(a)) == 5 && !memcmp(a, "hello", 5));
    close(p[0]); close(p[1]);
    printf("DFS_PASS readv_short writev\n");
}

/* pipe2, dup3, fcntl descriptor flags, F_DUPFD_CLOEXEC; F_SETFD must not reach
 * the driver (a serial port used to be "suspended"). */
static void descriptors(void)
{
    int p[2], fd, d;
    CHECK(pipe2(p, O_CLOEXEC | O_NONBLOCK) == 0);
    CHECK(fcntl(p[0], F_GETFD) == FD_CLOEXEC && fcntl(p[1], F_GETFD) == FD_CLOEXEC);
    CHECK(fcntl(p[0], F_GETFL) & O_NONBLOCK);
    char c;
    errno = 0; CHECK(read(p[0], &c, 1) == -1 && errno == EAGAIN);
    d = dup(p[0]);
    CHECK(d >= 0 && fcntl(d, F_GETFD) == 0);         /* per descriptor */
    CHECK(dup3(p[0], 20, O_CLOEXEC) == 20 && fcntl(20, F_GETFD) == FD_CLOEXEC);
    errno = 0; CHECK(dup3(20, 20, 0) == -1 && errno == EINVAL);
    fd = fcntl(p[0], F_DUPFD_CLOEXEC, 30);
    CHECK(fd >= 30 && fcntl(fd, F_GETFD) == FD_CLOEXEC);
    errno = 0; CHECK(dup(999) == -1 && errno == EBADF);
    errno = 0; CHECK(dup2(999, 40) == -1 && errno == EBADF);
    close(fd); close(20); close(d); close(p[0]); close(p[1]);
    printf("DFS_PASS pipe2 dup3 fd_cloexec f_dupfd_cloexec dup_errno\n");
}

/* openat family relative to a directory descriptor. */
static void at_calls(void)
{
    struct stat st;
    char buf[PATH_MAX];
    int dfd = open(dir, O_RDONLY | O_DIRECTORY);
    CHECK(dfd >= 0);
    CHECK(mkdirat(dfd, "dfs_probe_sub", 0755) == 0 || errno == EEXIST);
    int fd = openat(dfd, "dfs_probe_sub/f", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    CHECK(fd > 2);                                   /* never stdin */
    CHECK(write(fd, "at", 2) == 2);
    close(fd);
    memset(&st, 0xa5, sizeof(st));
    CHECK(fstatat(dfd, "dfs_probe_sub/f", &st, 0) == 0 && st.st_size == 2 && S_ISREG(st.st_mode));
    CHECK(faccessat(dfd, "dfs_probe_sub/f", R_OK, 0) == 0);
    CHECK(renameat(dfd, "dfs_probe_sub/f", dfd, "dfs_probe_sub/g") == 0);
    CHECK(unlinkat(dfd, "dfs_probe_sub/g", 0) == 0);
    CHECK(unlinkat(dfd, "dfs_probe_sub", AT_REMOVEDIR) == 0);
    CHECK(realpath(dir, buf) != NULL);
    close(dfd);
    printf("DFS_PASS openat fstatat faccessat renameat unlinkat mkdirat realpath\n");
}

/* stat/fstat fields and types; statvfs. */
static void stats(void)
{
    struct stat st, st2;
    struct statvfs sv;
    int p[2];
    memset(&st, 0xa5, sizeof(st));
    CHECK(stat(path_a, &st) == 0 && S_ISREG(st.st_mode) && st.st_nlink == 1 && st.st_uid == 0);
    int fd = open(path_a, O_RDONLY);
    memset(&st2, 0xa5, sizeof(st2));
    CHECK(fstat(fd, &st2) == 0 && st2.st_ino == st.st_ino && st2.st_dev == st.st_dev && st2.st_atime == 0);
    close(fd);
    CHECK(pipe(p) == 0);
    CHECK(fstat(p[0], &st) == 0 && S_ISFIFO(st.st_mode));
    close(p[0]); close(p[1]);
    fd = open("/dev/console", O_RDONLY | O_NOCTTY);
    if (fd >= 0) { CHECK(fstat(fd, &st) == 0 && S_ISCHR(st.st_mode)); close(fd); }
    CHECK(statvfs(dir, &sv) == 0 && sv.f_bsize > 0);
    printf("DFS_PASS stat_zeroed fstat_types statvfs\n");
}

/* poll leaves events alone, so a reused array keeps POLLOUT. */
static void polls(void)
{
    int p[2];
    struct pollfd pf = { 0, POLLOUT, 0 };
    CHECK(pipe(p) == 0);
    pf.fd = p[1];
    CHECK(poll(&pf, 1, 0) == 1 && (pf.revents & POLLOUT) && pf.events == POLLOUT);
    CHECK(poll(&pf, 1, 0) == 1 && (pf.revents & POLLOUT));
    pf.fd = p[0]; pf.events = POLLIN; pf.revents = 0x7fff;
    CHECK(poll(&pf, 1, 0) == 0 && pf.revents == 0);  /* timeout clears revents */
    close(p[0]); close(p[1]);
    printf("DFS_PASS poll_events_kept revents_cleared\n");
}

/* readdir d_type; rename replaces; rmdir of a file; getcwd ERANGE. */
static void names(void)
{
    char small[1];
    CHECK(write_file(path_a, "A") == 0 && write_file(path_b, "B") == 0);
    CHECK(rename(path_b, path_a) == 0);
    FILE *f = fopen(path_a, "r");
    CHECK(f && fgetc(f) == 'B');
    if (f) fclose(f);
    errno = 0; CHECK(rmdir(path_a) == -1 && errno == ENOTDIR);
    CHECK(mkdir(path_sub, 0755) == 0 || errno == EEXIST);
    errno = 0; CHECK(mkdir(path_sub, 0755) == -1 && errno == EEXIST);
    int seen_file = 0, seen_dir = 0;
    DIR *d = opendir(dir);
    CHECK(d != NULL);
    struct dirent *e;
    while (d && (e = readdir(d)))
    {
        if (!strcmp(e->d_name, strrchr(path_a, '/') + 1)) seen_file = e->d_type == DT_REG;
        if (!strcmp(e->d_name, strrchr(path_sub, '/') + 1)) seen_dir = e->d_type == DT_DIR;
    }
    if (d) closedir(d);
    CHECK(seen_file && seen_dir);
    CHECK(rmdir(path_sub) == 0);
    errno = 0; CHECK(getcwd(small, sizeof(small)) == NULL && errno == ERANGE);
    CHECK(unlink(path_a) == 0);
    printf("DFS_PASS rename_replace rmdir_enotdir mkdir_eexist d_type getcwd_erange\n");
}

/* Close-on-exec: the exec'd image no longer has the descriptor. */
static void cloexec_exec(const char *self)
{
    int fd = open(path_b, O_CREAT | O_RDWR | O_CLOEXEC, 0644), keep = open(path_b, O_RDONLY);
    CHECK(fd >= 0 && keep >= 0 && fcntl(fd, F_GETFD) == FD_CLOEXEC);
    char fdarg[16], keeparg[16];
    snprintf(fdarg, sizeof(fdarg), "%d", fd);
    snprintf(keeparg, sizeof(keeparg), "%d", keep);
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0)
    {
        char *argv[] = { (char *)self, "cloexec-child", fdarg, keeparg, NULL };
        execv(self, argv);
        _exit(9);
    }
    int status = 0;
    CHECK(pid > 0 && waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(fd); close(keep); unlink(path_b);
    printf("DFS_PASS o_cloexec_closed_on_exec\n");
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "cloexec-child"))
    {
        /* argv[2] was close-on-exec, argv[3] was not. */
        int gone = fcntl(atoi(argv[2]), F_GETFD) == -1 && errno == EBADF;
        int kept = fcntl(atoi(argv[3]), F_GETFD) == 0;
        return gone && kept ? 0 : 1;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    snprintf(dir, sizeof(dir), "%s", argc > 1 ? argv[1] : "/sdcard");
    snprintf(path_a, sizeof(path_a), "%s/dfs_probe_a", dir);
    snprintf(path_b, sizeof(path_b), "%s/dfs_probe_b", dir);
    snprintf(path_sub, sizeof(path_sub), "%s/dfs_probe_d", dir);
    positional();
    shared_handles();
    vectors();
    descriptors();
    at_calls();
    stats();
    polls();
    names();
    if (argv[0][0] == '/')
        cloexec_exec(argv[0]);
    printf("DFS_MUSL failures=%d dir=%s\n", failures, dir);
    return failures ? 1 : 0;
}

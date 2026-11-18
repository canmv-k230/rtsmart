/* Runs argv[1..] as a forked child and prints its real exit status as
 * "EXIT_PROBE code=N" or "EXIT_PROBE signal=N"; msh does not report it.
 * Build with pty_lifetime_board.mk. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "hal_syscall.h"
extern char **environ;
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    if (strcmp(argv[1], "--control-success") == 0) return 0;
    fflush(NULL);
    pid_t child = fork();
    if (child < 0) { perror("fork"); return 125; }
    if (child == 0) {
        syscall(_NRSYS_execve, (long)argv[1], (long)&argv[1], (long)environ);
        perror("execve");
        _exit(127);
    }
    int status = 0;
    long waited;
    do { waited = syscall(_NRSYS_waitpid, (long)child, (long)&status, 0); }
    while ((waited == -1 && errno == EINTR) || waited == -EINTR);
    if (waited != child) { printf("EXIT_PROBE wait failed=%ld\n", waited); return 125; }
    if (WIFEXITED(status)) { printf("EXIT_PROBE code=%d raw=%d\n", WEXITSTATUS(status), status); return WEXITSTATUS(status); }
    printf("EXIT_PROBE signal=%d raw=%d\n", WTERMSIG(status), status);
    return 128 + WTERMSIG(status);
}

// test_exec: dispatches to a single exec-family entry point chosen by argv[1].
// On success, control transfers to /usr/bin/env which dumps the child
// environment for the harness to verify. On failure, prints EXEC_FAILED:<msg>
// and exits nonzero so the harness can distinguish exec failure from wrong
// child env.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static const char *CHILD_PATH = "/usr/bin/env";
static char *const CHILD_ARGV[] = {"env", NULL};

static int fail(const char *tag) {
    fprintf(stderr, "EXEC_FAILED:%s:%s\n", tag, strerror(errno));
    return 1;
}

static int run_execve(void) {
    execve(CHILD_PATH, CHILD_ARGV, environ);
    return fail("execve");
}

static int run_execvp(void) {
    execvp("env", CHILD_ARGV);
    return fail("execvp");
}

static int run_execv(void) {
    execv(CHILD_PATH, CHILD_ARGV);
    return fail("execv");
}

static int run_execl(void) {
    execl(CHILD_PATH, "env", (char *)NULL);
    return fail("execl");
}

static int run_execlp(void) {
    execlp("env", "env", (char *)NULL);
    return fail("execlp");
}

static int run_execle(void) {
    execle(CHILD_PATH, "env", (char *)NULL, environ);
    return fail("execle");
}

static int run_execvpe(void) {
    execvpe("env", CHILD_ARGV, environ);
    return fail("execvpe");
}

static int wait_child(pid_t pid) {
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return fail("waitpid");
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

static int run_posix_spawn(void) {
    pid_t pid;
    int r = posix_spawn(&pid, CHILD_PATH, NULL, NULL, CHILD_ARGV, environ);
    if (r != 0) {
        errno = r;
        return fail("posix_spawn");
    }
    return wait_child(pid);
}

static int run_posix_spawnp(void) {
    pid_t pid;
    int r = posix_spawnp(&pid, "env", NULL, NULL, CHILD_ARGV, environ);
    if (r != 0) {
        errno = r;
        return fail("posix_spawnp");
    }
    return wait_child(pid);
}

static int run_fexecve(void) {
    int fd = open(CHILD_PATH, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return fail("open");
    fexecve(fd, CHILD_ARGV, environ);
    return fail("fexecve");
}

// Doubly-nested exec: exec /bin/sh -c "env" — verifies rules propagate through
// a chain and LD_PRELOAD removal actually stops propagation at depth 1.
static int run_grandchild_depth(void) {
    // Intermediate shell invokes env; if LD_PRELOAD was not stripped, the
    // shell itself would still have libchildenv loaded and would re-apply
    // rules to its own child. Stripping means grandchild env == child env.
    execlp("sh", "sh", "-c", "env", (char *)NULL);
    return fail("execlp/sh");
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <method>\n", argv[0]);
        return 2;
    }
    const char *m = argv[1];

    if (!strcmp(m, "execve"))        return run_execve();
    if (!strcmp(m, "execvp"))        return run_execvp();
    if (!strcmp(m, "execv"))         return run_execv();
    if (!strcmp(m, "execl"))         return run_execl();
    if (!strcmp(m, "execlp"))        return run_execlp();
    if (!strcmp(m, "execle"))        return run_execle();
    if (!strcmp(m, "execvpe"))       return run_execvpe();
    if (!strcmp(m, "posix_spawn"))   return run_posix_spawn();
    if (!strcmp(m, "posix_spawnp"))  return run_posix_spawnp();
    if (!strcmp(m, "fexecve"))       return run_fexecve();
    if (!strcmp(m, "grandchild"))    return run_grandchild_depth();

    fprintf(stderr, "unknown method: %s\n", m);
    return 2;
}

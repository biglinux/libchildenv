// libchildenv: LD_PRELOAD library that strips/injects environment variables
// in child processes per the CHILD_ENV_RULES rule list.
//
// Rule syntax (comma-separated): "VAR" unsets, "VAR=value" sets/overwrites.
//
// Hooks every exec*/posix_spawn entry point so spawn paths used by Qt6,
// GLib, Python, Go, systemd, etc. are all covered. A library constructor
// also strips unset rules from the host's own environ — without this,
// callers like KIO/KProcessRunner copy environ verbatim into D-Bus
// StartTransientUnit calls, leaking LD_PRELOAD past every exec hook.
//
// By the time the constructor runs, ld.so has already dlopen'd every
// LD_PRELOAD entry, so removing the variable from environ does not
// unload anything — it only blocks downstream propagation.

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

extern char **environ;

// Cached CHILD_ENV_RULES, captured before strip_host_environ() removes the
// variable from environ. Set once in the library constructor and read-only
// after main() starts, so no synchronization is needed.
static char *cached_rules = NULL;

// ---------- env builder ----------

static char **copy_envp(char *const envp[]) {
    int n = 0;
    if (envp) for (char *const *e = envp; *e; ++e) n++;
    char **out = malloc(sizeof(char *) * (n + 1));
    if (!out) return NULL;
    for (int i = 0; i < n; i++) {
        out[i] = strdup(envp[i]);
        if (!out[i]) { while (i--) free(out[i]); free(out); return NULL; }
    }
    out[n] = NULL;
    return out;
}

static void free_envp(char **e) {
    if (!e) return;
    for (char **p = e; *p; ++p) free(*p);
    free(e);
}

// Apply CHILD_ENV_RULES on top of `envp`, returning a freshly allocated array.
// Returns NULL on OOM. If no rules are set, copies envp verbatim.
//
// Reads from `cached_rules` (captured in the constructor) so that the rule
// list survives even after strip_host_environ() removes CHILD_ENV_RULES from
// the host environ. Falls back to getenv() if the constructor never ran
// (e.g., static link or interposition order edge case).
static char **build_child_env(char *const envp[]) {
    char *raw = cached_rules ? cached_rules : getenv("CHILD_ENV_RULES");
    if (!raw || !*raw) return copy_envp(envp);

    char *rules_str = strdup(raw);
    if (!rules_str) return NULL;

    int max_rules = 1;
    for (char *p = rules_str; *p; p++) if (*p == ',') max_rules++;

    typedef struct { char *name, *value; } Rule;
    Rule *rules = calloc((size_t)max_rules, sizeof(Rule));
    if (!rules) { free(rules_str); return NULL; }

    int rc = 0;
    char *str_ptr = rules_str, *tok;
    while ((tok = strsep(&str_ptr, ",")) != NULL) {
        if (!*tok || *tok == '=') continue;
        char *eq = strchr(tok, '=');
        if (eq) { *eq = '\0'; rules[rc].value = eq + 1; }
        rules[rc].name = tok;
        rc++;
    }

    int n = 0;
    if (envp) for (char *const *e = envp; *e; ++e) n++;

    char **out = malloc(sizeof(char *) * ((size_t)n + (size_t)rc + 1));
    if (!out) { free(rules_str); free(rules); return NULL; }

    int oi = 0;
    if (envp) for (char *const *e = envp; *e; ++e) {
        char *eq = strchr(*e, '=');
        size_t name_len = eq ? (size_t)(eq - *e) : strlen(*e);
        bool ruled = false;
        for (int i = 0; i < rc; i++) {
            if (strlen(rules[i].name) == name_len
                && !strncmp(*e, rules[i].name, name_len)) { ruled = true; break; }
        }
        if (!ruled && !(out[oi] = strdup(*e))) goto oom;
        if (!ruled) oi++;
    }
    for (int i = 0; i < rc; i++) {
        if (!rules[i].value) continue;
        size_t len = strlen(rules[i].name) + strlen(rules[i].value) + 2;
        if (!(out[oi] = malloc(len))) goto oom;
        snprintf(out[oi++], len, "%s=%s", rules[i].name, rules[i].value);
    }
    out[oi] = NULL;
    free(rules_str); free(rules);
    return out;

oom:
    while (oi--) free(out[oi]);
    free(out); free(rules_str); free(rules);
    return NULL;
}

// ---------- host-process strip (constructor) ----------

// Strip unset rules ("VAR") from our own environ. "VAR=value" rules are
// child-only — applying them to the host could clobber state callers rely on.
//
// Caches CHILD_ENV_RULES into `cached_rules` before stripping, so hooks keep
// working after the variable leaves the environ. CHILD_ENV_RULES itself is
// stripped too — otherwise it leaks to children spawned via paths that
// bypass exec hooks (KIO::KProcessRunner → systemd StartTransientUnit, which
// copies the host's environ verbatim into the new scope unit).
__attribute__((constructor))
static void strip_host_environ(void) {
    char *raw = getenv("CHILD_ENV_RULES");
    if (!raw || !*raw) return;
    cached_rules = strdup(raw);
    char *s = strdup(raw);
    if (!s) return;
    char *p = s, *tok;
    while ((tok = strsep(&p, ",")) != NULL) {
        if (!*tok || *tok == '=' || strchr(tok, '=')) continue;
        unsetenv(tok);
    }
    free(s);
}

// ---------- exec/spawn hooks ----------

#define WRAP_RET(call) do { \
    int saved = errno; free_envp(new_envp); errno = saved; return (call); \
} while (0)

int execve(const char *path, char *const argv[], char *const envp[]) {
    static int (*real)(const char *, char *const *, char *const *);
    if (!real) real = dlsym(RTLD_NEXT, "execve");
    if (!real) { errno = ENOSYS; return -1; }
    char **new_envp = build_child_env(envp);
    if (!new_envp) { errno = ENOMEM; return -1; }
    int r = real(path, argv, new_envp);
    int saved = errno; free_envp(new_envp); errno = saved;
    return r;
}

int execvpe(const char *file, char *const argv[], char *const envp[]) {
    static int (*real)(const char *, char *const *, char *const *);
    if (!real) real = dlsym(RTLD_NEXT, "execvpe");
    if (!real) { errno = ENOSYS; return -1; }
    char **new_envp = build_child_env(envp);
    if (!new_envp) { errno = ENOMEM; return -1; }
    int r = real(file, argv, new_envp);
    int saved = errno; free_envp(new_envp); errno = saved;
    return r;
}

// execv/execvp: route through real_execve/real_execvpe so we pass our
// modified envp explicitly, avoiding a global `environ` swap (race-prone).
int execv(const char *path, char *const argv[]) {
    static int (*real)(const char *, char *const *, char *const *);
    if (!real) real = dlsym(RTLD_NEXT, "execve");
    if (!real) { errno = ENOSYS; return -1; }
    char **new_envp = build_child_env(environ);
    if (!new_envp) { errno = ENOMEM; return -1; }
    int r = real(path, argv, new_envp);
    int saved = errno; free_envp(new_envp); errno = saved;
    return r;
}

int execvp(const char *file, char *const argv[]) {
    static int (*real)(const char *, char *const *, char *const *);
    if (!real) real = dlsym(RTLD_NEXT, "execvpe");
    if (!real) { errno = ENOSYS; return -1; }
    char **new_envp = build_child_env(environ);
    if (!new_envp) { errno = ENOMEM; return -1; }
    int r = real(file, argv, new_envp);
    int saved = errno; free_envp(new_envp); errno = saved;
    return r;
}

// va_list passed by pointer — passing by value breaks on aarch64/s390x where
// va_list is a struct rather than an array. Helper consumes the NULL
// terminator so callers (execle) can read trailing args (envp) directly.
static char **build_argv_from_va(const char *arg0, va_list *ap) {
    va_list cnt; va_copy(cnt, *ap);
    int n = 1;
    while (va_arg(cnt, char *) != NULL) n++;
    va_end(cnt);
    char **argv = malloc(sizeof(char *) * ((size_t)n + 1));
    if (!argv) return NULL;
    argv[0] = (char *)arg0;
    for (int i = 1; i < n; i++) argv[i] = va_arg(*ap, char *);
    argv[n] = NULL;
    (void)va_arg(*ap, char *); // consume NULL terminator
    return argv;
}

int execl(const char *path, const char *arg, ...) {
    va_list ap; va_start(ap, arg);
    char **argv = build_argv_from_va(arg, &ap);
    va_end(ap);
    if (!argv) { errno = ENOMEM; return -1; }
    int r = execv(path, argv);
    int saved = errno; free(argv); errno = saved;
    return r;
}

int execlp(const char *file, const char *arg, ...) {
    va_list ap; va_start(ap, arg);
    char **argv = build_argv_from_va(arg, &ap);
    va_end(ap);
    if (!argv) { errno = ENOMEM; return -1; }
    int r = execvp(file, argv);
    int saved = errno; free(argv); errno = saved;
    return r;
}

int execle(const char *path, const char *arg, ...) {
    va_list ap; va_start(ap, arg);
    char **argv = build_argv_from_va(arg, &ap);
    if (!argv) { va_end(ap); errno = ENOMEM; return -1; }
    char *const *envp = va_arg(ap, char *const *);
    va_end(ap);
    int r = execve(path, argv, envp);
    int saved = errno; free(argv); errno = saved;
    return r;
}

// posix_spawn family: required for Qt6 QProcess, GLib g_spawn_async,
// Python subprocess, Go os/exec — they bypass exec* entirely. Returns an
// errno value directly (not -1/errno).
int posix_spawn(pid_t *pid, const char *path,
                const posix_spawn_file_actions_t *fa,
                const posix_spawnattr_t *attr,
                char *const argv[], char *const envp[]) {
    static int (*real)(pid_t *, const char *,
        const posix_spawn_file_actions_t *, const posix_spawnattr_t *,
        char *const *, char *const *);
    if (!real) real = dlsym(RTLD_NEXT, "posix_spawn");
    if (!real) return ENOSYS;
    char **new_envp = build_child_env(envp);
    if (!new_envp) return ENOMEM;
    int r = real(pid, path, fa, attr, argv, new_envp);
    free_envp(new_envp);
    return r;
}

int fexecve(int fd, char *const argv[], char *const envp[]) {
    static int (*real)(int, char *const *, char *const *);
    if (!real) real = dlsym(RTLD_NEXT, "fexecve");
    if (!real) { errno = ENOSYS; return -1; }
    char **new_envp = build_child_env(envp);
    if (!new_envp) { errno = ENOMEM; return -1; }
    int r = real(fd, argv, new_envp);
    int saved = errno; free_envp(new_envp); errno = saved;
    return r;
}

int posix_spawnp(pid_t *pid, const char *file,
                 const posix_spawn_file_actions_t *fa,
                 const posix_spawnattr_t *attr,
                 char *const argv[], char *const envp[]) {
    static int (*real)(pid_t *, const char *,
        const posix_spawn_file_actions_t *, const posix_spawnattr_t *,
        char *const *, char *const *);
    if (!real) real = dlsym(RTLD_NEXT, "posix_spawnp");
    if (!real) return ENOSYS;
    char **new_envp = build_child_env(envp);
    if (!new_envp) return ENOMEM;
    int r = real(pid, file, fa, attr, argv, new_envp);
    free_envp(new_envp);
    return r;
}

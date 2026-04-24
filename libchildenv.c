#define _GNU_SOURCE
#include <stddef.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <errno.h>
#include <stdarg.h>
#include <spawn.h>
#include <sys/types.h>

// Pointer to the global environment variable, used by execvp and family.
extern char **environ;

// Forward declarations
static void free_child_environment(char **mod_envp);
static char **create_child_environment(char *const envp[]);

/**
 * @brief Creates a new, controlled environment for a child process based on rules.
 *
 * This function is the core of the library. It builds a completely new environment
 * array (`new_envp`) based on the parent's environment and a set of rules.
 *
 * Rules are read from the `CHILD_ENV_RULES` environment variable, comma-separated.
 * - To unset a variable: `VAR_NAME`
 * - To set/overwrite a variable: `VAR_NAME=new_value`
 *
 * @param envp The original environment of the parent process.
 * @return A new, dynamically allocated, self-contained environment array on success.
 *         The caller is responsible for freeing this array and its contents using
 *         `free_child_environment`. Returns `NULL` on any error (e.g., memory).
 */
static char **copy_envp(char *const envp[]) {
    int env_count = 0;
    if (envp != NULL) {
        for (char *const *e = envp; *e != NULL; ++e) env_count++;
    }

    char **envp_copy = malloc(sizeof(char *) * (env_count + 1));
    if (!envp_copy) return NULL;

    for (int i = 0; i < env_count; ++i) {
        envp_copy[i] = strdup(envp[i]);
        if (!envp_copy[i]) {
            for (int j = 0; j < i; ++j) free(envp_copy[j]);
            free(envp_copy);
            return NULL;
        }
    }
    envp_copy[env_count] = NULL;
    return envp_copy;
}

static char **create_child_environment(char *const envp[]) {
    char *rules_str_orig = getenv("CHILD_ENV_RULES");
    if (rules_str_orig == NULL || rules_str_orig[0] == '\0') {
        return copy_envp(envp);
    }

    char *rules_str = strdup(rules_str_orig);
    if (!rules_str) return NULL;

    int max_rules = 1;
    for (char *p = rules_str; *p; p++) {
        if (*p == ',') max_rules++;
    }

    typedef struct {
        char *name;
        char *value;
        bool applied;
    } Rule;

    Rule *rules = calloc((size_t)max_rules, sizeof(Rule));
    if (!rules) {
        free(rules_str);
        return NULL;
    }

    int rule_count = 0;
    char *str_ptr = rules_str;
    char *rule_token;
    while ((rule_token = strsep(&str_ptr, ",")) != NULL && rule_count < max_rules) {
        if (*rule_token == '\0' || *rule_token == '=') continue;

        char *eq_ptr = strchr(rule_token, '=');
        if (eq_ptr) {
            *eq_ptr = '\0';
            rules[rule_count].name = rule_token;
            rules[rule_count].value = eq_ptr + 1;
        } else {
            rules[rule_count].name = rule_token;
            rules[rule_count].value = NULL;
        }
        rules[rule_count].applied = false;
        rule_count++;
    }

    int env_count = 0;
    if (envp != NULL) {
        for (char *const *e = envp; *e != NULL; ++e) env_count++;
    }

    char **new_envp = malloc(sizeof(char *) * ((size_t)env_count + (size_t)rule_count + 1));
    if (!new_envp) {
        free(rules_str);
        free(rules);
        return NULL;
    }

    int new_env_idx = 0;
    bool success = true;

    if (envp != NULL) {
        for (char *const *e = envp; *e != NULL; ++e) {
            char *var = *e;
            char *eq_ptr = strchr(var, '=');
            size_t name_len = (eq_ptr) ? (size_t)(eq_ptr - var) : strlen(var);
            bool var_is_ruled = false;

            for (int i = 0; i < rule_count; i++) {
                size_t rule_name_len = strlen(rules[i].name);
                if (rule_name_len == name_len && strncmp(var, rules[i].name, name_len) == 0) {
                    var_is_ruled = true;
                    rules[i].applied = true;
                    break;
                }
            }

            if (!var_is_ruled) {
                new_envp[new_env_idx] = strdup(var);
                if (!new_envp[new_env_idx]) {
                    success = false;
                    break;
                }
                new_env_idx++;
            }
        }
    }

    if (success) {
        for (int i = 0; i < rule_count; i++) {
            if (rules[i].value) {
                size_t len = strlen(rules[i].name) + strlen(rules[i].value) + 2;
                char *new_var = malloc(len);
                if (!new_var) {
                    success = false;
                    break;
                }
                snprintf(new_var, len, "%s=%s", rules[i].name, rules[i].value);
                new_envp[new_env_idx++] = new_var;
            }
        }
    }

    free(rules_str);
    free(rules);

    if (!success) {
        for (int i = 0; i < new_env_idx; i++) free(new_envp[i]);
        free(new_envp);
        return NULL;
    }

    new_envp[new_env_idx] = NULL;
    return new_envp;
}

/**
 * @brief Frees a self-contained environment array.
 */
static void free_child_environment(char **mod_envp) {
    if (mod_envp == NULL) return;
    for (char **e = mod_envp; *e != NULL; ++e) {
        free(*e);
    }
    free(mod_envp);
}

// --- Wrappers for the exec function family ---

int execve(const char *pathname, char *const argv[], char *const envp[]) {
    static int (*real_execve)(const char *, char *const *, char *const *) = NULL;
    if (!real_execve) real_execve = dlsym(RTLD_NEXT, "execve");
    if (!real_execve) { errno = ENOSYS; return -1; }

    char **new_envp = create_child_environment(envp);
    if (!new_envp) {
        errno = ENOMEM;
        return -1;
    }

    int result = real_execve(pathname, argv, new_envp);
    int saved_errno = errno;
    free_child_environment(new_envp); // Only runs if execve fails
    errno = saved_errno;
    return result;
}

int execvpe(const char *file, char *const argv[], char *const envp[]) {
    static int (*real_execvpe)(const char *, char *const *, char *const *) = NULL;
    if (!real_execvpe) real_execvpe = dlsym(RTLD_NEXT, "execvpe");
    if (!real_execvpe) { errno = ENOSYS; return -1; }

    char **new_envp = create_child_environment(envp);
    if (!new_envp) {
        errno = ENOMEM;
        return -1;
    }

    int result = real_execvpe(file, argv, new_envp);
    int saved_errno = errno;
    free_child_environment(new_envp); // Only runs if execvpe fails
    errno = saved_errno;
    return result;
}

// execvp has no envp parameter; we route through real_execvpe so we can pass
// our modified envp explicitly, avoiding a global `environ` swap (racy under
// threads and visible to any signal handler reading environ).
int execvp(const char *file, char *const argv[]) {
    static int (*real_execvpe)(const char *, char *const *, char *const *) = NULL;
    if (!real_execvpe) real_execvpe = dlsym(RTLD_NEXT, "execvpe");
    if (!real_execvpe) { errno = ENOSYS; return -1; }

    char **new_envp = create_child_environment(environ);
    if (!new_envp) {
        errno = ENOMEM;
        return -1;
    }

    int result = real_execvpe(file, argv, new_envp);
    int saved_errno = errno;
    free_child_environment(new_envp);
    errno = saved_errno;
    return result;
}

// Same rationale as execvp: route execv through real_execve to avoid
// mutating the global `environ`.
int execv(const char *path, char *const argv[]) {
    static int (*real_execve)(const char *, char *const *, char *const *) = NULL;
    if (!real_execve) real_execve = dlsym(RTLD_NEXT, "execve");
    if (!real_execve) { errno = ENOSYS; return -1; }

    char **new_envp = create_child_environment(environ);
    if (!new_envp) {
        errno = ENOMEM;
        return -1;
    }

    int result = real_execve(path, argv, new_envp);
    int saved_errno = errno;
    free_child_environment(new_envp);
    errno = saved_errno;
    return result;
}

// --- Helper for variadic exec functions ---

// Builds argv from variadic args. Takes va_list by pointer because va_list
// is not portable when passed by value (breaks on aarch64, s390x, etc).
// Caller must free the returned argv.
static char **build_argv_from_va_list(const char *arg0, va_list *args) {
    va_list args_counter;
    va_copy(args_counter, *args);
    int argc = 1; // For arg0
    while (va_arg(args_counter, char *) != NULL) {
        argc++;
    }
    va_end(args_counter);

    char **argv = malloc(sizeof(char *) * ((size_t)argc + 1));
    if (!argv) return NULL;

    argv[0] = (char *)arg0;
    for (int i = 1; i < argc; i++) {
        argv[i] = va_arg(*args, char *);
    }
    argv[argc] = NULL;

    // Consume the NULL terminator so trailing args (e.g. envp) stay aligned.
    (void)va_arg(*args, char *);

    return argv;
}

int execl(const char *path, const char *arg, ...) {
    va_list args;
    va_start(args, arg);
    char **argv = build_argv_from_va_list(arg, &args);
    va_end(args);

    if (!argv) {
        errno = ENOMEM;
        return -1;
    }

    int result = execv(path, argv); // Calls our own wrapper
    int saved_errno = errno;
    free(argv);
    errno = saved_errno;
    return result;
}

int execlp(const char *file, const char *arg, ...) {
    va_list args;
    va_start(args, arg);
    char **argv = build_argv_from_va_list(arg, &args);
    va_end(args);

    if (!argv) {
        errno = ENOMEM;
        return -1;
    }

    int result = execvp(file, argv); // Calls our own wrapper
    int saved_errno = errno;
    free(argv);
    errno = saved_errno;
    return result;
}

int execle(const char *path, const char *arg, ...) {
    va_list args;
    va_start(args, arg);
    char **argv = build_argv_from_va_list(arg, &args);

    if (!argv) {
        va_end(args);
        errno = ENOMEM;
        return -1;
    }

    // helper already consumed NULL terminator; envp is next.
    char *const *envp = va_arg(args, char *const *);
    va_end(args);

    int result = execve(path, argv, envp); // Calls our own wrapper
    int saved_errno = errno;
    free(argv);
    errno = saved_errno;
    return result;
}

// --- posix_spawn family (bypasses exec* entirely in modern runtimes: Qt
// QProcess, GLib g_spawn_async, Python subprocess, Go os/exec). Without
// hooking these, LD_PRELOAD leaks to every spawned child despite rules. ---

// posix_spawn returns an error number directly (not -1/errno), so we return
// ENOMEM / ENOSYS instead of setting errno.
int posix_spawn(pid_t *pid, const char *path,
                const posix_spawn_file_actions_t *file_actions,
                const posix_spawnattr_t *attrp,
                char *const argv[], char *const envp[]) {
    static int (*real_posix_spawn)(pid_t *, const char *,
        const posix_spawn_file_actions_t *, const posix_spawnattr_t *,
        char *const *, char *const *) = NULL;
    if (!real_posix_spawn) real_posix_spawn = dlsym(RTLD_NEXT, "posix_spawn");
    if (!real_posix_spawn) return ENOSYS;

    char **new_envp = create_child_environment(envp);
    if (!new_envp) return ENOMEM;

    // glibc's posix_spawn either blocks until child execs (CLONE_VM path) or
    // fork+execs with COW. In both cases new_envp is no longer referenced by
    // the child once this returns, so freeing here is safe.
    int result = real_posix_spawn(pid, path, file_actions, attrp, argv, new_envp);
    free_child_environment(new_envp);
    return result;
}

int posix_spawnp(pid_t *pid, const char *file,
                 const posix_spawn_file_actions_t *file_actions,
                 const posix_spawnattr_t *attrp,
                 char *const argv[], char *const envp[]) {
    static int (*real_posix_spawnp)(pid_t *, const char *,
        const posix_spawn_file_actions_t *, const posix_spawnattr_t *,
        char *const *, char *const *) = NULL;
    if (!real_posix_spawnp) real_posix_spawnp = dlsym(RTLD_NEXT, "posix_spawnp");
    if (!real_posix_spawnp) return ENOSYS;

    char **new_envp = create_child_environment(envp);
    if (!new_envp) return ENOMEM;

    int result = real_posix_spawnp(pid, file, file_actions, attrp, argv, new_envp);
    free_child_environment(new_envp);
    return result;
}

int fexecve(int fd, char *const argv[], char *const envp[]) {
    static int (*real_fexecve)(int, char *const *, char *const *) = NULL;
    if (!real_fexecve) real_fexecve = dlsym(RTLD_NEXT, "fexecve");
    if (!real_fexecve) { errno = ENOSYS; return -1; }

    char **new_envp = create_child_environment(envp);
    if (!new_envp) {
        errno = ENOMEM;
        return -1;
    }

    int result = real_fexecve(fd, argv, new_envp);
    int saved_errno = errno;
    free_child_environment(new_envp);
    errno = saved_errno;
    return result;
}

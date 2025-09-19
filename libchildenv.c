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
static char **create_child_environment(char *const envp[]) {
    char *rules_str_orig = getenv("CHILD_ENV_RULES");
    if (rules_str_orig == NULL || envp == NULL) {
        int env_count = 0;
        for (char *const *e = envp; *e != NULL; ++e) env_count++;

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

    char *rules_str = strdup(rules_str_orig);
    if (!rules_str) return NULL;

    int max_rules = 0;
    if (rules_str[0] != '\0') {
        max_rules = 1;
        for (char *p = rules_str; *p; p++) {
            if (*p == ',') max_rules++;
        }
    }

    typedef struct {
        char *name;
        char *value;
        bool applied;
    } Rule;

    Rule *rules = calloc(max_rules, sizeof(Rule));
    if (!rules) {
        free(rules_str);
        return NULL;
    }

    int rule_count = 0;
    char *str_ptr = rules_str;
    char *rule_token;
    while ((rule_token = strsep(&str_ptr, ",")) != NULL && rule_count < max_rules) {
        if (*rule_token == '\0') continue;

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
    for (char *const *e = envp; *e != NULL; ++e) env_count++;

    char **new_envp = malloc(sizeof(char *) * (env_count + rule_count + 1));
    if (!new_envp) {
        free(rules_str);
        free(rules);
        return NULL;
    }

    int new_env_idx = 0;
    bool success = true;

    for (char *const *e = envp; *e != NULL; ++e) {
        char *var = *e;
        char *eq_ptr = strchr(var, '=');
        size_t name_len = (eq_ptr) ? (size_t)(eq_ptr - var) : strlen(var);
        bool var_is_ruled = false;

        for (int i = 0; i < rule_count; i++) {
            if (strncmp(var, rules[i].name, name_len) == 0 && rules[i].name[name_len] == '\0') {
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

    char **new_envp = create_child_environment(envp);
    if (!new_envp) {
        errno = ENOMEM;
        return -1;
    }

    int result = real_execve(pathname, argv, new_envp);
    free_child_environment(new_envp); // Only runs if execve fails
    return result;
}

int execvpe(const char *file, char *const argv[], char *const envp[]) {
    static int (*real_execvpe)(const char *, char *const *, char *const *) = NULL;
    if (!real_execvpe) real_execvpe = dlsym(RTLD_NEXT, "execvpe");

    char **new_envp = create_child_environment(envp);
    if (!new_envp) {
        errno = ENOMEM;
        return -1;
    }

    int result = real_execvpe(file, argv, new_envp);
    free_child_environment(new_envp); // Only runs if execvpe fails
    return result;
}

int execvp(const char *file, char *const argv[]) {
    static int (*real_execvp)(const char *, char *const *) = NULL;
    if (!real_execvp) real_execvp = dlsym(RTLD_NEXT, "execvp");

    char **new_envp = create_child_environment(environ);
    if (!new_envp) {
        errno = ENOMEM;
        return -1;
    }
    
    char **original_environ = environ;
    environ = new_envp;
    int result = real_execvp(file, argv);
    environ = original_environ; // Restore on failure
    free_child_environment(new_envp);
    return result;
}

int execv(const char *path, char *const argv[]) {
    static int (*real_execv)(const char *, char *const *) = NULL;
    if (!real_execv) real_execv = dlsym(RTLD_NEXT, "execv");

    char **new_envp = create_child_environment(environ);
    if (!new_envp) {
        errno = ENOMEM;
        return -1;
    }

    char **original_environ = environ;
    environ = new_envp;
    int result = real_execv(path, argv);
    environ = original_environ; // Restore on failure
    free_child_environment(new_envp);
    return result;
}

// --- Helper for variadic exec functions ---

// Builds an argv array from variadic arguments. Caller must free the result.
static char** build_argv_from_va_list(const char* arg0, va_list args) {
    va_list args_counter;
    va_copy(args_counter, args);
    int argc = 1; // For arg0
    while (va_arg(args_counter, char *) != NULL) {
        argc++;
    }
    va_end(args_counter);

    char **argv = malloc(sizeof(char*) * (argc + 1));
    if (!argv) return NULL;

    argv[0] = (char*)arg0;
    for (int i = 1; i < argc; i++) {
        argv[i] = va_arg(args, char*);
    }
    argv[argc] = NULL;

    return argv;
}

int execl(const char *path, const char *arg, ...) {
    va_list args;
    va_start(args, arg);
    char **argv = build_argv_from_va_list(arg, args);
    va_end(args);

    if (!argv) {
        errno = ENOMEM;
        return -1;
    }

    int result = execv(path, argv); // Calls our own wrapper
    free(argv);
    return result;
}

int execlp(const char *file, const char *arg, ...) {
    va_list args;
    va_start(args, arg);
    char **argv = build_argv_from_va_list(arg, args);
    va_end(args);

    if (!argv) {
        errno = ENOMEM;
        return -1;
    }

    int result = execvp(file, argv); // Calls our own wrapper
    free(argv);
    return result;
}

int execle(const char *path, const char *arg, ...) {
    va_list args;
    va_start(args, arg);
    char **argv = build_argv_from_va_list(arg, args);
    
    // After the argv NULL terminator, the next arg is envp
    char *const *envp = va_arg(args, char *const *);
    va_end(args);

    if (!argv) {
        errno = ENOMEM;
        return -1;
    }

    int result = execve(path, argv, envp); // Calls our own wrapper
    free(argv);
    return result;
}

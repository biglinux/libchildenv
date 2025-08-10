#define _GNU_SOURCE
#include <stddef.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <errno.h>

// Pointer to the global environment variable, used by execvp.
extern char **environ;

// Forward declaration
static void free_child_environment(char **mod_envp);

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
 * DESIGN JUSTIFICATION (Memory Management):
 * To ensure correctness and simplify memory management, this function allocates a
 * completely self-contained environment. Every variable string, whether copied from
 * the original environment or newly created, is allocated on the heap. This avoids
 * the complexity and risks of mixing original and new pointers, making the cleanup
 * process trivial and safe.
 *
 * @param envp The original environment of the parent process.
 * @return A new, dynamically allocated, self-contained environment array on success.
 *         The caller is responsible for freeing this array and its contents using
 *         `free_child_environment`. Returns `NULL` on any error (e.g., memory).
 */
static char **create_child_environment(char *const envp[]) {
    char *rules_str_orig = getenv("CHILD_ENV_RULES");
    if (rules_str_orig == NULL || envp == NULL) {
        // No rules, no-op. Return a copy of the original envp to maintain a
        // consistent memory model where the result is always self-contained and freeable.
        int env_count = 0;
        for (char *const *e = envp; *e != NULL; ++e) env_count++;

        char **envp_copy = malloc(sizeof(char *) * (env_count + 1));
        if (!envp_copy) return NULL;

        for (int i = 0; i < env_count; ++i) {
            envp_copy[i] = strdup(envp[i]);
            if (!envp_copy[i]) {
                // On error, free everything allocated so far.
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

    // --- Begin Parsing Rules ---
    // Count rules to allocate memory for them.
    int max_rules = 0;
    if (rules_str[0] != '\0') {
        max_rules = 1;
        for (char *p = rules_str; *p; p++) {
            if (*p == ',') max_rules++;
        }
    }

    typedef struct {
        char *name;
        char *value; // NULL if the rule is to unset the variable.
        bool applied;  // Flag to track if the rule was used.
    } Rule;

    Rule *rules = calloc(max_rules, sizeof(Rule));
    if (!rules) {
        free(rules_str);
        return NULL;
    }

    int rule_count = 0;
    char *str_ptr = rules_str;
    char *rule_token;
    // Use strsep instead of strtok for thread-safety.
    while ((rule_token = strsep(&str_ptr, ",")) != NULL && rule_count < max_rules) {
        if (*rule_token == '\0') continue; // Skip empty tokens

        char *eq_ptr = strchr(rule_token, '=');
        if (eq_ptr) { // Set/Overwrite rule
            *eq_ptr = '\0';
            rules[rule_count].name = rule_token;
            rules[rule_count].value = eq_ptr + 1;
        } else { // Unset rule
            rules[rule_count].name = rule_token;
            rules[rule_count].value = NULL;
        }
        rules[rule_count].applied = false;
        rule_count++;
    }
    // --- End Parsing Rules ---

    // Count original environment variables to allocate a sufficiently large new array.
    int env_count = 0;
    for (char *const *e = envp; *e != NULL; ++e) env_count++;

    // The new environment can have at most `env_count` (from original) + `rule_count` (new vars) elements.
    char **new_envp = malloc(sizeof(char *) * (env_count + rule_count + 1));
    if (!new_envp) {
        free(rules_str);
        free(rules);
        return NULL;
    }

    int new_env_idx = 0;
    bool success = true;

    // --- Begin Building New Environment ---
    // 1. Iterate the original environment, applying rules.
    for (char *const *e = envp; *e != NULL; ++e) {
        char *var = *e;
        char *eq_ptr = strchr(var, '=');
        size_t name_len = (eq_ptr) ? (size_t)(eq_ptr - var) : strlen(var);
        bool var_is_ruled = false;

        for (int i = 0; i < rule_count; i++) {
            // Check if the current variable `var` matches a rule's name.
            if (strncmp(var, rules[i].name, name_len) == 0 && rules[i].name[name_len] == '\0') {
                var_is_ruled = true;
                rules[i].applied = true; // Mark this rule as applied.
                break; // A variable can only match one rule name.
            }
        }

        // If the variable is not affected by any rule, copy it to the new environment.
        if (!var_is_ruled) {
            new_envp[new_env_idx] = strdup(var);
            if (!new_envp[new_env_idx]) {
                success = false;
                break;
            }
            new_env_idx++;
        }
    }

    // 2. Add all new variables ("set/overwrite" rules).
    // An applied "set" rule is an overwrite. An unapplied "set" rule is a new addition.
    if (success) {
        for (int i = 0; i < rule_count; i++) {
            if (rules[i].value) { // This is a "set/overwrite" rule.
                size_t len = strlen(rules[i].name) + strlen(rules[i].value) + 2; // name=value\0
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
    // --- End Building New Environment ---

    free(rules_str);
    free(rules);

    if (!success) {
        // Cleanup on failure
        for (int i = 0; i < new_env_idx; i++) {
            free(new_envp[i]);
        }
        free(new_envp);
        return NULL;
    }

    new_envp[new_env_idx] = NULL; // Terminate the new environment array.

    return new_envp;
}

/**
 * @brief Frees a self-contained environment array created by create_child_environment.
 *
 * @param mod_envp The environment array to free. If it's NULL, the function does nothing.
 */
static void free_child_environment(char **mod_envp) {
    if (mod_envp == NULL) return;

    for (char **e = mod_envp; *e != NULL; ++e) {
        free(*e); // Free each variable string.
    }
    free(mod_envp); // Free the array of pointers.
}


// --- Wrappers for the exec function family ---

int execve(const char *pathname, char *const argv[], char *const envp[]) {
    static int (*real_execve)(const char *, char *const *, char *const *) = NULL;
    if (!real_execve) {
        real_execve = dlsym(RTLD_NEXT, "execve");
    }

    char **new_envp = create_child_environment(envp);
    if (!new_envp) {
        errno = ENOMEM;
        return -1; // Fail-closed: Cannot execute if env creation fails.
    }

    int result = real_execve(pathname, argv, new_envp);
    
    // These lines only run if execve fails.
    free_child_environment(new_envp);
    return result;
}

int execvpe(const char *file, char *const argv[], char *const envp[]) {
    static int (*real_execvpe)(const char *, char *const *, char *const *) = NULL;
    if (!real_execvpe) {
        real_execvpe = dlsym(RTLD_NEXT, "execvpe");
    }

    char **new_envp = create_child_environment(envp);
    if (!new_envp) {
        errno = ENOMEM;
        return -1; // Fail-closed
    }

    int result = real_execvpe(file, argv, new_envp);

    free_child_environment(new_envp);
    return result;
}

int execvp(const char *file, char *const argv[]) {
    static int (*real_execvpe)(const char *, char *const *, char *const *) = NULL;
    if (!real_execvpe) {
        // execvp can be implemented by calling execvpe with the global `environ`.
        real_execvpe = dlsym(RTLD_NEXT, "execvpe");
    }

    // Use the global `environ` as the base, as per execvp's behavior.
    char **new_envp = create_child_environment(environ);
    if (!new_envp) {
        errno = ENOMEM;
        return -1; // Fail-closed
    }
    
    // We can use execvpe to get the PATH search behavior of execvp.
    int result = real_execvpe(file, argv, new_envp);
    
    free_child_environment(new_envp);
    return result;
}

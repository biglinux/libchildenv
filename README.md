<img width="1368" height="805" alt="libchildenv" src="https://github.com/user-attachments/assets/d05752de-d09c-4414-9bc7-8eb7b4adb168" />


# Better memory management on Linux systems

After weeks of testing different memory allocators to check memory usage by desktop Linux programs, mainly `libtcmalloc.so`, `libjemalloc.so`, and `libmimalloc.so`, I identified significant improvements. However, loading these libs with `LD_PRELOAD` has the side effect that child processes also switch the default memory allocator, which makes it unfeasible for use in some desktop programs.

Besides this library, I count on your help to discover which programs have better memory management using another `malloc`. You can help by sending the information as an issue or on our forum at [www.biglinux.com.br/forum](http://www.biglinux.com.br/forum).

It is also a good idea to inform the project's maintainer about this improvement, because with a few adjustments, the program could use another `malloc` by default, and thus we help make the Linux desktop more efficient. The main point to check is not the initial memory usage, but to verify if during prolonged use the programs are freeing memory or are constantly increasing their consumption.

Some programs I tested and that showed great improvement in memory management, which in some situations with prolonged use multiply their initial memory usage several times, and with other allocators maintain memory usage close to the initial usage:

*   Nemo File Manager
*   Thunar File Manager
*   Pamac Manager
*   Gnome Shell
*   Cinnamon

---

## libchildenv: Modifying environment variables received by child processes

With `libchildenv`, we can use `LD_PRELOAD` to load another memory allocator into the process we are going to run and not pass this change to child processes. But this is just one use case; we can do this with any environment variable, including passing environment variables to child processes without activating them in the process we are running.

`libchildenv` is a shared library (.so) for Linux that intercepts `exec*` family system calls to modify the environment that is passed to new processes. The main objective is to prevent the inheritance of sensitive or configuration environment variables, such as `LD_PRELOAD`, by child processes.

---

## Installation

The source code only depends on `glibc` and `libdl`, requiring no other dependencies.

### Compilation

```bash
gcc -shared -fPIC -o libchildenv.so libchildenv.c -ldl
```

### System-Wide Installation

To make the library globally available without needing to specify its full path, move it to a standard library directory.

1.  **Move the library (e.g., to `/usr/local/lib`):**
    ```bash
    sudo mv libchildenv.so /usr/local/lib/
    ```

2.  **Update the dynamic linker's cache:**
    ```bash
    sudo ldconfig
    ```

After these steps, you can refer to the library in `LD_PRELOAD` simply as `libchildenv.so`.

---

## Usage

Operation is configured through two environment variables:

### Environment Variables

*   **`LD_PRELOAD`**: Must contain `libchildenv.so`, optionally along with other libraries (e.g., `libchildenv.so:libtcmalloc.so`).
*   **`CHILD_ENV_RULES`**: Contains the environment modification rules.

### Rule Syntax

Rules are specified in a single, comma-separated string.

*   To **remove** a variable: `VARIABLE_NAME`
*   To **set/overwrite** a variable: `VARIABLE_NAME=VALUE`

---

## Quick Start: Using libchildenv.sh

The easiest way to run a program with an isolated memory allocator is to use the provided `libchildenv.sh` script. This script sets up the correct environment for `libmimalloc`, `libjemalloc`, or `libtcmalloc` with `libchildenv` so that only the target program uses the allocator, and child processes do not inherit it.

### Example: Run Nemo with tcmalloc (child-safe)

```bash
libchildenv.sh tcmalloc nemo
```

### Example: Run a program with jemalloc

```bash
libchildenv.sh jemalloc some_other_program
```

### Example: Run a program with mimalloc

```bash
libchildenv.sh mimalloc some_other_program
```

### Example: Verify loaded libraries in a process

```bash
libchildenv.sh verify nemo
```
This prints the loaded allocator and childenv libraries for the newest `nemo` process, matching the verification style below.

---

## Manual Usage Examples

### 1. Didactic Example: Removing a Variable

This example demonstrates how `libchildenv` removes the `USER` environment variable from a child process.

1.  **Open terminal.**

2.  **In Terminal, start a bash shell with `libchildenv` loaded and the rule to add `MY_TEST` not to main process, only to child:**
    ```bash
    CHILD_ENV_RULES="MY_TEST=biglinux" LD_PRELOAD=libchildenv.so bash
    ```
    *You are now in a new shell where `libchildenv` is active.*

3.  **Still in Terminal, verify that the `MY_TEST` variable exists:**
    ```bash
    echo $MY_TEST
    ```
    *Expected output: (a blank line)*

4.  **Now, start a child process (another `bash` shell) from this shell:**
    ```bash
    bash
    ```

5.  **In this new shell (the child process), check the `USER` variable:**
    ```bash
    echo $MY_TEST
    ```
    *Expected output:*
    ```
    biglinux
    ```

The variable was successfully removed from the child's environment.

### 2. Main Use Case: Isolating Memory Allocators

It is common to use `LD_PRELOAD` to inject memory allocation libraries like `tcmalloc`, `jemalloc`, or `mimalloc` to improve an application's performance. However, this causes all child processes (terminals, text editors, etc.) to also inherit this configuration, which is often undesirable. `libchildenv` solves this.

#### Example with tcmalloc

Objective: Run the `nemo` file manager with `tcmalloc`, but prevent any program opened from `nemo` from inheriting `LD_PRELOAD` and `tcmalloc`'s variables.

```bash
# The rules remove LD_PRELOAD and the tcmalloc configuration variable.
CHILD_ENV_RULES="LD_PRELOAD,TCMALLOC_AGGRESSIVE_DECOMMIT" \
LD_PRELOAD="libchildenv.so:libtcmalloc.so" \
TCMALLOC_AGGRESSIVE_DECOMMIT=1 \
nemo
```

#### Example with jemalloc

```bash
CHILD_ENV_RULES="LD_PRELOAD" \
LD_PRELOAD="libchildenv.so:libjemalloc.so" \
some_other_program
```

#### Example with mimalloc

```bash
CHILD_ENV_RULES="LD_PRELOAD,MIMALLOC_PURGE_DELAY" \
LD_PRELOAD="libchildenv.so:libmimalloc.so" \
MIMALLOC_PURGE_DELAY=0 \
some_other_program
```

---

## Verifying the Isolation

We can confirm that the `nemo` process from the previous example is using the libraries, but its children are not.

1.  **Get the Process ID (PID) of Nemo:**
    ```bash
    pgrep nemo
    ```
    *Will return a number, like `12345`*

2.  **List the mapped libraries in the parent process (nemo):**
    Use the `lsof` (List Open Files) command to inspect the loaded libraries.
    ```bash
    # The $(pgrep nemo) command inserts the PID directly.
    lsof -p $(pgrep nemo) | grep -E 'tcmalloc|childenv|libmimalloc|libjemalloc'
    ```
    *Expected Output (confirms the parent loaded the libraries):*
    ```
    nemo    12345    your_user  mem    REG    8,1    123456    /usr/local/lib/libchildenv.so
    nemo    12345    your_user  mem    REG    8,1    789012    /usr/lib/x86_64-linux-gnu/libtcmalloc.so.4
    ```

3.  **Verify the Child Process (The Final Proof):**
    *   Inside the `nemo` window, open a child process.
    *   In a separate terminal, find the child process's PID (e.g., `pgrep gedit`) and check its libraries:
    ```bash
    # Replace 'gedit' with the name of the process you started
    lsof -p $(pgrep gedit) | grep -E 'tcmalloc|childenv|libmimalloc|libjemalloc'
    ```
    *Expected Output:*
    ```
    (No output)
    ```
    The absence of output proves that the child process did not inherit `LD_PRELOAD` and therefore did not load `libtcmalloc.so` or `libchildenv.so`. The objective was achieved.

---

## Technical Explanation

### Intercepted Functions

Currently, `libchildenv` intercepts the following `glibc` functions:
*   `execve`
*   `execvpe`
*   `execvp`

### How it Works

1.  **Loading:** The library is loaded into a target process before any other by setting `LD_PRELOAD=libchildenv.so`.
2.  **Hooking:** `libchildenv` provides its own implementations of the `exec*` family functions (`execve`, `execvp`). When the target process tries to create a child, the `libchildenv` implementation runs first.
3.  **Resolving the Original Function:** Inside the hooked function, a pointer to the original `glibc` `exec*` function is obtained using `dlsym(RTLD_NEXT, "execve")`.
4.  **Processing Rules:** The library reads and parses the content of the `CHILD_ENV_RULES` environment variable.
5.  **Building the New Environment:** A new environment vector (`char*[]`) is allocated in memory. The library iterates over the parent's environment and applies the rules to build the new vector, unsetting or overwriting variables as specified.
6.  **Execution:** The original `exec*` function is finally called, but with the new, modified environment vector. If the call fails, the allocated memory is freed to prevent memory leaks.
### Intercepted Functions

`libchildenv` currently intercepts the following `glibc` functions:
*   `execve`
*   `execvpe`
*   `execvp`

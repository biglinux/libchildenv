#!/bin/bash
# Integration test suite for libchildenv. Builds the library and a test
# harness, runs each intercepted exec entry point, and verifies child-process
# environment matches expectations under CHILD_ENV_RULES.
#
# Exit status: 0 if all tests pass, 1 otherwise.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_DIR"

SO="$REPO_DIR/libchildenv.so"
BIN="$SCRIPT_DIR/test_exec"

RED=$'\033[0;31m'
GREEN=$'\033[0;32m'
YELLOW=$'\033[0;33m'
RST=$'\033[0m'

pass=0
fail=0
fails=()

build() {
    echo "[build] libchildenv.so"
    gcc -shared -fPIC -O2 -Wall -Wextra -o "$SO" "$REPO_DIR/libchildenv.c" -ldl \
        || { echo "${RED}build failed${RST}"; exit 2; }

    echo "[build] tests/test_exec"
    gcc -O2 -Wall -Wextra -o "$BIN" "$SCRIPT_DIR/test_exec.c" \
        || { echo "${RED}build failed${RST}"; exit 2; }
}

# Run test_exec with given env setup and return captured stdout.
# Args: <method> <rules> [extra env assignments...]
run_capture() {
    local method=$1
    local rules=$2
    shift 2
    # Use env to inject extra vars cleanly. LD_PRELOAD must point at absolute
    # path since we don't install into ldconfig search path.
    env -i \
        PATH="/usr/bin:/bin" \
        HOME="$HOME" \
        LD_PRELOAD="$SO" \
        CHILD_ENV_RULES="$rules" \
        "$@" \
        "$BIN" "$method" 2>&1
}

report_pass() {
    echo "  ${GREEN}PASS${RST} $1"
    pass=$((pass + 1))
}

report_fail() {
    local name=$1 reason=$2 output=$3
    echo "  ${RED}FAIL${RST} $name — $reason"
    echo "    --- captured output ---"
    sed 's/^/    /' <<<"$output"
    echo "    --- end ---"
    fail=$((fail + 1))
    fails+=("$name")
}

# Matrix: each exec method must (a) inject SET_VAR, (b) strip UNSET_VAR,
# (c) strip LD_PRELOAD so grandchildren do not inherit.
test_method() {
    local method=$1
    local out
    out=$(run_capture "$method" "SET_VAR=injected,UNSET_VAR,LD_PRELOAD" \
          UNSET_VAR=should_not_leak)

    if grep -q '^EXEC_FAILED:' <<<"$out"; then
        report_fail "$method" "exec entry point failed" "$out"
        return
    fi
    if ! grep -q '^SET_VAR=injected$' <<<"$out"; then
        report_fail "$method" "SET_VAR not injected" "$out"
        return
    fi
    if grep -q '^UNSET_VAR=' <<<"$out"; then
        report_fail "$method" "UNSET_VAR leaked to child" "$out"
        return
    fi
    if grep -q '^LD_PRELOAD=' <<<"$out"; then
        report_fail "$method" "LD_PRELOAD leaked to child" "$out"
        return
    fi
    report_pass "$method"
}

build

echo ""
echo "=== exec-family matrix ==="
for m in execve execvp execv execl execlp execle execvpe \
         posix_spawn posix_spawnp fexecve; do
    test_method "$m"
done

echo ""
echo "=== edge cases ==="

# Empty CHILD_ENV_RULES → env copied verbatim.
out=$(run_capture execve "" TESTVAR=pass)
if grep -q '^TESTVAR=pass$' <<<"$out" && ! grep -q '^EXEC_FAILED' <<<"$out"; then
    report_pass "empty-rules (passthrough)"
else
    report_fail "empty-rules" "TESTVAR not passed through" "$out"
fi

# CHILD_ENV_RULES unset entirely → env copied verbatim.
out=$(env -i PATH="/usr/bin:/bin" HOME="$HOME" LD_PRELOAD="$SO" TESTVAR=pass \
      "$BIN" execve 2>&1)
if grep -q '^TESTVAR=pass$' <<<"$out"; then
    report_pass "no-rules-var (passthrough)"
else
    report_fail "no-rules-var" "TESTVAR not passed through" "$out"
fi

# Malformed rules: empty tokens, leading '=', trailing comma — must not crash
# and must not wildcard-delete everything (the old empty-name bug).
out=$(run_capture execve ",SET_VAR=ok,,=bad," KEEP_VAR=survived)
if grep -q '^SET_VAR=ok$' <<<"$out" && grep -q '^KEEP_VAR=survived$' <<<"$out"; then
    report_pass "malformed-rules tolerated"
else
    report_fail "malformed-rules" "parser regressed" "$out"
fi

# Value with embedded '=' (rule parser must only split on FIRST '=').
out=$(run_capture execve "EQ_VAR=a=b=c")
if grep -q '^EQ_VAR=a=b=c$' <<<"$out"; then
    report_pass "value with '=' preserved"
else
    report_fail "equals-in-value" "value got truncated" "$out"
fi

# Overwrite semantics: rule with value on a var already present must win.
out=$(run_capture execve "OVR=new" OVR=old)
if grep -q '^OVR=new$' <<<"$out" && ! grep -q '^OVR=old$' <<<"$out"; then
    report_pass "set-rule overrides existing var"
else
    report_fail "overwrite" "rule did not override existing value" "$out"
fi

echo ""
echo "=== grandchild propagation (LD_PRELOAD strip must be effective) ==="
# Child is 'sh -c env'. If LD_PRELOAD leaks, sh would re-load libchildenv and
# UNSET_VAR would still be stripped at the sh→env boundary (false negative).
# We flip the polarity: inject a var in the child ONLY. The grandchild env
# must still see it because sh forwards its env to env(1) unchanged. The
# critical check is that LD_PRELOAD is absent from the grandchild, meaning
# the chain cleaned up at depth 1 and did not re-trigger.
out=$(run_capture grandchild "SET_VAR=deep,LD_PRELOAD" UNSET_VAR=leaked)
if grep -q '^EXEC_FAILED' <<<"$out"; then
    report_fail "grandchild" "shell exec failed" "$out"
elif ! grep -q '^SET_VAR=deep$' <<<"$out"; then
    report_fail "grandchild" "SET_VAR did not reach grandchild via sh" "$out"
elif grep -q '^LD_PRELOAD=' <<<"$out"; then
    report_fail "grandchild" "LD_PRELOAD leaked to grandchild" "$out"
else
    report_pass "grandchild env clean"
fi

echo ""
echo "=== negative baseline (sanity check: harness must catch leaks) ==="
# Without LD_PRELOAD the rules have no effect: UNSET_VAR SHOULD leak.
# If this test reports "pass", the suite above is giving false positives.
out=$(env -i PATH="/usr/bin:/bin" HOME="$HOME" \
      UNSET_VAR=definitely_leaked CHILD_ENV_RULES="UNSET_VAR" \
      "$BIN" execve 2>&1)
if grep -q '^UNSET_VAR=definitely_leaked$' <<<"$out"; then
    report_pass "baseline: rules inert without LD_PRELOAD (harness valid)"
else
    report_fail "baseline" \
        "harness or test binary is broken — UNSET_VAR should have leaked" "$out"
fi

echo ""
echo "=== results ==="
total=$((pass + fail))
if [[ $fail -eq 0 ]]; then
    echo "${GREEN}$pass/$total passed${RST}"
    exit 0
else
    echo "${RED}$fail/$total failed${RST}"
    printf '  - %s\n' "${fails[@]}"
    exit 1
fi

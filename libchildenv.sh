#!/bin/bash
# Wrapper to run a command with libchildenv and a memory allocator.
# Usage: libchildenv.sh [mimalloc|jemalloc|tcmalloc] <command> [args...]
#        libchildenv.sh verify <process_name>
#        libchildenv.sh apply-malloc <binary> <mimalloc|jemalloc|tcmalloc>

set -u

usage() {
    cat >&2 <<EOF
Usage: $0 [mimalloc|jemalloc|tcmalloc] <command> [args...]
       $0 verify <process_name>
       $0 apply-malloc <binary> <mimalloc|jemalloc|tcmalloc>
EOF
}

if [[ $# -lt 1 ]]; then
    usage
    exit 1
fi

option_selected="$1"
shift

# Each allocator is represented as an array so env assignments survive
# expansion without literal quote corruption (bash does not re-parse
# assignment prefixes after variable expansion).
mimalloc_env=(
    "LD_PRELOAD=libchildenv.so:libmimalloc.so"
    "CHILD_ENV_RULES=LD_PRELOAD,MIMALLOC_PURGE_DELAY,CHILD_ENV_RULES"
    "MIMALLOC_PURGE_DELAY=0"
)
jemalloc_env=(
    "LD_PRELOAD=libchildenv.so:libjemalloc.so"
    "CHILD_ENV_RULES=LD_PRELOAD,MALLOC_CONF,CHILD_ENV_RULES"
    "MALLOC_CONF=narenas:1"
)
tcmalloc_env=(
    "LD_PRELOAD=libchildenv.so:libtcmalloc.so"
    "CHILD_ENV_RULES=LD_PRELOAD,TCMALLOC_AGGRESSIVE_DECOMMIT,CHILD_ENV_RULES"
    "TCMALLOC_AGGRESSIVE_DECOMMIT=1"
)

run_with_malloc() {
    local -n env_arr=$1
    shift
    if [[ $# -eq 0 ]]; then
        usage
        exit 1
    fi
    exec env "${env_arr[@]}" "$@"
}

wrap_binary_with_malloc() {
    local bin_path="$1"
    local -n env_arr=$2

    if [[ $EUID -ne 0 ]]; then
        echo "You need root permission" >&2
        exit 1
    fi
    if [[ ! -e "$bin_path" ]]; then
        echo "Binary not found: $bin_path" >&2
        exit 1
    fi
    if ! file -b --mime "$bin_path" | grep -q 'charset=binary'; then
        echo "$bin_path is not a binary (already wrapped or a script)." >&2
        exit 1
    fi
    if [[ -e "$bin_path.orig" ]]; then
        echo "$bin_path.orig already exists; refusing to overwrite." >&2
        exit 1
    fi

    cp -f "$bin_path" "$bin_path.orig"

    # Build env-assignment prefix (properly shell-quoted for embedding).
    local env_line=""
    local item
    for item in "${env_arr[@]}"; do
        env_line+="${item} "
    done

    cat > "$bin_path" <<EOF
#!/bin/sh
exec env ${env_line}"${bin_path}.orig" "\$@"
EOF
    chmod +x "$bin_path"
    echo "Now $bin_path uses custom malloc"
}

case "$option_selected" in
    mimalloc)
        run_with_malloc mimalloc_env "$@"
        ;;
    jemalloc)
        run_with_malloc jemalloc_env "$@"
        ;;
    tcmalloc)
        run_with_malloc tcmalloc_env "$@"
        ;;

    verify)
        if [[ $# -ne 1 ]]; then
            echo "Usage: $0 verify <process_name>" >&2
            exit 1
        fi
        proc_name="$1"
        pid=$(pgrep -fn -- "$proc_name" || true)
        if [[ -z "$pid" ]]; then
            echo "No process named '$proc_name' found." >&2
            exit 1
        fi
        lsof -p "$pid" 2>/dev/null | grep -E 'childenv|mimalloc|jemalloc|tcmalloc'
        ;;

    apply-malloc)
        if [[ $# -ne 2 ]]; then
            echo "Usage: $0 apply-malloc <binary> <mimalloc|jemalloc|tcmalloc>" >&2
            exit 1
        fi
        bin="$1"
        malloc="$2"
        case "$malloc" in
            mimalloc) wrap_binary_with_malloc "$bin" mimalloc_env ;;
            jemalloc) wrap_binary_with_malloc "$bin" jemalloc_env ;;
            tcmalloc) wrap_binary_with_malloc "$bin" tcmalloc_env ;;
            *)
                echo "Unknown allocator: $malloc" >&2
                exit 1
                ;;
        esac
        ;;

    -h|--help|help)
        usage
        ;;

    *)
        echo "Unknown option: $option_selected" >&2
        usage
        exit 1
        ;;
esac

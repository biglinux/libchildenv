#!/bin/bash
# Simple wrapper to run a command with libchildenv and a memory allocator
# Usage: ./libchildenv.sh [mimalloc|jemalloc|tcmalloc] <command> [args...]
#        ./libchildenv.sh verify <process_name>
#        ./libchildenv.sh apply-malloc <binary> <allocator>

option_selected="$1"
shift

apply_mimalloc='LD_PRELOAD="libchildenv.so:libmimalloc.so" CHILD_ENV_RULES="LD_PRELOAD,MIMALLOC_PURGE_DELAY,CHILD_ENV_RULES" MIMALLOC_PURGE_DELAY=0'
apply_jemalloc='LD_PRELOAD="libchildenv.so:libjemalloc.so" CHILD_ENV_RULES="LD_PRELOAD,MALLOC_CONF" MALLOC_CONF=narenas:1,CHILD_ENV_RULES'
apply_tcmalloc='LD_PRELOAD="libchildenv.so:libtcmalloc.so" CHILD_ENV_RULES="LD_PRELOAD,TCMALLOC_AGGRESSIVE_DECOMMIT,CHILD_ENV_RULES" TCMALLOC_AGGRESSIVE_DECOMMIT=1'

wrap_binary_with_malloc() {
    local bin_path="$1"
    local malloc_env="$2"
    if [[ "$UID" != "0" ]]; then
        echo "You need root permission"
        exit 1
    fi
    if file -i "$bin_path" | grep -q charset=binary; then
        cp -f "$bin_path" "$bin_path.orig"
        echo -e "#!/bin/sh\n$malloc_env \"$bin_path.orig\" \"$@\"" > "$bin_path"
        chmod +x "$bin_path"
        echo "Now $bin_path uses custom malloc"
    else
        echo "$bin_path is a script, not applying again."
        exit 1
    fi
}

case "$option_selected" in
    mimalloc)
        $apply_mimalloc exec "$@"
        exit
        ;;

    jemalloc)
        $apply_jemalloc exec "$@"
        exit
        ;;

    tcmalloc)
        $apply_tcmalloc  exec "$@"
        exit
        ;;

    verify)
        if [ $# -ne 1 ]; then
            echo "Usage: $0 verify <process_name>"
            exit
        fi
        PROC="$1"
        PID=$(pgrep -fn "$PROC")
        if [ -z "$PID" ]; then
            echo "No process named '$PROC' found."
            exit 1
        fi
        lsof -p "$PID" 2>/dev/null | grep -E 'childenv|mimalloc|jemalloc|tcmalloc'
        exit
        ;;

    apply-malloc)
        if [ $# -ne 2 ]; then
            echo "Usage: $0 apply-malloc <binary> <mimalloc|jemalloc|tcmalloc>"
            exit 1
        fi
        BIN="$1"
        MALLOC="$2"
        case "$MALLOC" in
            mimalloc)
                wrap_binary_with_malloc "$BIN" "$apply_mimalloc"
                ;;
            jemalloc)
                wrap_binary_with_malloc "$BIN" "$apply_jemalloc"
                ;;
            tcmalloc)
                wrap_binary_with_malloc "$BIN" "$apply_tcmalloc"
                ;;
            *)
                echo "Unknown allocator: $MALLOC"
                exit 1
                ;;
        esac
        ;;

    *)
        echo "Unknown allocator: $option_selected"
        echo "Usage: $0 [mimalloc|jemalloc|tcmalloc] <command> [args...]"
        echo "       $0 verify <process_name>"
        echo "       $0 apply-malloc <binary> <mimalloc|jemalloc|tcmalloc>"
        ;;
esac



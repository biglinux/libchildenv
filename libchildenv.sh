#!/bin/bash
# Simple wrapper to run a command with libchildenv and a memory allocator
# Usage: ./libchildenv.sh [mimalloc|jemalloc|tcmalloc] <command> [args...]
#        ./libchildenv.sh verify <process_name>

if [ $# -lt 1 ]; then
    echo "Usage: $0 [mimalloc|jemalloc|tcmalloc] <command> [args...]"
    echo "       $0 verify <process_name>"
    exit 1
fi

ALLOCATOR="$1"
shift

case "$ALLOCATOR" in
    mimalloc)
        LD_PRELOAD="libchildenv.so:libmimalloc.so" CHILD_ENV_RULES="LD_PRELOAD,MIMALLOC_PURGE_DELAY" MIMALLOC_PURGE_DELAY=0 exec "$@"
        ;;
    jemalloc)
        LD_PRELOAD="libchildenv.so:libjemalloc.so" CHILD_ENV_RULES="LD_PRELOAD,MALLOC_CONF" MALLOC_CONF=narenas:1,metadata_thp:auto,percpu_arena:phycpu exec "$@"
        ;;
    tcmalloc)
        LD_PRELOAD="libchildenv.so:libtcmalloc.so" CHILD_ENV_RULES="LD_PRELOAD,TCMALLOC_AGGRESSIVE_DECOMMIT" TCMALLOC_AGGRESSIVE_DECOMMIT=1 exec "$@"
        ;;
    verify)
        if [ $# -ne 1 ]; then
            echo "Usage: $0 verify <process_name>"
            exit 1
        fi
        PROC="$1"
        PID=$(pgrep -n "$PROC")
        if [ -z "$PID" ]; then
            echo "No process named '$PROC' found."
            exit 1
        fi
        lsof -p "$PID" 2>/dev/null | grep -E 'childenv|mimalloc|jemalloc|tcmalloc'
        ;;
    *)
        echo "Unknown allocator: $ALLOCATOR"
        echo "Usage: $0 [mimalloc|jemalloc|tcmalloc] <command> [args...]"
        echo "       $0 verify <process_name>"
        ;;
esac

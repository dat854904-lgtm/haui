#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Explicit opt-in; built/installed VTXFS logging stays DISABLED by default.
# Ftrace instance is isolated from the global kernel tracer.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CTL=/proc/dynamic_debug/control
TRACE=/sys/kernel/tracing
INSTANCE="$TRACE/instances/vtxfs_audit"

usage() {
  cat <<'EOF'
VTXFS kernel diagnostics. Must load the module in a controlled setup first.
  tools/trace-vtxfs.sh inventory
  sudo tools/trace-vtxfs.sh enable
  sudo tools/trace-vtxfs.sh enable-file super.c
  sudo tools/trace-vtxfs.sh enabled
  sudo tools/trace-vtxfs.sh follow              # Ctrl+C stops viewer
  sudo tools/trace-vtxfs.sh disable
  sudo tools/trace-vtxfs.sh ftrace-start function
  sudo tools/trace-vtxfs.sh ftrace-start function_graph
  sudo tools/trace-vtxfs.sh ftrace-read
  sudo tools/trace-vtxfs.sh ftrace-stop

Debug entry log: vtxfs-call enter <function>() pid=<tid>
Detail log:      vtxfs-event <function>(): <stage/parameters>
Ftrace:          *:mod:vtxfs includes available static helpers and core
                 functions; use real operation workloads to determine activity.
EOF
}
require_loaded() {
  [[ -d /sys/module/vtxfs ]] || {
    echo "VTXFS kernel module is not loaded. No trace settings changed." >&2
    exit 2
  }
}
require_root() {
  (( EUID == 0 )) || {
    echo "Root required: re-run with sudo on the authorized Arch Linux system." >&2
    exit 2
  }
}
set_debug() {
  local flag="$1" file="${2:-}" qualifier="module vtxfs "
  [[ -z "$file" ]] || qualifier+="file $file "
  printf '%s\n' "${qualifier}format \"vtxfs-call\" $flag" > "$CTL"
  printf '%s\n' "${qualifier}format \"vtxfs-event\" $flag" > "$CTL"
}
case "${1:-}" in
  inventory)
    cd "$ROOT"
    python3 tests/instrument_kernel_calls.py | tail -1
    python3 tests/audit_kernel_symbols.py
    ;;
  enable|disable)
    require_root; require_loaded
    [[ -w "$CTL" ]] || { echo "No permission to write $CTL" >&2; exit 2; }
    if [[ "$1" == enable ]]; then set_debug +p; else set_debug -p; fi
    echo "VTXFS dynamic debugging: $1"
    ;;
  enable-file)
    require_root; require_loaded
    file="${2:-}"
    [[ "$file" =~ ^[a-zA-Z0-9_-]+\.(c|h)$ && -f "$ROOT/kernel/$file" ]] || {
      echo "Specify kernel basename, e.g. super.c" >&2; exit 2;
    }
    set_debug +p "$file"
    echo "Enabled traces in kernel/$file only."
    ;;
  enabled)
    require_root; require_loaded
    grep 'vtxfs-\(call\|event\)' "$CTL" | grep '=p' || true
    ;;
  follow)
    require_root; require_loaded
    echo "Streaming kernel trace entries (Ctrl+C stops only the viewer)"
    dmesg --follow --human | grep --line-buffered -E 'vtxfs-(call|event)'
    ;;
  ftrace-start)
    require_root; require_loaded
    mode="${2:-function}"
    [[ "$mode" == function || "$mode" == function_graph ]] || {
      echo "Use 'function' or 'function_graph'" >&2; exit 2;
    }
    mountpoint -q "$TRACE" || {
      echo "tracefs not mounted at $TRACE; not touching global tracer." >&2
      exit 2
    }
    mkdir -p "$INSTANCE"
    echo 0 > "$INSTANCE/tracing_on"
    echo nop > "$INSTANCE/current_tracer"
    echo '*:mod:vtxfs' > "$INSTANCE/set_ftrace_filter"
    echo "$mode" > "$INSTANCE/current_tracer"
    echo > "$INSTANCE/trace"
    echo 1 > "$INSTANCE/tracing_on"
    echo "Started VTXFS ftrace in $INSTANCE, mode=$mode"
    ;;
  ftrace-read)
    require_root; require_loaded
    [[ -f "$INSTANCE/trace" ]] || { echo "Trace not created" >&2; exit 2; }
    cat "$INSTANCE/trace"
    ;;
  ftrace-stop)
    require_root
    [[ -d "$INSTANCE" ]] || { echo "No VTXFS tracing instance."; exit 0; }
    echo 0 > "$INSTANCE/tracing_on"
    echo nop > "$INSTANCE/current_tracer"
    echo "Stopped isolated VTXFS ftrace instance."
    ;;
  -h|--help|help|"") usage ;;
  *) usage; exit 2 ;;
esac

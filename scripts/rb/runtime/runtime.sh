#!/bin/sh
# RX3 modular runtime foundation. Not wired into start-rb.sh yet.
set -u
RX3_RUNTIME_ROOT=${RX3_RUNTIME_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)}
RX3_MODULE_DIR=${RX3_MODULE_DIR:-"$RX3_RUNTIME_ROOT/modules"}
RX3_RUNTIME_LOG=${RX3_RUNTIME_LOG:-/tmp/rx3-runtime.log}
LOADED_MODULES=""; CURRENT_MODULE=""; CURRENT_NAMESPACE=""
PREPARE_HOOKS=""; AFTER_LAUNCH_HOOKS=""; POST_LAUNCH_HOOKS=""; REPORT_HOOKS=""
MODULE_LOAD_FAILED=0
say() { printf '%s\n' "$*" | tee -a "$RX3_RUNTIME_LOG"; }
. "$RX3_RUNTIME_ROOT/lib/module-api.sh" || exit 1
load_modules() {
    [ -d "$RX3_MODULE_DIR" ] || { say "runtime: no module directory at $RX3_MODULE_DIR"; return 0; }
    for module in "$RX3_MODULE_DIR"/*/module.sh; do
        [ -f "$module" ] || continue
        CURRENT_MODULE=""; CURRENT_NAMESPACE=""
        . "$module" || MODULE_LOAD_FAILED=1
    done
    [ "$MODULE_LOAD_FAILED" -eq 0 ]
}
runtime_main() {
    : > "$RX3_RUNTIME_LOG" 2>/dev/null || true
    say "RX3 modular runtime foundation"
    load_modules || { say "runtime: module registration failed"; return 1; }
    say "runtime: loaded modules:${LOADED_MODULES:- none}"
}
[ "${RX3_RUNTIME_SOURCE_ONLY:-0}" = "1" ] || runtime_main "$@"

#!/bin/sh
# Minimal module registration API for the RX3 runtime foundation.
# No binary patching, preload changes or rbp restarts are performed here.

module_begin()
{
    id=$1
    namespace=$2
    case "$id" in ""|*[!a-z0-9-]*) say "runtime: invalid module id [$id]"; MODULE_LOAD_FAILED=1; return 1;; esac
    case "$namespace" in ""|*[!a-z0-9_]*) say "runtime: invalid namespace [$namespace] for $id"; MODULE_LOAD_FAILED=1; return 1;; esac
    case " $LOADED_MODULES " in *" $id "*) say "runtime: duplicate module [$id]"; MODULE_LOAD_FAILED=1; return 1;; esac
    LOADED_MODULES="$LOADED_MODULES $id"
    CURRENT_MODULE=$id
    CURRENT_NAMESPACE=$namespace
}

register_lifecycle_hook()
{
    phase=$1
    hook=$2
    [ -n "$CURRENT_MODULE" ] || { say "runtime: hook [$hook] registered outside a module"; MODULE_LOAD_FAILED=1; return 1; }
    case "$hook" in "${CURRENT_NAMESPACE}_"*) ;; *) say "runtime: hook [$hook] escapes namespace [$CURRENT_NAMESPACE]"; MODULE_LOAD_FAILED=1; return 1;; esac
    case "$phase" in
        prepare) PREPARE_HOOKS="$PREPARE_HOOKS $hook";;
        after) AFTER_LAUNCH_HOOKS="$AFTER_LAUNCH_HOOKS $hook";;
        post) POST_LAUNCH_HOOKS="$POST_LAUNCH_HOOKS $hook";;
        report) REPORT_HOOKS="$REPORT_HOOKS $hook";;
        *) say "runtime: unknown lifecycle phase [$phase]"; MODULE_LOAD_FAILED=1; return 1;;
    esac
}

register_prepare_hook() { register_lifecycle_hook prepare "$1"; }
register_after_launch_hook() { register_lifecycle_hook after "$1"; }
register_post_launch_hook() { register_lifecycle_hook post "$1"; }
register_report_hook() { register_lifecycle_hook report "$1"; }

run_hooks()
{
    hooks=$1
    failed=0
    for hook in $hooks; do "$hook" || failed=1; done
    return "$failed"
}

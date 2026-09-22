#!/bin/sh
module_begin core core
core_report() { say "core: modular runtime registration active"; }
register_report_hook core_report

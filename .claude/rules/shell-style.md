# Shell and C style
- `#!/usr/bin/env bash`, `set -euo pipefail`, functions under ~50 lines, no hardcoded absolute user paths — take them from variables at the top.
- Scripts must be idempotent and print what they are about to do before destructive steps.
- Pass `bash -n` and `shellcheck -S warning` (the PostToolUse hook runs these).
- C shims (`audioshim.c`, `keyshim`, ...): soft-float ARM32 target; check return values; no unbounded buffers.

---
name: verification-loop
description: Verify work before commit/PR: run .claude/scripts/verify.sh, review the diff, then report PASS/FAIL. Use after finishing a change or before pushing.
---
Adapted from ECC `verification-loop` (build/type/lint/test phases replaced by what this repo has).
1. Run `.claude/scripts/verify.sh`. On FAIL stop, fix, re-run (loop until PASS). Treat WARN as items to mention.
2. `git diff --stat` then read the diff: unquoted vars, missing `set -euo pipefail`, destructive steps without a printed target/confirmation, hardcoded paths, docs not updated.
3. Flash/erase/deploy script touched => run `security-reviewer`; any script/C change => `code-reviewer`.
4. Hardware behavior cannot be verified here: say explicitly what was NOT tested on the device.
5. Report:
```
VERIFICATION REPORT
Script: PASS/FAIL (n warnings)   Diff review: PASS/FAIL   Reviewers: run/skipped
Not verified on device: ...
Ready to commit: YES/NO
```

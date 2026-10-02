---
name: eval-harness
description: Eval-driven development for risky or flaky work (e.g. the start-up pop, keyshim crash). Define pass/fail criteria first, grade with code/model/human graders, track pass@k and pass^k.
---
Adapted from ECC `eval-harness`.
**Before coding**, write in the session note (`docs/sessions/`):
```
[CAPABILITY EVAL: name]  Task / Success criteria (checkboxes) / Expected output
[REGRESSION EVAL: name]  Baseline (commit or doc/16 hashes) / Checks that must stay PASS
```
**Graders**: code-based (exit codes, `verify.sh`, log greps, md5 vs docs/16 §1) > model-based (diff quality) > human (listening for the pop, display check — only the user can grade these; ask).
**Reliability**: run the check k times on the device. pass@k = at least one success (k=1 70%, k=3 91%); pass^k = all k succeed (k=3 34%) — use pass^k for fixes that must be consistent (pop, crash on restart).
**Report**: `EVAL REPORT: name` with each trial result, regressions, and Status READY / BLOCKED. Safety-relevant changes stay BLOCKED until the user approves.

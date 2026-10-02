---
name: session-handoff
description: Save session state to docs/sessions/ before compacting or ending, so the next session resumes cleanly.
---
Create `docs/sessions/YYYY-MM-DD-<topic>.md` (new file per session) with:
1. **Goal** and current state.
2. **Worked** — approaches that succeeded, with evidence (commands, hashes, log lines).
3. **Failed** — attempts that did not work and why.
4. **Left** — untried ideas and next steps.
Never include secrets. Then tell the user the path to pass to the next session.

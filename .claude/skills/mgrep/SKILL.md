---
name: mgrep
description: Semantic code/doc search with mgrep (Mixedbread) when ripgrep keywords are not enough. Only use after the user has installed and logged in to mgrep.
---
`mgrep "<natural-language query>"` searches the indexed repo; `mgrep --web "<query>"` searches the web. Check `command -v mgrep` first; if missing, tell the user to follow docs/17-claude-workflow.md and fall back to Grep.
Privacy: mgrep syncs repo files to a cloud store. Respect `.mgrepignore`; never index firmware, keys or `work/`/`images/`. Use plain Grep/Glob for exact identifiers.

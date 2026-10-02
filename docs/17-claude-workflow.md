# 17 — Claude Code workflow (adapted from affaan-m/ECC)

Source: https://github.com/affaan-m/ECC (shortform, longform and security guides). ECC ships ~70 agents and hundreds of skills; here only what fits a docs + shell repo is applied, following its own advice ("don't overcomplicate", "context is precious").

## Applied in this repo
| ECC tip | Where |
|---|---|
| Project CLAUDE.md + modular rules | `CLAUDE.md`, `.claude/rules/` |
| Hooks (PreToolUse/PostToolUse/SessionStart/PreCompact) | `.claude/settings.json`, `.claude/hooks/` |
| Block secrets/firmware, no `git add -A` | `guard-secrets.sh` |
| Auto syntax-check/shellcheck after edits | `check-shell.sh` |
| Keep stray `.md` out of the tree | `warn-stray-md.sh` |
| Session memory persistence (SessionStart + PreCompact + handoff) | `session-context.sh`, `precompact.sh`, skill `session-handoff`, `docs/sessions/` |
| Scoped subagents with model selection (Haiku/Sonnet/Opus) | `.claude/agents/`, `rules/agents.md` |
| Security baseline (least agency, untrusted input, supply chain) | `rules/security.md` |

## Habits to use (no files needed)
- `/fork` for side questions, `git worktree add ../rbtv-x branch` for overlapping parallel work; `/rename` sessions; keep 3–4 tasks max.
- `/compact` manually at logical points; `/rewind` and checkpoints for undo; `/statusline` for context %.
- Few MCPs enabled (<10); prefer CLI + skills over always-on MCPs.
- pass@k vs pass^k: use repeated runs when a fix must be consistent (e.g. the start-up pop).
- Two-instance kickoff for new areas: one scaffolds, one researches (PRD/docs).

## Not applied (add on demand)
Language reviewers, TDD/e2e agents, continuous-learning skill, mgrep, LSP/Next.js/Supabase MCPs — irrelevant here. Install from the ECC repo only after reviewing them as supply-chain code.

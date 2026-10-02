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

## continuous-learning and mgrep (added)
- **continuous-learning**: ECC marks v1 deprecated in favor of v2 (background observer on every tool call, writes under `~/.claude`). v2 is heavy and off-repo, so a lean v1-style flow is used instead: `Stop` hook `learn-reminder.sh` nudges after 10+ user messages, `/continuous-learning` writes reviewable skills to `.claude/skills/learned/`. Revisit v2 if the volume of learnings justifies it.
- **mgrep** (https://github.com/mixedbread-ai/mgrep, third-party, not part of ECC): it is a local install, not a repo file. Tip: ECC reports ~2x fewer tokens vs grep. It **uploads repo files to Mixedbread's cloud store**, so it is opt-in: `.mgrepignore` excludes firmware, keys and images, and `.claude/skills/mgrep` only activates if you install it:
  ```bash
  npm install -g @mixedbread/mgrep   # or: claude plugin marketplace add https://github.com/mixedbread-ai/mgrep
  mgrep login
  mgrep watch --dry-run              # check what would be uploaded first
  ```

## Verification loops and evals (added)
- `.claude/scripts/verify.sh` — deterministic grader: `bash -n`/shellcheck on all scripts, C syntax check, no firmware/keys tracked, hooks valid and executable, markdown links resolve. Cross-compile-only C files give WARN, not FAIL.
- `/verification-loop` — runs verify.sh, reviews the diff, calls the reviewers, and states what was *not* verified on the device.
- `/eval-harness` — define capability/regression evals first; use pass^k for must-be-consistent fixes (start-up pop, keyshim crash).
- `harness-optimizer` agent — audits only `.claude/**` and `CLAUDE.md`; security-relevant changes stay BLOCKED until you approve. ECC's version calls `node scripts/harness-audit.js`, which does not exist here, so it uses verify.sh instead.

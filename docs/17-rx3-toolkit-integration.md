# 17 — RX3 Toolkit integration baseline

This document marks the start of the integration work inspired by `Tratosca/rx3-toolkit`.

## Protected baseline

The currently functional project state is preserved at:

- branch: `backup/functional-2026-09-22`
- commit: `4ce92254df52c8a3a5ac62801555503be68e9782`

All integration work must happen on:

- branch: `feature/rx3-toolkit-runtime`

## Integration principles

1. Do not modify the protected functional branch.
2. Keep each functional change in a separate commit whenever practical.
3. Validate the exact `rbp` binary before applying binary patches.
4. Never reuse firmware offsets blindly; verify opcode/bytes against the active binary.
5. Prefer modular runtime components for new functionality.
6. Preserve rollback paths for runtime changes.
7. Keep DDJ-400, display, audio and compatibility layers independently testable.

## Planned phases

### Phase 1 — Runtime foundation
- module API
- patch registry
- guarded patch validation
- rbp hash/version validation
- preload management
- diagnostics
- rollback / recovery hooks

### Phase 2 — Beat Jump
- ±32 Beat Jump
- direct/immediate Beat Jump path

### Phase 3 — Performance core
- shared runtime core
- display hooks
- touch/UI hooks
- asset management

### Phase 4 — Key Shift
- per-deck ±12 semitone control
- on-screen KEY controls
- DDJ-400 mappings

### Phase 5 — Stems
- pre-generated vocal/instrumental sidecars
- runtime stem loader
- audio routing
- pad feedback
- on-screen STEMS controls

## Commit strategy

Each significant step should be committed independently so regressions can be isolated and reverted without disturbing the known-good baseline.

Suggested prefixes:

- `runtime:`
- `patch:`
- `ddj400:`
- `audio:`
- `display:`
- `stems:`
- `keyshift:`
- `docs:`

## License note

The upstream `Tratosca/rx3-toolkit` project is MPL-2.0. Any MPL-covered source files adapted directly from that project must retain the applicable MPL notices and obligations.

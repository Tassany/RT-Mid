# CLAUDE.md

See [RULES.md](RULES.md) for the full development process this project follows.

Claude/AI must never add attribution or co-authorship lines (e.g.
"Co-Authored-By: Claude", "🤖 Generated with Claude Code") to git commit
messages or pull request descriptions in this repository. All git history
and messages here are authored entirely by the human maintainer — see
RULES.md §1, which already bars an assistant from drafting or suggesting
commit message text at all.

## Development Methodology — Spec-Driven Development (SDD)

Feature work that qualifies as "elaborate" under RULES.md §9 follows the
spec → plan → tasks → execute pipeline in `specs/`. `specs/memory/constitution.md`
defers to RULES.md rather than duplicating it — RULES.md is the authority.

### Build & Test Commands
`make test` builds and runs every `tests/*.cpp` binary (stops at the first
failure but reports which one). `make codegen` regenerates
`generated/pipeline_generated.{hpp,cpp}` from `plans/deployment_plan.json`
— required before `codegen_pipeline_test` can build; `make test` handles
this automatically via a Makefile prerequisite. `make clean` removes
`build/`. Each `tests/*.cpp` file also documents its own standalone `g++`
line in a header comment, for building that one binary in isolation.

### Before implementing any feature
1. Read `specs/[feature]/spec.md`, `plan.md`, `tasks.md`.
2. Read `specs/memory/constitution.md` (→ RULES.md).

### While implementing
- Task list order is strict; mark `[~]`/`[x]` as you go.
- Test-first per RULES.md §3.
- `[C]` checkpoints: build + run the affected test binaries, audit task
  marks, check against spec acceptance criteria, record in `progress.md`.
- Every edit explained before/alongside it (RULES.md §1) — the author
  applies edits and all git actions themselves.

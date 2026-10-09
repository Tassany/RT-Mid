# Development Constitution

This project's actual development rules live in [RULES.md](../../RULES.md) —
written because most of RT-Mid's first implementation was unsupervised AI
code generation the author couldn't defend. SDD's generic default
constitution is not used here; RULES.md is the authority for all SDD
phases, and takes precedence where the two would otherwise conflict.

Relevant to SDD phases:

- **§1 (Git and merge protocol):** diffs target ~150 lines; an assistant
  proposes commits, never executes `git add`/`commit`/`push` without
  explicit per-action approval; every edit is explained before/alongside
  it, never as a silent diff.
- **§2 (Rewrite vs. document):** rewrite only what has no nameable
  invariant; document what does. Bar is stricter for core-contribution
  code (allocator, `Dispatcher`) than supporting mechanism.
- **§3 (Tests):** every non-trivial function touched gets at least one
  automated test — unit-level where possible.
- **§9 (Elaborate work):** work spanning more than one architectural layer
  goes through grill-with-docs before it becomes a spec. This feature
  already completed that step — see
  [docs/adr/0001-keep-component-layer-narrow-registry-scope.md](../../docs/adr/0001-keep-component-layer-narrow-registry-scope.md).
- **§10 (Doxygen comments):** rewritten alongside the code they describe,
  capped at 120 words.

No premature abstraction, no speculative generality (see ADR-0001 for what
that cost here), no code without a nameable reason to exist.

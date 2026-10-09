# RRC WCRT Analysis

## Overview

Implements the worst-case response time (WCRT) analysis — "the RRC
algorithm" — Section IV-A, Eq. 1–4, of Wu, Zhang, Guan & Ma, "TDTA:
Topology-Based Real-Time DAG Task Allocation on Identical Multiprocessor
Platforms" (IEEE TPDS 34(11), 2023). This is the piece every allocator
spec written so far (`specs/wf-dru-allocator/`, `specs/eru-allocator/`,
`specs/tdta-allocator/`) explicitly deferred as a Non-Goal: those features
only *place* subtasks; this one *computes* `R(τᵢ)`, the quantity that
actually proves (or disproves) a claim like "TDTA improves schedulability
over WF+DRU" — up to now this codebase could only show that claim by
*measuring* response time on real hardware (`tools/eval/interference_eval_main.cpp`),
never by *calculating* the paper's own theoretical bound.

**This feature is allocation-strategy-agnostic by design.** It consumes a
`DeploymentPlan` whose subtasks already have `core` assigned — by
`"worst_fit"`, `"eru"`, `"tdta"`, or even hand-authored in a JSON plan —
and computes `R(τᵢ)` for whichever tasks are present. It does not call
`allocator::apply_auto_allocation` itself and does not care which
strategy produced the allocation it's given.

**Paper gap, disclosed up front (RULES.md §4 — faithfulness means naming
what's NOT specified, not papering over it):** Eq. 2–3 define `R(λᵢ,ₖ)`
as a value bounded by an expression that itself contains `R(λᵢ,ₖ)` (inside
`⌈R(λᵢ,ₖ)/Tⱼ⌉`) — a fixed-point recurrence. This paper does not give the
iterative procedure to solve it; its own text says so directly: "This
article is inspired by the RRC algorithm and draws on some necessary
formulations from this algorithm. Hence, a short introduction to the RRC
algorithm is given, and more details are provided in the original
article" (footnote, Sect. IV, citing a different, uncited-in-full-text-here
reference [11] this project doesn't have a copy of). This implementation
fills that gap with the standard real-time-systems fixed-point iteration
for response-time analysis (Audsley/Joseph-Pandya-style: start from the
interference-free lower bound, repeatedly recompute the right-hand side
using the previous iterate, stop at a fixed point or once the bound
exceeds the deadline) — the conventional, textbook way to solve exactly
this shape of recurrence, not a novel technique, but also not something
*this* paper's text states explicitly. Flagged as a Key Decision in
`plan.md` rather than silently assumed.

## User Stories

- As the author, I want `R(τᵢ)` computed from the paper's own Eq. 1–4, so
  a claim like "TDTA reduces the low-priority task's WCRT vs WF+DRU" can
  be checked against the paper's own theoretical bound, not only against
  one noisy real-hardware measurement (`interference_eval.png`).
- As the author, I want this analysis decoupled from any one allocator,
  so it can compare WF+DRU vs ERU vs TDTA's allocations of the *same*
  plan by just computing `R(τᵢ)` three times against three differently-
  allocated copies of it.

## Acceptance Criteria

- [ ] **Complete path enumeration** (`λᵢ`, Sect. III): given one DAG
      task's (IED-reduced) subtasks/connections, enumerate every directed
      source-to-sink path. Each path's length `L(λᵢ,ₖ) = Σ C_i,j` over
      its subtasks.
- [ ] **Self-interference** (Definition 3): for subtask `Vᵢ,ⱼ`,
      `self(Vᵢ,ⱼ)` is every other subtask `Vᵢ,b` of the *same* task `τᵢ`
      such that (1) neither constrains the other (not reachable from one
      to the other, either direction, on the IED-reduced graph) and (2)
      `Pᵢ,b = Pᵢ,ⱼ` (same processor). `self(λᵢ,ₖ) = ⋃` over the path's own
      subtasks.
- [ ] **`Ω` (Eq. 4)**: for a higher-priority task `τⱼ`'s Str structure
      `ξⱼ,ᵣ` and a path `λᵢ,ₖ`: `η = ρ(ξⱼ,ᵣ) ∩ ρ(λᵢ,ₖ)` (processors the
      structure and the path both use); if `η ≠ ∅`,
      `Ω = (|η|-1) · min` over `η`'s processors of that structure's
      cumulative WCET on that one processor; `Ω = 0` (term excluded) when
      `η = ∅`.
- [ ] **`I^h_j(λᵢ,ₖ)` (Eq. 3)** and **`R(λᵢ,ₖ)` (Eq. 2)**: the fixed-point
      iteration described in `plan.md`'s Key Decisions, over every
      higher-priority task `τⱼ ∈ hp(τᵢ)` (`TaskInfo::priority` ascending
      comparison, smaller = higher priority — same convention
      `specs/tdta-allocator/` already established).
- [ ] **`R(τᵢ)` (Eq. 1)**: max over all of `τᵢ`'s complete paths.
- [ ] **Schedulability check**: `R(τᵢ) ≤ Dᵢ` for every task in the plan;
      a plan-level helper reports which tasks (if any) are unschedulable.
- [ ] Verified against a from-scratch, hand-traced 2-task worked example
      (two subtasks levels deep, one non-trivial `Ω`) — see `plan.md` —
      not only against the already-verified `tdta-allocator` worked
      example, since that one alone never exercises `Ω`/`I^h_j`/`R` at
      all (it has no second task).
- [ ] Reuses `ied::remove_invalid_edges` and
      `tdta::detail::find_str_structures` as-is — no changes to either.

## Non-Goals

- Calling any allocator itself — this feature only reads `core` fields
  already set on the plan it's given.
- A new eval/comparison tool that computes `R(τ_LOW)` for
  `interference_eval_main.cpp`'s own topology under each strategy and
  plots/prints it alongside the measured numbers. Natural follow-up once
  this feature's own correctness is established, not part of it.
- `guided`/`validate` (`AllocationConfig`'s `rta`/`rta_v` fields,
  `src/rta_fonseca2016.hpp`): that's a reference to a *different* paper's
  RTA technique (Fonseca et al. 2016) the codebase already declared as an
  unimplemented hook; unrelated to this feature, not touched.
- System jitter: Eq. 3's own text says "we ignore the system jitter" —
  this implementation does too, matching the paper exactly.
- Multi-source/multi-sink normalization — same existing assumption every
  other feature in this codebase carries (one source, one sink per task).

## Open Questions

Resolved during grilling: the fixed-point iteration gets an explicit
iteration cap (generous constant; throws `std::runtime_error` if
exceeded) rather than relying solely on the mathematical termination
argument — a cheap defensive guard against a malformed plan or an
upstream bug turning into a silent infinite loop instead of a clear
error. See `plan.md`'s Key Decisions.

## Dependencies

- `specs/tdta-allocator/` (`ied::remove_invalid_edges`,
  `tdta::detail::find_str_structures`, `TaskInfo::priority`) — reused
  directly, not reimplemented.
- Section III (system model: `λᵢ`, `L(λᵢ,ₖ)`, `Cᵢ,ⱼ`, `Pᵢ,ⱼ`, `ρ`),
  Definition 3 (self-interference), and Section IV-A (Eq. 1–4, RRC) of
  `TDTA_Topology-Based_Real-Time_DAG_Task_Allocation_on_Identical_Multiprocessor_Platforms.pdf`,
  p. 2898.

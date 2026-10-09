# ERU Core Allocator

## Overview

Adds a second automatic subtask-to-core allocation heuristic alongside the
existing WF+DRU (`specs/wf-dru-allocator/`): **Equilibrium Remaining
Utilization (ERU)**, Algorithm 2 of Wu, Zhang, Guan & Ma, "TDTA:
Topology-Based Real-Time DAG Task Allocation on Identical Multiprocessor
Platforms" (IEEE TPDS, vol. 34, no. 11, Nov. 2023), Sect. VI.

**Paper errata, noted for the record:** the pseudocode box on p. 2901 is
captioned "Algorithm 2: The IED Method" — a leftover copy-paste of
Algorithm 1's own caption. The surrounding body text ("Algorithm 2 shows
the specific steps of the ERU algorithm") and the function signature
itself (`function ERU(Ū, V̄, C̄, Tᵢ)`) make the actual identity
unambiguous. This implementation is of the ERU function, not IED (already
implemented separately, if at all — IED is unrelated invalid-edge-deletion
graph simplification, not an allocator).

**Explicit scope, resolved before this spec** (see `[NEEDS CLARIFICATION]`
below for what's still open): this implements **ERU alone (Algorithm 2)**
as its own standalone `plan.allocation.strategy`, run once over the whole
plan's unassigned subtasks as a single group — analogous in shape to how
WF+DRU is exposed today. It does **not** implement the paper's full TDTA
strategy (Algorithm 3), which calls ERU repeatedly per level/`Str`-group
after first partitioning subtasks via the IED method and level/`Str`
detection (Sects. IV–V) — that machinery does not exist in this codebase
yet and is a separate, much larger feature. Naming it accurately (ERU, not
TDTA) is itself part of RULES.md §4 faithfulness: this is a real, useful,
paper-grounded load-balancing heuristic in its own right, but it is not the
paper's namesake allocation strategy, and no comment, docstring, or commit
message in this feature should imply otherwise.

The paper's own prose motivation (Sect. VI, before Algorithm 2): "We
propose an equilibrium remaining utilization (ERU) algorithm, which allows
for a more load-balanced allocation of the specific subtasks to
processors." Its distinguishing rule vs. Worst-Fit: within one ERU call, at
most `μ = min(m, |subtasks to place|)` distinct processors ever receive
work — the `μ` processors with the globally largest real remaining
utilization are pre-selected once, then subtasks are placed largest-WCET
first onto whichever of those `μ` processors currently has the most
*virtual* remaining utilization, where all `μ` virtual slots restart at
1.0 for the call regardless of the processors' real remaining utilization.
This spreads one group's workload evenly across exactly `μ` processors,
decoupled from pre-existing load skew between them — a deliberately
different tradeoff from Worst-Fit's always-pick-the-single-most-free-core
rule.

## User Stories

- As the author defending this codebase for the paper, I want a second
  allocator that implements exactly the ERU rule as described in Sect. VI
  of the TDTA paper, so its placement decisions are traceable to that
  algorithm's pseudocode line-by-line, not to an AI's guess at what
  "load-balanced" should mean.
- As the author, I want ERU exposed the same way WF+DRU is
  (`plan.allocation.strategy`), so existing tooling
  (`tools/eval/allocator_comparison_eval_main.cpp`-style harnesses) can
  compare the two allocators on the same plan without bespoke plumbing.

## Acceptance Criteria

- [ ] A new `eru::equilibrium_remaining_utilization_place` (name TBD in
      plan.md) function implements Algorithm 2 faithfully:
      1. **Inputs**: the plan's per-core remaining utilization `Ū` (one
         value per core, same starting-capacity convention as WF+DRU:
         `plan.allocation.capacity` per core minus already-assigned
         subtasks' utilization), and the group of unassigned subtasks to
         place, `V̄` (for this feature: *all* `CORE_UNASSIGNED` subtasks in
         the plan, in one call).
      2. **μ selection**: `μ = min(num_cores, |V̄|)`; the `μ` cores with the
         largest current `Ū` are selected once, ranked descending (ties:
         lowest core index) into `θ = [θ_1..θ_μ]`.
      3. **Virtual scratch state**: `Û = [Û^1..Û^μ]`, all initialized to
         `1.0` (or `plan.allocation.capacity` if nonzero — same generalization
         WF+DRU already makes for "1.0"), one per selected core, reset for
         this call only.
      4. **Placement loop**: while unplaced subtasks remain, pick the
         unplaced subtask with the largest WCET (ties: original plan
         order — stable), place it on whichever selected core currently has
         the largest `Û` (ties: lowest core index, i.e. earliest in θ),
         then subtract that subtask's utilization from *both* that core's
         `Û` slot and its real `Ū` (the real `Ū` is what persists/returns).
      5. **Output**: every subtask in the input group gets `core` assigned
         to one of the `μ` selected cores; the plan's per-core remaining
         utilization reflects every placement.
- [ ] Per-subtask utilization/WCET-to-utilization conversion matches
      WF+DRU's existing generalization: `wcet_us / period_us` per subtask
      (the paper's single shared `C_i,α/T_i` term, generalized the same way
      `allocator::detail::utilization` already generalizes it for
      multi-task plans — no single plan-wide `T_i` exists in this
      codebase's `DeploymentPlan`).
- [ ] Exposed as `plan.allocation.strategy == "eru"`, resolvable through
      the same public entry point WF+DRU uses today
      (`allocator::apply_auto_allocation`), which becomes a dispatcher
      between the two strategies (see plan.md for the split into
      `dru.hpp`/`dru.cpp`, `eru.hpp`/`eru.cpp`, `allocator.hpp`/
      `allocator.cpp`).
- [ ] `num_cores`/per-core capacity resolution identical to WF+DRU
      (`plan.allocation.num_cores`, default `hardware_concurrency()`;
      `plan.allocation.capacity`, default `1.0`).
- [ ] Throws `std::runtime_error` (matching the existing convention) if:
      - a pre-assigned subtask names a core outside `[0, num_cores)`.
      - placing a subtask would push a core's `Û`/`Ū` negative beyond
        floating-point epsilon (mirrors WF+DRU's infeasibility check —
        ERU's own pseudocode has no such guard, but leaving it unchecked
        would silently overcommit a core, which this codebase's existing
        convention treats as an error, not a valid outcome).
- [ ] `allocator::apply_auto_allocation` still rejects any
      `plan.allocation` combination that is neither the existing WF+DRU
      triple nor `strategy == "eru"`.
- [ ] Existing WF+DRU behavior, tests, and public call sites
      (`tools/eval/allocator_comparison_eval_main.cpp`,
      `tests/allocator_test.cpp`) are unaffected by the file split — same
      inputs produce the same outputs.
- [ ] Unit tests for ERU mirror `tests/allocator_test.cpp`'s structure and
      rigor (μ-selection, virtual-vs-real Û/Ū distinction, tie-breaking,
      infeasibility, an end-to-end plan check) — see plan.md/tasks.md.

## Non-Goals

- The full TDTA strategy (Algorithm 3): level division, `Str`-structure
  detection, and per-group ERU calls driven by DAG topology (Sects. IV–V
  of the paper). Explicitly deferred to a future, separate feature once/if
  IED and level/`Str` detection exist in this codebase.
- Changing `AllocationConfig`'s declared-but-unimplemented `sort_by`/
  `weight` enum semantics beyond documenting `"eru"` as a recognized
  `strategy` value — ERU's subtask/core selection rules are intrinsic to
  the algorithm, not configurable via `sort_by`/`weight`, the same way
  WF+DRU's are effectively fixed today despite the broader declared enum.
- A new allocator-comparison eval tool/plot for ERU vs. WF+DRU — reusing
  `tools/eval/allocator_comparison_eval_main.cpp`'s pattern for that is a
  natural follow-up, not part of this feature.
- Any change to `Dispatcher`/`TeamManager`/scheduling/response-time
  analysis — this is allocation-time placement only, same boundary
  WF+DRU already respects.

## Open Questions

- [NEEDS CLARIFICATION: infeasibility guard] The paper's Algorithm 2 has
  no explicit "does it fit" check — it assumes `V̄`'s total utilization
  already fits within the `μ` selected cores' capacity (guaranteed
  upstream by TDTA's own construction, which this codebase doesn't have).
  Confirm the proposed behavior above (throw on overcommit, mirroring
  WF+DRU) is correct rather than, say, silently letting a core's
  utilization exceed capacity.

## Dependencies

- `src/allocator.hpp`/`src/deployment_plan.hpp`/`src/dag.hpp` (existing
  WF+DRU code and shared plan/DAG types — see
  `specs/wf-dru-allocator/spec.md`).
- Section VI, "TDTA: Topology-Based Real-Time DAG Task Allocation on
  Identical Multiprocessor Platforms" (Wu et al., IEEE TPDS 34(11), 2023) —
  `TDTA_Topology-Based_Real-Time_DAG_Task_Allocation_on_Identical_Multiprocessor_Platforms.pdf`,
  p. 2901 (Algorithm 2 pseudocode) and p. 2900–2901 (body text motivating
  it).

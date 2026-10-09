# WF+DRU Core Allocator

## Overview

Replaces the placeholder in `src/allocator.hpp` (everything to core 0) with
the real automatic subtask-to-core allocation heuristic the paper is built
around: **Worst-Fit with Decreasing Remaining Utilisation (WF+DRU)**, as
introduced in Verucchi, Sañudo Olmedo & Bertogna, "A survey on real-time DAG
scheduling, revisiting the Global-Partitioned Infinity War" (Real-Time
Systems, 2023), Sect. 4 intro + Sect. 7.11/8.2.

The paper describes the algorithm only in prose (no pseudocode):

> "Decreasing Remaining Utilization (DRU) orders the nodes by the total
> utilization of their successors [...] the best heuristic to partition
> nodes to cores is using the Worst-Fit strategy, having ordered the nodes
> by their Decreasing Remaining Utilisation value."

Resolved during grilling (see Key Decisions in `plan.md`):
"successors" means the DAG's immediate/direct successors (`succᵢ` in the
paper's own Table 2 notation, distinct from `Descᵢ`, the full descendant
set) — the same relation `DAG::Node::successors` already stores.

This is the paper's main scientific contribution; RULES.md §4 requires the
implementation to be provably faithful to this model, checked against a
precise restatement of it (this document + `plan.md`), not inferred from
generated code.

## User Stories

- As the author defending this codebase for the paper, I want the
  allocator to implement exactly the WF+DRU rule as described in the
  survey, so every placement decision it makes is traceable to a specific
  sentence in the paper, not to an AI's guess at what "seemed reasonable."
- As the author, I want a deployment plan that omits some subtasks' `core`
  field to come out schedulable-by-construction (no core over-committed
  past utilization 1.0), so the allocator's output can be trusted as an
  input to the rest of the pipeline (`TeamManager`, `Dispatcher`) without
  a second manual check.

## Acceptance Criteria

- [x] `allocator::apply_auto_allocation(DeploymentPlan&)` assigns a core to
      every subtask with `core == CORE_UNASSIGNED`, using:
      1. **Utilization** `U(subtask) = wcet_us / period_us` for every
         subtask in the plan (assigned or not).
      2. **Remaining utilization** of a subtask = the sum of `U(s)` over
         `s` in that subtask's *direct* successors in the whole-plan DAG
         (all tasks' subtasks as one node set, `plan.connections` as
         edges — matching how `JsonParser::validate_structure` already
         builds its DAG).
      3. **DRU ordering**: the `CORE_UNASSIGNED` subtasks, sorted by
         descending remaining utilization (ties broken by original plan
         order — stable sort).
      4. **Worst-Fit placement**: cores numbered `0..num_cores-1`, each
         starting at capacity 1.0 (partitioned-FTP utilization bound) minus
         the utilization of every subtask *already* assigned to that core
         in the plan. Walk the DRU-ordered list; for each subtask, place it
         on whichever core currently has the most remaining capacity
         (ties broken by lowest core index), subtracting its utilization
         from that core afterward.
- [x] `num_cores` resolution: `plan.allocation.num_cores` if nonzero, else
      the number of hardware threads available
      (`std::thread::hardware_concurrency()`).
- [x] Per-core capacity resolution: `plan.allocation.capacity` if nonzero,
      else `1.0`.
- [x] `apply_auto_allocation` throws `std::runtime_error` (consistent with
      `JsonParser`'s existing error convention) if:
      - `plan.allocation.strategy` is set to anything other than
        `"worst_fit"`, or `sort_by` to anything other than
        `"remaining_utilization_desc"`, or `weight` to anything other than
        `"utilization"` — this change implements exactly one combination,
        not the general framework those fields imply (see Non-Goals).
      - No core has enough remaining capacity for some subtask being
        placed (an infeasible plan is reported, not silently overcommitted
        or left unassigned).
- [x] `AllocationConfig`'s defaults (`src/deployment_plan.hpp`) are
      `sort_by = "remaining_utilization_desc"` and `weight = "utilization"`
      (strategy's default, `"worst_fit"`, is already correct) — so a plan
      that omits the `allocation` block, or omits these fields within it,
      gets real WF+DRU rather than an immediate rejection.
- [x] `plans/deployment_plan.json`'s `"allocation"` block is updated from
      `{"strategy": "worst_fit", "sort_by": "none"}` to declare the actual
      implemented combination explicitly (belt-and-suspenders with the new
      defaults above).
- [x] Every non-trivial function has at least one automated test
      (RULES.md §3): utilization computation, remaining-utilization/DRU
      ordering, and Worst-Fit placement are each independently testable
      (see `plan.md` Implementation Phases for the two-commit split this
      implies).
- [x] `make clean && make test` — `tests/allocator_test.cpp` (new, 19/19
      assertions) and `tests/test_flux.cpp` pass. `tests/performance_test.cpp`
      fails, but for a pre-existing, unrelated environmental reason (no
      PREEMPT_RT kernel/`sudo` in this sandbox — confirmed identical to the
      baseline failure before this change, not a regression it introduces).

## Non-Goals

- Implementing `first_fit`/`best_fit` strategies or the other `sort_by`
  orderings (`priority_desc`, `period_asc`, `utilization_desc`, etc.) or
  `weight = "count"` — these exist in `AllocationConfig` because the
  survey's own Sect. 7 experiments compare against them as baselines, not
  because this codebase needs the general comparison framework. Rejected
  with a clear error if requested (see Acceptance Criteria).
- `AllocationConfig::guided`/`::validate` (`rta`/`rta_v` modes,
  `rta_fonseca2016.hpp`, `bench_registry.hpp`) — not part of WF+DRU as
  described in this paper, not referenced or validated by this change at
  all (confirmed with the author: no rejection-validation code for these
  either, just leave them unconsulted).
- [allocator-correctness-checker.md](../../docs/backlog/allocator-correctness-checker.md)
  — an independent oracle re-implementation to diff against; explicitly
  follow-on work, unblocked by this change but not part of it.
- Any change to `Dispatcher`/`TeamManager` — they already consume
  `SubtaskInfo::core` however it was set; this change only affects how
  that field gets filled in.
- Any paper text edit.

## Dependencies

- [docs/backlog/implement-wf-dru-allocator.md](../../docs/backlog/implement-wf-dru-allocator.md)
  — the backlog item this spec executes.
- `src/dag.hpp`/`src/dag.cpp` — reused as-is for the whole-plan DAG
  (`DAG::Node::successors` already gives direct successors).
- `src/deployment_plan.hpp` (`AllocationConfig`, `SubtaskInfo`,
  `CORE_UNASSIGNED`) — defaults change, no field additions/removals.

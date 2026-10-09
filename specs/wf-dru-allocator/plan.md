# WF+DRU Core Allocator — Technical Plan

## Technical Approach

Two independent, unit-testable functions in `namespace allocator::detail`,
composed by the public `allocator::apply_auto_allocation` entry point
(same signature as the current stub, so `parser_json.cpp` needs no
changes):

1. **DRU ordering** — pure function over the plan's utilization data and
   its whole-plan DAG. No mutation, no core knowledge. Independently
   testable: given subtasks + connections, does it produce the right
   remaining-utilization values and the right descending order?
2. **Worst-Fit placement** — consumes that ordering, mutates
   `SubtaskInfo::core` in place. Independently testable: given an
   ordering + initial per-core occupancy (including pre-assigned
   subtasks), does it place greedily-by-max-remaining-capacity and throw
   on infeasibility?

Both build the whole-plan `DAG` the same way `JsonParser::validate_structure`
already does (all tasks' subtasks as nodes, `plan.connections` as edges) —
duplicating that ~6-line loop rather than adding a shared helper, since
`validate_structure`'s DAG is function-local and this is the only other
call site; a shared abstraction would be premature for two call sites of a
six-line loop (RULES.md's "no premature abstraction" per the constitution).

## Key Decisions

| Decision | Choice | Rationale | Needs sign-off? |
|---|---|---|---|
| "Successors" in DRU | Direct/immediate successors (`DAG::Node::successors`), not the full descendant closure | Paper's own Table 2 notation distinguishes `succᵢ` (immediate) from `Descᵢ` (all reachable); DRU's definition text says "successors," matching `succᵢ` | No — settled in grill (Q1) |
| Allocation scope | Whole plan (all tasks' subtasks as one DAG, one shared core pool) | Matches `validate_structure`'s existing whole-plan `DAG` and `TeamManager`'s global `(core, priority)` pooling — no other layer resets anything per-task | No — settled in grill (Q2) |
| Implemented `AllocationConfig` surface | Only `strategy=worst_fit`, `sort_by=remaining_utilization_desc`, `weight=utilization`; everything else rejected | RULES.md §4 asks for fidelity to *this* paper's chosen combination; the general comparison framework (`first_fit`/`best_fit`/other orderings) has no current consumer and is a separate, much larger task | No — settled in grill (Q3) |
| `guided`/`validate` (`rta`/`rta_v`) | Not referenced anywhere in this change — no field read, no rejection/validation code either | Author's explicit correction during grill: don't even add the rejection-validation for these, just leave them alone | No — settled in grill (Q4), amended by author |
| Per-core capacity default | `1.0` when `AllocationConfig::capacity == 0.0` | The only capacity notion the paper's utilization-based bin-packing model implies (partitioned-FTP/EDF utilization bound) | No — settled in grill (Q5) |
| Unplaceable subtask | `std::runtime_error`, matching `JsonParser`'s existing throw-on-cycle convention | No fallback described in the paper; silently overcommitting or skipping would produce an unschedulable plan that only fails later at runtime | No — settled in grill (Q6) |
| Pre-assigned subtasks (`core != CORE_UNASSIGNED`) | Their utilization is subtracted from their core's starting capacity before placement begins; they are never moved | Otherwise Worst-Fit could overcommit a core a fixed-core plan (e.g. `mcflow_priority_*.json`) already partially fills | No — settled in grill (Q7) |
| `AllocationConfig` defaults (`sort_by`, `weight`) | Changed to `remaining_utilization_desc` / `utilization` (found during spec write-up, not grilled explicitly — flagged here for visibility) | Current defaults (`priority_desc` / `count`) aren't implemented by this change at all; a plan that omits the `allocation` block (or omits these fields) would otherwise hit the new hard-error path immediately. Since this change deliberately implements *only* WF+DRU, that should also be what "no config given" means | Flagged — not previously grilled, please confirm |
| Commit split | Two commits: (1) utilization + DRU ordering, (2) Worst-Fit placement | RULES.md §1 ~150-line target; (1) has an obvious standalone contract, (2) becomes a much smaller diff on top | No — settled in grill (Q8) |

## Interface Contracts

`src/allocator.hpp`:
```cpp
namespace allocator {

// Public entry point — signature unchanged from the stub.
void apply_auto_allocation(DeploymentPlan& plan);

namespace detail {

// U(subtask) = wcet_us / period_us.
double utilization(const SubtaskInfo& st);

// Builds the whole-plan DAG (all tasks' subtasks as nodes, plan.connections
// as edges) and returns the CORE_UNASSIGNED subtask ids ordered by
// descending remaining utilization (sum of direct successors' utilization).
// Ties broken by original plan order (stable sort).
std::vector<int> decreasing_remaining_utilization_order(const DeploymentPlan& plan);

// Resolves num_cores (plan.allocation.num_cores, or hardware_concurrency()
// if 0) and per-core capacity (plan.allocation.capacity, or 1.0 if 0).
// Seeds each core's remaining capacity by subtracting the utilization of
// every subtask already assigned to it. Walks `ordered_ids`, placing each
// on the core with the most remaining capacity; throws std::runtime_error
// if that core still can't fit it.
void worst_fit_place(DeploymentPlan& plan, const std::vector<int>& ordered_ids);

} // namespace detail
} // namespace allocator
```
`apply_auto_allocation` becomes: validate `plan.allocation` matches the one
implemented combination (throw otherwise) →
`decreasing_remaining_utilization_order(plan)` →
`worst_fit_place(plan, ordered_ids)`.

`src/deployment_plan.hpp` — `AllocationConfig` default member initializers:
```cpp
// before
std::string sort_by = "priority_desc";
std::string weight  = "count";

// after
std::string sort_by = "remaining_utilization_desc";
std::string weight  = "utilization";
```
Doxygen comment on `AllocationConfig` updated to state these are the only
values this codebase currently implements (RULES.md §10).

## Data Model

No struct field additions/removals. `AllocationConfig`'s two default
values change (above). `SubtaskInfo::core` is the only field mutated by
this change, exactly as the stub already does.

## Implementation Phases

1. **`allocator::detail::utilization` + `decreasing_remaining_utilization_order`**
   (`src/allocator.hpp`) — pure/read-only, builds the whole-plan `DAG`,
   computes utilization per subtask, computes remaining utilization for
   each `CORE_UNASSIGNED` subtask, returns the DRU-sorted id list. Paired
   with `tests/allocator_test.cpp` cases covering: a simple chain, a
   fan-out (one node with multiple direct successors, confirming only
   *direct* successors count, not transitive ones), and a node with no
   successors (remaining utilization 0).
2. **`allocator::detail::worst_fit_place` + `apply_auto_allocation`**
   (`src/allocator.hpp`) — consumes phase 1's ordering, resolves
   `num_cores`/capacity, seeds initial occupancy from pre-assigned
   subtasks, places greedily, throws on infeasibility. Paired with test
   cases covering: empty-plan (nothing to place), a plan where a
   pre-assigned subtask's utilization must be respected as starting
   occupancy, and an infeasible plan (confirms the throw).
3. **`AllocationConfig` default change + `plans/deployment_plan.json`
   update** — small, bundled with phase 2 since it's what makes the
   existing demo plan actually exercise the new allocator instead of
   throwing on its current `sort_by: "none"`.
4. **Full verification** — `make clean && make test`, all binaries green.

Two commits per the grill (Q8): phase 1 alone, then phases 2-4 together
(placement + defaults + verification form one coherent, testable unit —
splitting them further would leave an intermediate commit that doesn't
build a working allocator).

## Risks and Mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| `AllocationConfig` default change silently changes behavior for a plan that relies on the old defaults but sets no `allocation` block at all | A plan starts throwing where it previously assigned core 0 to everything (stub behavior) | Grep `plans/*.json` for any plan lacking `core` on some subtask and no explicit `allocation` block; confirmed only `plans/deployment_plan.json` needs auto-allocation today, and it's updated in phase 3 |
| Floating-point capacity comparison rejects an exact-fit placement due to rounding | A feasible plan spuriously throws | Small epsilon (`1e-9`) tolerance in the `capacity >= utilization` fit check |
| `std::thread::hardware_concurrency()` returns 0 on some platform (unspecified per the standard) | `num_cores` resolves to 0, every placement fails | Not newly introduced by this change (same fallback question would apply to any prior `num_cores=0` use); document the assumption in the Doxygen comment, no extra runtime guard — out of scope for this codebase's target environment |

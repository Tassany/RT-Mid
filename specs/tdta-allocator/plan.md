# TDTA Core Allocator — Technical Plan

## Technical Approach

Two new modules on top of `specs/eru-allocator/`'s split
(`allocator.hpp`/`dru.hpp`/`eru.hpp`), plus one data-model addition:

- **`src/ied.hpp` + `src/ied.cpp`** — Algorithm 1 alone. Pure function:
  given one task's subtasks and its own connections, returns the reduced
  connection list with invalid edges removed. Builds a throwaway `DAG`
  internally to compute predecessor sets and reachability ("constrains");
  never mutates the shared `DAG` class (no `remove_edge` added to it —
  narrow, single-purpose module instead of widening a shared type for one
  caller).
- **`src/tdta.hpp` + `src/tdta.cpp`** — everything downstream of IED:
  level assignment (Eq. 5), earliest-start-time (Eq. 8), Str-structure
  detection (Definition 1), the per-level/per-group orchestration
  (Algorithm 3's body), and the outer loop across `plan.tasks` by
  **ascending** `TaskInfo::priority` (smaller value = higher priority, per
  the paper's convention — Sect. III). Calls
  `eru::detail::equilibrium_remaining_utilization_place` (from
  `specs/eru-allocator/`) once per group — the same detail-level function
  `eru.cpp`'s own whole-plan entry point calls, just with a much smaller,
  topology-chosen `group_ids` each time.
- **`src/deployment_plan.hpp`** — `TaskInfo` gains `int priority =
  TASK_PRIORITY_UNSET;` (new sentinel, `-1`, mirroring `CORE_UNASSIGNED`'s
  existing pattern).
- **`src/allocator.hpp`/`allocator.cpp`** — dispatcher gains a third
  branch: `"tdta"` → `tdta::apply_tdta_allocation`.

Each of IED / levels / earliest-start-time / Str-detection is verified
independently against the paper's own worked example (Fig. 2 → Fig. 3,
`Ci=22, Ti=Di=100`, WCETs `⟨3,5,2,5,3,2,2⟩` for `V_i,1..V_i,7`) — traced by
hand below so the plan itself, not just the eventual test file, is
checkable against the paper:

- **Fig. 2 edges**: `1→2, 1→3, 1→4, 2→4, 2→5, 2→6, 3→6, 4→7, 5→7, 6→7`.
- **IED removes exactly `e(1,4)`**: `4`'s predecessors are `{1,2}`; `1`
  constrains `2` (direct edge); so `1`'s edge into `4` is redundant with
  `1→2→4`. No other predecessor-pair in the graph has one constraining the
  other, so nothing else is removed — matches the paper's own claim of
  exactly one additional `Str` structure appearing post-IED.
- **Levels (post-IED)**: `0:{1}`, `1:{2,3}`, `2:{4,5,6}`, `3:{7}` — matches
  Fig. 3's stated 4 levels, including `V_i,6`'s level 2 (`max(L(2),L(3))+1
  = max(1,1)+1`), the paper's own example computation.
- **Str structures (post-IED)**: node `1`'s successors `{2,3}` each have
  predecessor set exactly `{1}` → `ξ_1={2,3}` (the paper's own pre-IED
  example, still valid post-IED). Node `2`'s successors `{4,5,6}`: `4` and
  `5` now have predecessor set exactly `{2}` (post-IED), `6` does not
  (`{2,3}`) → `ξ_2={4,5}`, `6` falls to the level-2 leftover group. This
  is the "additional `Str` structure" the paper's Sect. V text refers to.

## Key Decisions

| Decision | Choice | Rationale | Needs sign-off? |
|---|---|---|---|
| Per-task DAG scope | `Gᵢ` built from exactly task i's subtasks + connections whose both endpoints belong to task i | Matches the paper's model (independent DAG tasks); confirmed no sample plan currently has cross-task connections | No — settled in grill |
| `DAG` class unchanged | IED/levels/Str-detection build short-lived local `DAG` instances (or plain adjacency maps) from filtered edge lists; no `remove_edge` or reachability method added to the shared `DAG` class | Keeps a widely-used shared type narrow (same house style as `narrow-component-registry-scope`); this feature is the only caller that needs edge removal/reachability at all | Flagged — alternative (add `remove_edge`+`reachable` to `DAG`) is reasonable too, confirm preference |
| "Constrains" (reachability) timing | Computed once on the **original** (pre-removal) edge set; all of Algorithm 1's removal decisions use that fixed relation, never recomputed mid-algorithm | Theorem 1 proves an invalid edge is redundant — removing it cannot change any other node's reachability — so computing reachability once is provably equivalent to recomputing incrementally, and much simpler to implement correctly | Flagged — not previously grilled |
| `TASK_PRIORITY_UNSET` sentinel | New `constexpr int TASK_PRIORITY_UNSET = -1;`, `TaskInfo::priority` defaults to it | Mirrors `CORE_UNASSIGNED`'s existing pattern exactly; resolves spec.md's open question without inventing a new convention | No — direct consequence of the priority-field decision already made |
| Missing-priority behavior | `tdta::apply_tdta_allocation` throws `std::runtime_error` if `plan.tasks.size() > 1` and any task's `priority == TASK_PRIORITY_UNSET`; a single-task plan proceeds regardless (no ordering ambiguity possible). This check runs **before** the ascending sort, never after — `TASK_PRIORITY_UNSET` is `-1`, numerically the smallest possible value, so sorting first and checking after would silently treat an unset task as highest-priority instead of throwing | Resolves spec.md's `[NEEDS CLARIFICATION]` — avoids a silent, arbitrary tie-break the author didn't ask for, while not forcing every existing single-task plan to add a priority it doesn't need | No — resolves the spec's flagged open question, confirm |
| Str-structure representation | `std::vector<std::vector<int>>` (subtask ids), only groups with `size ≥ 2` kept | A "structure" with one member can't self-interfere (Definition 3 needs two members); Lemma 4's own proof picks two distinct members, implying `≥2` throughout the paper's usage | No |
| Str-group ordering tie-break | Groups with equal minimum `δ` ordered by their common-predecessor subtask's id (ascending) | The paper only specifies ascending-by-earliest-start-time; ties aren't addressed. A deterministic, cheap tie-break that doesn't affect correctness, only which of several equally-valid orderings is produced | Flagged — not previously grilled |
| Cross-module call | `tdta.cpp` calls `eru::detail::equilibrium_remaining_utilization_place` directly (not the whole-plan `eru::apply_eru_allocation`) | That's the actual Algorithm-2-shaped primitive Algorithm 3 calls per group; the whole-plan entry point is `eru-allocator`'s own top-level strategy, a different caller of the same primitive | No — direct reading of Algorithm 3 line 12 |

## Interface Contracts

`src/deployment_plan.hpp`:
```cpp
constexpr int TASK_PRIORITY_UNSET = -1; // alongside CORE_UNASSIGNED

struct TaskInfo {
    int id;
    int priority = TASK_PRIORITY_UNSET; // pi(tau_i); allocation-time-only,
                                         // unrelated to SubtaskInfo::priority
    std::vector<SubtaskInfo> subtasks;
};
```

`src/ied.hpp`:
```cpp
namespace ied {
// Algorithm 1. task_connections must all have both endpoints in
// task_subtasks (caller's responsibility -- see build_task_dag in
// tdta.cpp). Returns the reduced connection list; task_connections
// itself is never mutated.
std::vector<ConnectionInfo> remove_invalid_edges(
    const std::vector<SubtaskInfo>& task_subtasks,
    const std::vector<ConnectionInfo>& task_connections);
} // namespace ied
```

`src/tdta.hpp`:
```cpp
namespace tdta {

// Dispatcher-facing entry point: orders plan.tasks by ascending
// TaskInfo::priority (smaller value = higher priority; stable; throws if
// ambiguous -- see Key Decisions), then calls detail::allocate_task once
// per task in that order.
void apply_tdta_allocation(DeploymentPlan& plan);

namespace detail {

// Eq. 5, over the (already IED-reduced) per-task edges.
std::unordered_map<int, int> compute_levels(
    const std::vector<SubtaskInfo>& task_subtasks,
    const std::vector<ConnectionInfo>& reduced_connections);

// Eq. 8, over the same reduced edges.
std::unordered_map<int, double> compute_earliest_start_times(
    const std::vector<SubtaskInfo>& task_subtasks,
    const std::vector<ConnectionInfo>& reduced_connections);

// Definition 1. One vector<int> of subtask ids per Str structure found
// (|result[k]| >= 2 always).
std::vector<std::vector<int>> find_str_structures(
    const std::vector<SubtaskInfo>& task_subtasks,
    const std::vector<ConnectionInfo>& reduced_connections);

// Algorithm 3 for exactly one task: IED -> levels -> per-level grouping
// (one group per Str structure, ordered by min earliest-start-time within
// the group, plus one final non-Str leftover group) ->
// eru::detail::equilibrium_remaining_utilization_place per group, in
// order. Mutates only this task's subtasks' SubtaskInfo::core.
void allocate_task(DeploymentPlan& plan, const TaskInfo& task);

} // namespace detail
} // namespace tdta
```

`src/allocator.hpp` — `apply_auto_allocation` gains a third branch:
`cfg.strategy == "tdta"` → `tdta::apply_tdta_allocation(plan)`.

`src/deployment_plan.hpp` — `AllocationConfig::strategy` comment gains
`| tdta` alongside `worst_fit | eru` (from `eru-allocator`'s own plan).

## Data Model

- `TaskInfo` gains `priority` (above). No other struct changes.
- No changes to `SubtaskInfo`, `AllocationConfig`'s fields (only its
  `strategy` doc-comment), `DAG`, or any runtime (`TeamManager`/
  `Dispatcher`) type.

## Implementation Phases

1. **`TaskInfo::priority` + `TASK_PRIORITY_UNSET`** (`deployment_plan.hpp`)
   — additive, no behavior change (nothing reads it yet). Existing tests
   unaffected.
2. **`ied::remove_invalid_edges`** (`src/ied.cpp`) — tested against the
   worked example above (removes exactly `e(1,4)`), a graph with no
   invalid edges (no-op), and a case with two independent invalid edges
   (confirms the pairwise check isn't order-dependent).
3. **`tdta::detail::compute_levels`** — tested against the worked
   example's 4 levels, plus a single-source-single-sink trivial case
   (level 0 and level 1 only).
4. **`tdta::detail::compute_earliest_start_times`** — small dedicated
   test using the worked example's own WCETs (`3,5,2,5,3,2,2`).
5. **`tdta::detail::find_str_structures`** — tested against the worked
   example's `ξ_1={2,3}`, `ξ_2={4,5}`, confirming `1`, `6`, `7` belong to
   no structure.
6. **`tdta::detail::allocate_task` + `tdta::apply_tdta_allocation` +
   dispatcher's `"tdta"` branch** — end-to-end test reproducing the
   worked example's own DAG task on a small core count (confirms
   Str-structure members land on *different* cores where capacity
   allows — the paper's own load-balancing claim), plus a two-task,
   distinct-priority test confirming allocation order, plus the
   missing-priority throw case.
7. **Full verification** — `make clean && make test`, all binaries green,
   no regression in `dru_test`/`eru_test`/every other existing test.

One commit per phase (7 commits) — each phase's own function is
independently meaningful and testable per RULES.md §1's ~150-line target;
phase 6 is the largest and may need splitting further in `tasks.md` if it
grows past that guideline.

## Risks and Mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| Reachability-once (vs. incremental) assumption is subtly wrong for some DAG shape the worked example doesn't cover | IED removes an edge it shouldn't (or misses one), corrupting levels/Str-detection downstream | Theorem 1's proof is general, not worked-example-specific; still, add one extra hand-built test case with a longer transitive chain (3+ hops) beyond the paper's own 7-node example |
| `tdta` strategy on a plan someone already wrote for `worst_fit`/`eru`, now with unset task priorities and >1 task | Throws where it previously would have run under the other two strategies | Expected and desired (Key Decisions) — `tdta` is a new opt-in value; no existing plan sets `strategy: "tdta"` today |
| `eru::detail::equilibrium_remaining_utilization_place`'s "recompute remaining capacity from plan state" design (from `eru-allocator/plan.md`) gets called many times per task here | Repeated O(cores) rescans per group; noted as a future-relevance risk in `eru-allocator/plan.md` and now realized | Still acceptable at this codebase's plan sizes; revisit only if phase 7's checkpoint shows a measurable slowdown |
| Str-detection or level computation silently assumes single source/sink (Non-Goal) on a malformed task | Confusing error or wrong result instead of a clear rejection | Out of scope per spec.md's Non-Goals; not hardened against further here — same implicit assumption WF+DRU's DAG handling already carries |

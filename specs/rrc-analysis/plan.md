# RRC WCRT Analysis — Technical Plan

## Technical Approach

One new module, `src/rrc.hpp` + `src/rrc.cpp`, built in the same
dependency order as the equations themselves (path enumeration →
self-interference → Ω → I^h_j/R(λ) → R(τ) → schedulability), reusing
`ied::remove_invalid_edges` and `tdta::detail::find_str_structures`
exactly as they are. One small extraction: the "slice `plan.connections`
to one task's own subtasks" loop currently inlined in
`tdta::detail::allocate_task` moves to `ied::task_connections(plan,
task)` — RRC needs the identical operation for *every* higher-priority
task it looks at (not just once), a third call site, past the point
where duplicating it is the simpler choice.

For a given path `λᵢ,ₖ` being analyzed, every higher-priority task
`τⱼ ∈ hp(τᵢ)` needs its own IED-reduced graph and its own Str structures
recomputed fresh (same per-task slicing `tdta::detail::allocate_task`
already does) — `rrc.cpp` calls `ied::remove_invalid_edges` +
`tdta::detail::find_str_structures` once per `(λᵢ,ₖ, τⱼ)` pair. For this
codebase's plan sizes this is cheap; `plan.md`'s Risks table notes it as
the one thing to revisit if a much larger plan ever makes this eval tool
slow.

## Worked Example (hand-traced, used in `tasks.md`'s end-to-end test)

A minimal 2-task example purpose-built to exercise every term in Eq. 2–4
at least once, including a non-trivial `Ω` — `specs/tdta-allocator/`'s
own worked example (the paper's Fig. 2) never exercises `Ω`/`I^h_j`/`R`
at all, since it has no second task.

**τ_HIGH** (`priority=0`, `T=D=100`): `V1(C=2,P=p0) → V2(C=4,P=p0)`,
`V1 → V3(C=3,P=p1)`, `V2 → V4(C=1,P=p1)`, `V3 → V4`. One Str structure
`ξ_{1,1} = {V2,V3}` (parent `V1`). No invalid edges (IED is a no-op here).

**τ_LOW** (`priority=1`, `T=D=50`): `W1(C=2,P=p0) → W2(C=2,P=p1) →
W3(C=1,P=p0)` — a plain chain, one path `λ_{2,1} = {W1,W2,W3}`.

- `L(λ_{2,1}) = 2+2+1 = 5`.
- `self(λ_{2,1}) = ∅` — a chain has no two subtasks free of a precedence
  constraint, so Eq. 2's middle term is 0.
- `ρ(λ_{2,1}) = {p0,p1}`.
- `η^{2,1}_{1,1} = ρ(ξ_{1,1}) ∩ ρ(λ_{2,1}) = {p0,p1} ∩ {p0,p1} = {p0,p1}`,
  `|η|=2`.
- `Ω^{2,1}_{1,1} = (2-1) · min(C(V2)=4, C(V3)=3) = 3`.
- `Σ C_{1,t}` over every `τ_HIGH` subtask whose processor is in
  `ρ(λ_{2,1})` — all four are on `p0`/`p1`, so `= 2+4+3+1 = 10`.
- `I^h_1(λ_{2,1}) ≤ ⌈R/100⌉ · (10 − 3) = ⌈R/100⌉ · 7`.
- Fixed point: `R⁰ = L + self = 5`. `⌈5/100⌉=1` → `I = 7` → `R¹ = 12`.
  `⌈12/100⌉=1` → `I = 7` → `R² = 12`. **Converged at `R(λ_{2,1}) = 12`**,
  in 2 iterations.
- `R(τ_LOW) = max{12} = 12 ≤ D_LOW = 50` → schedulable.

A second, separate small case (reusing the exact diamond
`1→2,1→3,2→4,3→4` already used in `specs/tdta-allocator/tasks.md`'s IED
tests) isolates self-interference alone: `P(2)=P(3)=p0` (same processor,
no precedence between 2 and 3 — siblings in a diamond) must put `3` in
`self(2)` and `2` in `self(3)`; `P(2)=p0, P(3)=p1` must leave both empty.

## Key Decisions

| Decision | Choice | Rationale | Needs sign-off? |
|---|---|---|---|
| Fixed-point iteration procedure | Start `R⁰ = L(λ) + Σself C`; each step recompute `Σⱼ I^h_j(λ, R)` using the *previous* iterate's `R` inside `⌈R/Tⱼ⌉`, then `R' = L + Σself + Σ I^h_j`; stop when `R' = R` (converged) or `R' > Dᵢ` (declare this path unschedulable, stop early — no need to keep iterating once infeasibility is already proven) | Standard real-time-systems response-time-analysis recurrence (Audsley/Joseph-Pandya-style); this paper's own text says it draws on a *different*, uncited-here article for "more details" — this is the conventional way to solve the exact shape of recurrence Eq. 2–3 state, not invented for this project | Flagged — spec.md's paper-gap disclosure, confirm the convention is acceptable |
| Iteration cap | Constant `kMaxIterations = 10000`; throws `std::runtime_error` if exceeded without convergence | Resolves spec.md's open question (grilled) — defensive guard against a malformed plan or upstream bug producing a non-terminating case, cheap to add | No — resolves the spec's flagged open question |
| `hp(τᵢ)` requires every task's priority set | If `plan.tasks.size() > 1`, throws if any task's `TaskInfo::priority == TASK_PRIORITY_UNSET` — identical convention to `tdta::apply_tdta_allocation` | `hp(τᵢ)` is undefined without a total priority order; reusing the exact same guard `tdta-allocator` already established keeps the two features consistent instead of inventing a second convention | No — direct reuse of an existing, already-grilled decision |
| Per-hp-task IED/Str recomputed per path | No caching across paths/calls in this feature; each `(λᵢ,ₖ, τⱼ)` pair recomputes `τⱼ`'s reduced graph + Str structures fresh | Simplicity over performance for this codebase's plan sizes (tens of subtasks); flagged in Risks if it ever needs to change | Flagged — not previously grilled, a cache keyed by task id would be the natural upgrade if profiling ever shows this matters |
| `ied::task_connections` extraction | Move the "slice `plan.connections` to one task's own subtasks" loop out of `tdta::detail::allocate_task` into a new `ied::task_connections(plan, task)`, used by both `tdta.cpp` (replacing its inline loop, behavior unchanged) and the new `rrc.cpp` | Third call site for the identical operation — past the "two call sites, don't abstract yet" line this codebase's own precedent (`wf-dru-allocator/plan.md`) draws | Flagged — small mechanical refactor of already-shipped (but uncommitted) `tdta.cpp` code, confirm it's wanted now vs. duplicating a third time |
| `Ω` term exclusion when `η = ∅` | A Str structure whose processors don't overlap `ρ(λᵢ,ₖ)` at all contributes nothing to the `Σ Ω` subtraction (consistent with Eq. 4's own stated precondition `η ≠ ∅`) | Direct reading of Eq. 4's stated domain | No |
| Self-interference reachability graph | Computed on the IED-reduced graph (same graph levels/Str/δ already use), not the original | Theorem 1 proves removing an invalid edge never changes any node's reachability to any other — so this is provably equivalent to using the original graph, and keeps one consistent "the graph" notion across every feature built so far | No — direct consequence of Theorem 1, already relied on by `ied`/`tdta` |

## Interface Contracts

`src/ied.hpp` (new function on the existing module):
```cpp
namespace ied {
// Connections from plan.connections whose both endpoints are in task's
// own subtasks. Extracted from tdta::detail::allocate_task's own inline
// loop (now its only caller besides this), behavior unchanged.
std::vector<ConnectionInfo> task_connections(const DeploymentPlan& plan, const TaskInfo& task);
} // namespace ied
```

`src/tdta.cpp`: `allocate_task` calls `ied::task_connections(plan, task)`
instead of its own inline slicing loop.

`src/rrc.hpp`:
```cpp
namespace rrc {

namespace detail {

// Sect. III's lambda_i: every complete source-to-sink path, as sequences
// of subtask ids, over one task's own (already IED-reduced) subtasks/
// connections.
std::vector<std::vector<int>> enumerate_complete_paths(
    const std::vector<SubtaskInfo>& task_subtasks,
    const std::vector<ConnectionInfo>& reduced_connections);

// L(lambda) = sum of WCET over the path's own subtasks.
double path_length(const std::vector<int>& path,
                    const std::vector<SubtaskInfo>& task_subtasks);

// Definition 3, restricted to task_subtasks (same DAG task only):
// subtasks with no precedence constraint to subtask_id (either direction,
// on the IED-reduced graph) AND the same assigned core.
std::vector<int> self_interference_set(
    int subtask_id,
    const std::vector<SubtaskInfo>& task_subtasks,
    const std::vector<ConnectionInfo>& reduced_connections);

// self(lambda) = union of self_interference_set over the path's own
// subtasks, deduplicated; WCET sum of that set is Eq. 2's middle term.
std::vector<int> path_self_interference(
    const std::vector<int>& path,
    const std::vector<SubtaskInfo>& task_subtasks,
    const std::vector<ConnectionInfo>& reduced_connections);

// Eq. 4, summed over every Str structure of hp_task that overlaps
// path's processors (eta != empty; see Key Decisions for the eta = empty
// case). hp_task_subtasks/hp_task_reduced are hp_task's own (sliced,
// IED-reduced) graph; core_by_id covers the whole plan.
double omega_sum(
    const std::vector<SubtaskInfo>& hp_task_subtasks,
    const std::vector<ConnectionInfo>& hp_task_reduced,
    const std::vector<int>& path,
    const std::unordered_map<int, int>& core_by_id);

// Eq. 2 + 3 combined: the fixed-point R(lambda_i,k). task is lambda's own
// DAG task; plan supplies every higher-priority task (ascending
// TaskInfo::priority) and every subtask's core. Throws std::runtime_error
// if plan.tasks.size() > 1 and any task's priority is
// TASK_PRIORITY_UNSET, or if the iteration cap (plan.md) is exceeded.
double response_time_of_path(
    const DeploymentPlan& plan,
    const TaskInfo& task,
    const std::vector<int>& path,
    const std::vector<SubtaskInfo>& task_subtasks,
    const std::vector<ConnectionInfo>& reduced_connections);

} // namespace detail

// Eq. 1: R(tau_i), the max response time over task's own complete paths.
double compute_wcrt(const DeploymentPlan& plan, const TaskInfo& task);

// R(tau_i) <= D_i for every task in plan. Returns the ids of tasks that
// fail (empty = plan fully schedulable).
std::vector<int> check_schedulability(const DeploymentPlan& plan);

} // namespace rrc
```

## Data Model

No struct field additions/removals — reuses `TaskInfo::priority`,
`SubtaskInfo::core`/`wcet_us`/`period_us`/`deadline_us` exactly as they
are.

## Implementation Phases

1. **`ied::task_connections` extraction** — move `tdta::detail::allocate_task`'s
   inline slicing loop out; `tdta_test.cpp`'s existing `allocate_task`
   case must still pass byte-for-byte (zero behavior change).
2. **`enumerate_complete_paths` + `path_length`** — tested against the
   worked example's `τ_LOW` (one path, `L=5`) and `τ_HIGH` (two paths,
   via `V2`/via `V3`).
3. **`self_interference_set` + `path_self_interference`** — tested
   against the diamond case (same-core siblings self-interfere,
   different-core siblings don't) and the worked example's chain (empty).
4. **`omega_sum`** — tested against the worked example's `Ω^{2,1}_{1,1}=3`
   exactly, plus an `η = ∅` case (structure on processors the path never
   touches) confirming it contributes 0.
5. **`response_time_of_path` + `compute_wcrt`** — tested against the
   worked example's full fixed-point trace (`R⁰=5 → R¹=12 → R²=12`,
   converges at `R(τ_LOW)=12`), the iteration-cap throw (a hand-built
   case engineered not to converge), and the missing-priority throw.
6. **`check_schedulability`** — tested against the worked example
   (`τ_LOW` schedulable, `12 ≤ 50`) and an engineered unschedulable case.
7. **Full verification** — `make clean && make test`.

One commit per phase (7 commits), matching `tdta-allocator`'s own split.

## Risks and Mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| Recomputing IED/Str per `(path, hp-task)` pair becomes slow on a much larger plan than this codebase currently has | Schedulability checks take noticeably long | Acceptable now (tens of subtasks); the natural fix (cache per task id) is a small, localized change if profiling ever shows it matters — not pre-built speculatively (RULES.md §2) |
| The fixed-point convention (Key Decision) turns out to differ from the uncited original RRC paper's actual procedure | A computed `R(τᵢ)` could differ from "the" paper-intended value in some edge case this project can't check without that reference | Disclosed explicitly in spec.md and here rather than silently assumed; the worked example's hand trace is checkable independently of which paper is "right" about iteration mechanics, since it follows directly from Eq. 2–4's own stated inequalities |
| `ied::task_connections` extraction accidentally changes `tdta::detail::allocate_task`'s behavior | Regression in already-shipped (if uncommitted) `tdta-allocator` work | Phase 1 is its own commit, zero intended behavior change, `tdta_test.cpp`'s existing cases are the regression check before any new RRC code is written |

# Fonseca et al. 2016 RTA — Technical Plan

## Technical Approach

One new module, `src/rta_fonseca2016.hpp` + `src/rta_fonseca2016.cpp`
(filenames match `deployment_plan.hpp`'s own pre-existing reference),
reusing `rrc::detail::enumerate_complete_paths`/`path_length` and
`ied::remove_invalid_edges`/`task_connections`. Built in this order:
execution-region grouping → p-workload/Θ/self (Lemma 1) → Theorem 3's
virtual-task expansion → Algorithm 1's recursive per-core WCRT → Theorem
2's assembly into `R(λ)` → Corollary 1's max into `R(τᵢ)`.

**How `R(λ)` is actually assembled — resolved after a mis-reading caught
during spec review (worth recording so it isn't re-introduced):**
`R(λ)` is **not** read directly off of Algorithm 1's own recursion output
for the path as a whole. The paper's own text (right after Algorithm 1)
states it plainly: *"At this point, the WCRT of the original path can be
computed thanks to Theorem 2."* Algorithm 1's job is only to populate, for
**every distinct core the path touches**, that core's own self-suspending
task response time `R(τᵖ)` and suspension bound `S^{p,ub}` (Eq. 6).
Theorem 2 (Eq. 5) is then applied **explicitly, as its own final step**:
`R(λ) = Σ_{p∈proc(λ)} [R(τᵖ) − S^{p,ub}]`.

## Worked Example (hand-traced, used in `tasks.md`'s end-to-end tests)

Purpose-built with a path spanning **3 distinct cores** (so the middle
core is genuinely suspended-over, not just visited once at a boundary),
a higher-priority **DAG** task (Theorem 3), and a non-empty `self(λ)`.

**τ_HIGH** (`priority=0`, `T=D=100`): `Hs(C=1,core9) → H1(C=2,core0)`,
`Hs → H2(C=1,core0)`, `H1 → Hm(C=1,core9)`, `H2 → Hm`. Two paths, both
`core9→core0→core9` shape; by symmetry both give `R=5` (worked below for
one, the other is identical by symmetry): path `{Hs,H1,Hm}` — `self` =
`{H2}` (shares core0, no precedence to the path's boundary-on-core0
subtask `H1` restricted to core0); `τ^{core0}` (just `H1`) has
`R=C(H1)+self(H2)=2+1=3`, `S^{core0,ub}=0` (q=1); `τ^{core9}` (`Hs,Hm`)
has `S^{core9,ub}=R(τ^{core0})−0=3`, and via Eq. 8 (no higher-priority
task at all — `τ_HIGH` has none): `R(τ^{core9}) = (C(Hs)+C(Hm)=2) +
S^{ub}(3) + 0 + 0 = 5`. Theorem 2: `R(λ)=[R(τ^{c9})−S^{c9,ub}] +
[R(τ^{c0})−S^{c0,ub}] = [5−3]+[3−0] = 2+3 = 5`. **`R(τ_HIGH) = 5`**
(`D=100`, schedulable) — this value feeds Theorem 3's `J_{j,o} =
R(τⱼ)−Cₒ` below.

**τ_LOW** (`priority=1`, `T=D=50`): source `W1(C=2,core0)`, `W1→W2(C=3,core1)`,
`W1→W4(C=1,core0)`, `W2→W3(C=1,core0,sink)`, `W4→W3`. Two paths:

- **`λ1 = {W1,W2,W3}`** (cores `0,1,0` — the interesting one). No
  consecutive same-core pair, so no merging. `proc(λ1)={c0,c1}`.
  `Θ` is empty on both cores (`W1` is the source — no predecessors at
  all; `W3` is the sink — no successors at all), so
  `self(λ1) = {W4}` (shares `c0`, not in the path, not excluded by `Θ`
  — **note**: `W4` is actually a direct successor of `W1` *and* direct
  predecessor of `W3` in the full DAG, yet Lemma 1's `Θ` formula still
  doesn't exclude it, since `Θ` only looks at predecessors of the path's
  *first* subtask on a core and successors of its *last* — not "nodes
  sandwiched between them". This is a real, paper-documented source of
  pessimism, not an implementation bug — worth a code comment where
  `self_interference_set` is tested, so a future reader doesn't mistake
  it for one).
  - `τ^{c1}` (just `W2`): `hp` filtered to core1 = ∅ (`τ_HIGH` has nothing
    on core1). `R(τ^{c1}) = C(W2) = 3`, `S^{c1,ub}=0`.
  - `τ^{c0}` (`W1`,`W3`, `q=2`): `hp` filtered to core0 = `{τ_HIGH}`
    (`H1`,`H2` are on core0) → Theorem 3 expands it into
    `τ_{HIGH,H1}(C=2, J=R(τ_HIGH)−C(H1)=5−2=3, T=100)` and
    `τ_{HIGH,H2}(C=1, J=5−1=4, T=100)`. `self` filtered to core0 =
    `{W4}` (`C=1`). `S^{c0,ub} = R(τ^{c1})−S^{c1,ub} = 3−0 = 3`.
    Eq. 8: `R(τ^{c0}) = (C(W1)+C(W3)=3) + S^{ub}(3) +
    [⌈(R+3)/100⌉×2 + ⌈(R+4)/100⌉×1] + self(1)`. Fixed point:
    `R⁰=7` → iter: `⌈10/100⌉=1,⌈11/100⌉=1` → interference `3` →
    `R¹=10` → iter: `⌈13/100⌉=1,⌈14/100⌉=1` → interference `3` →
    `R²=10`. **Converged, `R(τ^{c0})=10`.**
  - Theorem 2: `R(λ1) = [R(τ^{c0})−S^{c0,ub}] + [R(τ^{c1})−S^{c1,ub}]
    = [10−3] + [3−0] = 7+3 = **10**`.
- **`λ2 = {W1,W4,W3}`** (cores `0,0,0` — all three consecutive on the
  same core, **merged** into one region per the required preprocessing
  step: `C_merged = 2+1+1 = 4`). `proc(λ2)={c0}` only.
  `self(λ2) = ∅` (no other `τ_LOW` subtask is on core0 besides the path's
  own three). `hp` filtered to core0 = `{τ_HIGH}` (same Theorem-3
  expansion as above). Eq. 8 (single region, `q=1`, `S^{ub}=0`):
  `R = 4 + [⌈(R+3)/100⌉×2+⌈(R+4)/100⌉×1] + 0`. Fixed point: `R⁰=4` →
  `⌈7/100⌉=1,⌈8/100⌉=1` → interference `3` → `R¹=7` → `⌈10/100⌉=1,⌈11/100⌉=1`
  → interference `3` → `R²=7`. **Converged, `R(λ2)=7`.** Theorem 2
  with one core: `R(λ2)=[7−0]=7`.

**`R(τ_LOW) = max(10, 7) = 10`** (`D=50`, schedulable).

## Key Decisions

| Decision | Choice | Rationale | Needs sign-off? |
|---|---|---|---|
| ~~Per-core memoization, not a subtask-pair matrix~~ **Superseded post-delivery** | `std::map<std::pair<int,int> /*first,last region index*/, {R, S_ub}>` — range-keyed, matching the paper's own `RTs` matrix (indexed by subtask pairs) after all | The original per-core-keyed simplification was found wrong, not just unconfirmed: `tools/eval/fonseca_wcrt_eval_main.cpp` run against a real topology (TDTA's own allocation of `interference_topology.hpp`'s HIGH task) produced an alternating `core0,core1,core0,core1` path, where core0 and core1 each get resolved over two genuinely different, non-overlapping ranges — a core-keyed map collides on this; a range-keyed one doesn't. See `progress.md`'s "Post-delivery correctness fix" entry for the full derivation and the regression test added. | No — resolved; this row is kept (struck through) as a record of a flagged-but-wrong simplification, not deleted, since the ADR-0001 precedent in this codebase values keeping that kind of history visible |
| Execution-region merging is required, not optional | Consecutive same-core subtasks in a path are merged (summed WCET) *before* Algorithm 1's recursion runs — Section V's own stated simplifying assumption, but this feature treats it as load-bearing | Traced by hand (worked example's `λ2`): without merging, the "same-core-boundary" recursive case can hand itself a "remote" subtask that's actually on the *same* core as the boundary pair, which Eq. 6 has no sensible interpretation for — not just a convenience, required for Algorithm 1's own cases to stay well-defined | No — directly demonstrated by the worked example, not a judgment call |
| `S^{p,ub}` (Eq. 6) computed by explicit per-core summation | When resolving a same-core-boundary pair, sum `[R(τᵒ)−S^{o,ub}]` over every *distinct* core `o` present in the trimmed middle range, reading each from the per-core memo (already populated by recursing into that middle first) | Eq. 6 is itself a sum over remote cores; reading the already-memoized entries after recursing into the middle generalizes correctly to a middle spanning more than one distinct core, not just the single-remote-core shape the worked example happens to use | Flagged — not literally spelled out by Algorithm 1's own pseudocode (which shows the single-remote-core case via its own recursive structure); this is this feature's own, reasoned completion of it for the general case |
| Theorem 2 applied as an explicit final step | `response_time_of_path` runs the recursion once (populating the per-core memo for every core in `proc(λ)`), then computes `R(λ)` via Eq. 5 directly, rather than trying to make the recursion itself return a path-wide value | Matches the paper's own text precisely ("the WCRT of the original path can be computed thanks to Theorem 2") — resolves the spec-review mis-reading recorded in the Technical Approach above | No — directly stated by the paper, re-confirmed by re-reading |
| `self_interference_set` kept as a distinct name from `rrc::detail::self_interference_set` despite living in a different namespace | Same name, deliberately — both represent "the self-interference concept" in their respective paper's own model, just computed differently (documented prominently in both modules' Doxygen and in `spec.md`'s Overview) | Parallel structure between the two sibling features aids readability more than a forced rename would avoid confusion, given the namespaces already disambiguate and the difference is already documented at the point someone would look it up | No |
| Theorem 3 virtual tasks computed from each hp DAG task's *own* WCRT via this same paper's analysis | `virtual_tasks_for_core` requires `R(τⱼ)` already computed — enforced by processing tasks in ascending `TaskInfo::priority` order, same `plan.tasks.size()>1 ⟹ priority required` guard `rrc-analysis`/`tdta-allocator` already use | Direct reading of Theorem 3's own `J_{j,o} = R(τⱼ)−Cₒ` definition; reusing the established guard keeps this feature consistent with its two siblings rather than inventing a third convention | No |
| Iteration cap + epsilon for Eq. 7/8's fixed point | Same constants/convention as `rrc::detail::response_time_of_path` (`kMaxIterations=10000`, `kEpsilon=1e-9`) | Identical shape of problem (a monotonic `ceil`-based RTA recurrence), no reason to invent different constants; code itself is *not* shared (different right-hand side), only the convention is | No |

## Interface Contracts

`src/rta_fonseca2016.hpp`:
```cpp
namespace rta_fonseca2016 {

namespace detail {

// Section V's stated simplification, required (see Key Decisions): groups
// a path into maximal consecutive-same-core runs, summing WCET per run.
struct ExecutionRegion {
    int core;
    std::vector<int> subtask_ids; // contiguous run, path order
    double wcet;
};
std::vector<ExecutionRegion> group_execution_regions(
    const std::vector<int>& path, const std::vector<SubtaskInfo>& task_subtasks);

// Definition 3: sum of WCET over ALL of task_subtasks assigned to core
// (not path-restricted).
double p_workload(const std::vector<SubtaskInfo>& task_subtasks, int core);

// Lemma 1 / Eq. 3-4: self(lambda) -- subtasks sharing a core with path,
// excluding the path's own members and Theta (provably non-interfering
// via core-restricted pred/succ of the path's first/last subtask on each
// shared core). A DIFFERENT computation from rrc::detail::self_interference_set
// (see spec.md's Overview) despite the same name.
std::vector<int> self_interference_set(
    const std::vector<int>& path,
    const std::vector<SubtaskInfo>& task_subtasks,
    const std::vector<ConnectionInfo>& reduced_connections);

// Theorem 3: one virtual sequential task per subtask of hp_task assigned
// to core. r_hp_task is hp_task's own WCRT (already computed -- caller's
// responsibility, enforced by priority-ordered processing).
struct VirtualTask { double c; double t; double j; };
std::vector<VirtualTask> virtual_tasks_for_core(
    const TaskInfo& hp_task, int core, double r_hp_task);

// Algorithm 1 + Theorem 2 combined: R(lambda_i,k). Runs the recursive
// per-core unfolding (Eq. 6/7/8), then assembles R(lambda) via Theorem 2
// (Eq. 5) explicitly -- see plan.md's Technical Approach. Throws
// std::runtime_error if plan.tasks.size() > 1 and any task's priority is
// TASK_PRIORITY_UNSET, or if a fixed point doesn't converge within the
// iteration cap.
double response_time_of_path(
    const DeploymentPlan& plan,
    const TaskInfo& task,
    const std::vector<int>& path,
    const std::vector<SubtaskInfo>& task_subtasks,
    const std::vector<ConnectionInfo>& reduced_connections);

} // namespace detail

// Corollary 1 (Eq. 2): max over task's own complete paths (reusing
// rrc::detail::enumerate_complete_paths).
double compute_wcrt(const DeploymentPlan& plan, const TaskInfo& task);

// R(tau_i) <= D_i for every task; returns ids of tasks that fail.
std::vector<int> check_schedulability(const DeploymentPlan& plan);

} // namespace rta_fonseca2016
```

Not exposed in the header (private, `rta_fonseca2016.cpp`'s own anonymous
namespace, mirroring `rrc.cpp`'s `extend_path`/`reachable_via`
precedent): the recursive `path_analysis` helper implementing Algorithm
1's two cases (base case via Eq. 7; same-core-boundary via Eq. 8 using
the per-core memo), taking and populating a
`std::unordered_map<int, {double r; double s_ub;}>` passed by reference,
plus whatever small context (plan/task/self-set/hp-task-WCRTs) it needs —
kept private since its signature is an implementation detail, not
something another module should call directly (unlike `rrc.hpp`'s own
`response_time_of_path`, which — note — is NOT the same granularity:
RRC's version computes one path's `R` directly via a flat fixed point;
this paper's `response_time_of_path` wraps a whole recursive tree).

## Data Model

No struct field additions/removals — reuses `TaskInfo::priority`,
`SubtaskInfo::core`/`wcet_us`/`period_us`/`deadline_us` exactly as
`rrc-analysis` already does.

## Implementation Phases

1. **`group_execution_regions`** — tested against the worked example's
   `λ1` (no merge, 3 regions) and `λ2` (full merge, 1 region of WCET 4).
2. **`p_workload` + `self_interference_set`** (Lemma 1) — tested against
   the worked example's `self(λ1)={W4}`, `self(λ2)=∅`, and the documented
   pessimism case (`W4` included despite being "sandwiched" in precedence).
3. **`virtual_tasks_for_core`** (Theorem 3) — tested against `τ_HIGH`'s
   `H1`/`H2` on core0 producing `τ_{HIGH,H1}(C=2,J=3,T=100)` and
   `τ_{HIGH,H2}(C=1,J=4,T=100)`, given `R(τ_HIGH)=5`.
4. **Recursive `path_analysis` + `response_time_of_path`** (Eq. 6-8,
   Theorem 2) — tested against the full worked example: `R(λ1)=10`
   (exercising the 3-core recursion, both algorithm cases, Theorem 3),
   `R(λ2)=7` (exercising merge + single-core trivial case), plus the
   missing-priority and iteration-cap throws (same engineered-case
   technique `rrc-analysis` already used, re-verified numerically before
   use given that session's own floating-point lesson).
5. **`compute_wcrt` + `check_schedulability`** — tested against
   `R(τ_HIGH)=5`, `R(τ_LOW)=10`, both schedulable; an engineered
   unschedulable case (shrink a deadline below its computed `R`).
6. **Full verification** — `make clean && make test`.

One commit per phase (6 commits).

## Risks and Mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| The "sum over distinct middle cores" generalization of `S^{p,ub}` (Key Decisions) is this feature's own reasoned completion, not literally spelled out by Algorithm 1's pseudocode for the multi-distinct-core-middle case | Could diverge from what the paper's own (unshown) general-case handling intends in some topology this feature's worked example doesn't cover | The 3-core worked example validates the single-remote-core case exactly; flagged explicitly in Key Decisions and this file rather than asserted as unambiguous — if a future topology's result looks suspicious, re-derive by hand the way this plan.md's worked example was, before trusting it |
| Execution-region merging interacts with `self_interference_set`, which operates on the *original* (unmerged) path/subtask ids | A subtle bug could merge regions for the recursion while still computing `self()` correctly against unmerged ids, or vice versa, and the two could drift apart silently | `self_interference_set`'s own tests (Phase 2) run against the unmerged `path` explicitly; `response_time_of_path`'s own tests (Phase 4) check the *merged* recursion's final numbers, so a drift between the two would show up as a wrong final `R`, not silently pass |
| Two different "self-interference" concepts (this paper's vs. RRC's) living in sibling modules under the same function name | A future reader skims one and assumes it matches the other's semantics | Documented prominently in three places already: `spec.md`'s Overview, this file's Key Decisions, and (per RULES.md §10) the Doxygen comment on each `self_interference_set` itself |

# ERU Core Allocator — Technical Plan

## Technical Approach

Splits today's header-only `src/allocator.hpp` (which currently holds all
of WF+DRU inline) into three header+impl pairs, following this codebase's
existing `.hpp`/`.cpp` convention (`dag.hpp`/`dag.cpp`,
`parser_json.hpp`/`parser_json.cpp`) instead of the header-only style
`allocator.hpp` happens to use today:

- **`src/allocator.hpp` + `src/allocator.cpp`** — the shared layer and the
  public dispatcher. Keeps `allocator::detail::utilization` and
  `allocator::detail::build_plan_dag` exactly where they are today (both
  strategies need them, unchanged); `allocator::apply_auto_allocation`
  becomes a small dispatcher on `plan.allocation.strategy` instead of
  WF+DRU's own entry point.
- **`src/dru.hpp` + `src/dru.cpp`** — today's `decreasing_remaining_utilization_order`
  and `worst_fit_place`, moved verbatim (bodies unchanged) into `namespace
  dru::detail`, behind a new `dru::apply_wf_dru_allocation` (today's
  `apply_auto_allocation` body, validation included). Includes
  `allocator.hpp` for the shared helpers.
- **`src/eru.hpp` + `src/eru.cpp`** — new. `eru::detail::equilibrium_remaining_utilization_place`
  implements Algorithm 2; `eru::apply_eru_allocation` is the whole-plan
  entry point (one group = every `CORE_UNASSIGNED` subtask in the plan).
  Includes `allocator.hpp` for the shared helpers.

This mirrors the user's own framing exactly: `allocator.cpp` is the "main"
that ties `dru.cpp` and `eru.cpp` together, and owns only what both
strategies share.

## Key Decisions

| Decision | Choice | Rationale | Needs sign-off? |
|---|---|---|---|
| Where shared helpers live | `utilization()`/`build_plan_dag()` stay in `allocator::detail`; `dru.hpp`/`eru.hpp` `#include "allocator.hpp"` | Avoids a third new file for two three/six-line functions (would repeat `wf-dru-allocator/plan.md`'s own "premature abstraction" call for a *worse* reason here, since the functions already exist and just need to stay put) while still matching "allocator ties the two together" | No — direct implication of this feature's own framing |
| Namespace rename | `allocator::detail::decreasing_remaining_utilization_order`/`worst_fit_place` → `dru::detail::...` (bodies unchanged) | These are WF+DRU-specific; `eru::detail` needs its own sibling namespace, so `allocator::detail` should only keep what's genuinely shared | Flagged — renames a public-ish test-facing symbol, confirm |
| μ-selection (θ) implementation | A single stable sort of core indices by (descending remaining capacity, ascending index), instead of literally replicating Algorithm 2 lines 8–11's iterative argmax-and-remove loop | Identical result (top-μ by remaining capacity, ties lowest-index) to the paper's loop, but O(m log m) instead of O(m²) and far less error-prone in C++; the loop's *purpose* (rank the μ largest) is what's traced to the paper, not its literal iteration mechanics | Flagged — not previously grilled |
| Largest-WCET selection (line 13) implementation | A single stable sort of the group's subtask ids by descending WCET upfront, instead of repeating Algorithm 2's per-iteration argmax over `C̄` | Same equivalence argument as above; WCETs don't change during the loop (unlike `Û`, which must stay a live per-iteration argmax) | Flagged — not previously grilled |
| State threading (`Ū` across repeated ERU calls) | No explicit `Ū` parameter — `equilibrium_remaining_utilization_place` recomputes each core's remaining capacity by scanning the plan's *current* `SubtaskInfo::core` assignments at the start of every call, exactly like `worst_fit_place` already does | Makes the function correct to call once (this feature) or many times in sequence (future `tdta-allocator`, which calls it once per group) with zero extra plumbing — the plan itself *is* the paper's `Ū`, already updated in place by the previous call | No — direct consequence of reusing `worst_fit_place`'s existing pattern |
| Infeasibility guard | `std::runtime_error` if placing a subtask would drive its assigned core's remaining capacity negative past `1e-9`, mirroring `worst_fit_place` | Algorithm 2 itself has no such check (assumes TDTA already guaranteed a fit); this codebase's existing convention treats silent overcommit as a bug, not a valid outcome | Flagged — the open question left in `spec.md`, resolving it here to unblock the plan |
| `AllocationConfig` strategy comment | Add `eru` to the inline comment listing recognized `strategy` values (`src/deployment_plan.hpp`) | Keeps the doc comment honest about what's actually implemented, same as the existing comment already does for `worst_fit` | No |
| Test file split | Rename `tests/allocator_test.cpp` → `tests/dru_test.cpp` (namespace-only edits, same cases); add new `tests/eru_test.cpp` | Test file names should track the module they exercise, same as `dag.cpp`↔no dedicated test file but `parser_json.cpp`↔`parser_json_test.cpp` pattern elsewhere in `tests/` | Flagged — confirm the rename is wanted vs. keeping `allocator_test.cpp` as an umbrella file |

## Interface Contracts

`src/allocator.hpp`:
```cpp
namespace allocator {

// Dispatches on plan.allocation.strategy: "worst_fit" -> dru::apply_wf_dru_allocation,
// "eru" -> eru::apply_eru_allocation. Throws std::runtime_error otherwise.
void apply_auto_allocation(DeploymentPlan& plan);

namespace detail {
double utilization(const SubtaskInfo& st);          // unchanged
DAG build_plan_dag(const DeploymentPlan& plan);      // unchanged
} // namespace detail
} // namespace allocator
```

`src/dru.hpp` (unchanged bodies, new namespace):
```cpp
#include "allocator.hpp"
namespace dru {
void apply_wf_dru_allocation(DeploymentPlan& plan); // today's apply_auto_allocation body
namespace detail {
std::vector<int> decreasing_remaining_utilization_order(const DeploymentPlan& plan);
void worst_fit_place(DeploymentPlan& plan, const std::vector<int>& ordered_ids);
} // namespace detail
} // namespace dru
```

`src/eru.hpp`:
```cpp
#include "allocator.hpp"
namespace eru {
// Whole-plan entry point: one ERU call over every CORE_UNASSIGNED subtask.
void apply_eru_allocation(DeploymentPlan& plan);

namespace detail {
// Implements Algorithm 2 over exactly the subtasks named by group_ids
// (the paper's V̄). mu = min(num_cores, group_ids.size()) cores are
// selected once by current remaining capacity (descending, ties lowest
// index) into theta; a virtual scratch Uhat[mu], each initialized to
// per-core capacity, tracks this call's own round-robin balance
// independently of the real remaining capacity, which is read from and
// written back to the plan's current SubtaskInfo::core assignments (see
// plan.md's "state threading" decision -- no separate Ubar in/out param).
// group_ids' subtasks are placed largest-WCET-first (ties: group_ids'
// own order) onto whichever of the mu cores currently has the largest
// virtual remaining capacity (ties: lowest core index).
// Throws std::runtime_error on infeasibility (see plan.md).
void equilibrium_remaining_utilization_place(DeploymentPlan& plan,
                                              const std::vector<int>& group_ids);
} // namespace detail
} // namespace eru
```

`src/deployment_plan.hpp` — `AllocationConfig::strategy` comment gains
`| eru` alongside the existing `first_fit | best_fit | worst_fit` list.

## Data Model

No struct field additions/removals (that's `tdta-allocator`'s
`TaskInfo::priority`, out of scope here). Only file/namespace
reorganization plus one new algorithm.

## Implementation Phases

1. **File split, no behavior change** — move `decreasing_remaining_utilization_order`/
   `worst_fit_place` into `dru.hpp`/`dru.cpp` (`dru::detail`), add
   `dru::apply_wf_dru_allocation`, turn `allocator::apply_auto_allocation`
   into a one-branch dispatcher (`"worst_fit"` only for now).
   Rename `tests/allocator_test.cpp` → `tests/dru_test.cpp`, update its
   `allocator::detail::...` calls to `dru::detail::...`. Update
   `Makefile`'s `EXTRA_SRCS` (`dru_test` needs `src/dag.cpp
   src/parser_json.cpp src/dru.cpp src/allocator.cpp`). `make test` green,
   identical pass/fail set to before the split.
2. **`eru::detail::equilibrium_remaining_utilization_place`**
   (`src/eru.cpp`) — μ-selection, virtual `Û` scratch, largest-WCET-first
   placement loop, infeasibility throw. `tests/eru_test.cpp` cases: μ <
   num_cores (group smaller than core count) picks exactly the top-μ
   cores by remaining capacity; virtual-vs-real distinction (a core
   pre-loaded to low real remaining capacity still gets picked if it's
   one of the μ selected, and competes on equal virtual footing); tie-break
   on core index; tie-break on WCET (stable, original order); infeasible
   group throws.
3. **`eru::apply_eru_allocation` + dispatcher wiring** — whole-plan group
   collection, `allocator::apply_auto_allocation`'s second branch
   (`"eru"`), rejects anything else. End-to-end test on
   `plans/deployment_plan.json`-shaped input (every subtask assigned, no
   core over capacity).
4. **Full verification** — `make clean && make test`, all binaries green,
   `dru_test`/`eru_test` both pass, no regression in any other test.

Three commits: (1) the file split alone (zero behavior change, easy to
verify against the pre-split test results), (2) ERU's own placement logic
+ its tests, (3) the whole-plan entry point + dispatcher wiring + defaults
comment update.

## Risks and Mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| File-split phase silently changes WF+DRU behavior (e.g. a subtle namespace/include mistake) | Existing plans (`plans/*.json`) get different core assignments than before, undetected | Phase 1 is its own commit with zero intended behavior change; `dru_test.cpp`'s cases (moved verbatim from `allocator_test.cpp`) must pass byte-for-byte identically before phase 2 starts |
| `specs/wf-dru-allocator/spec.md`/`progress.md` still say the implementation lives in `src/allocator.hpp` | Future reader looks in the wrong file | Small addendum note in `specs/wf-dru-allocator/progress.md` pointing at the relocation, added in phase 1's commit |
| Recomputing remaining capacity from plan state on every call (vs. threading an explicit `Ū`) becomes a hidden O(n) rescan per group once `tdta-allocator` calls this many times per task | Slower than the paper's own stated complexity for large `|Vᵢ|` | Acceptable for this codebase's plan sizes (tens of subtasks, not thousands); revisit only if `tdta-allocator`'s own checkpoint shows it matters |

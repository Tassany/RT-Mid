# TDTA Core Allocator — Tasks

Test-first per RULES.md §3. Depends on `specs/eru-allocator/` being
implemented first (`eru::detail::equilibrium_remaining_utilization_place`
must exist). Plan.md's Phase 6 ("`allocate_task` + `apply_tdta_allocation`
+ dispatcher branch") is split into two commits here (Phases 6 and 7) to
stay under RULES.md §1's ~150-line guideline, so this feature lands as
**7 commits** total. No `[P]` tasks — sequential, explained one at a time
per RULES.md §1.

**Worked example used throughout** (plan.md's Technical Approach, from the
paper's Fig. 2 → Fig. 3, `Ti=100`, WCETs `C1..C7 = 3,5,2,5,3,2,2`):
edges `1→2,1→3,1→4,2→4,2→5,2→6,3→6,4→7,5→7,6→7`; IED removes `e(1,4)`
only; post-IED levels `0:{1}, 1:{2,3}, 2:{4,5,6}, 3:{7}`; `δ = [0,3,3,8,8,8,13]`
for `V1..V7`; `Str` structures `ξ_1={2,3}` (parent `1`), `ξ_2={4,5}`
(parent `2`); `6`, `7`, `1` belong to no `Str` structure.

## Status Legend
- `[ ]` Not started · `[x]` Complete · `[~]` In progress · `[C]` Checkpoint

## Phase 1: `TaskInfo::priority` + `TASK_PRIORITY_UNSET` (commit 1)

- [x] Baseline: `make clean && make test` — matches `eru-allocator`'s
      final checkpoint (`dru_test`/`eru_test`/`test_flux` pass,
      `performance_test` fails, pre-existing/unrelated).
- [x] Implement: `src/deployment_plan.hpp` — added `constexpr int
      TASK_PRIORITY_UNSET = -1;` next to `CORE_UNASSIGNED`; `TaskInfo`
      gains `int priority = TASK_PRIORITY_UNSET;`. Doxygen on both updated
      (RULES.md §10) stating this is allocation-time-only metadata,
      unrelated to `SubtaskInfo::priority`. Confirmed no positional
      `TaskInfo{...}` aggregate-init call sites exist anywhere in
      `src`/`tests`/`tools` that inserting a field would silently break
      (`grep "TaskInfo{"` — none found; every call site sets fields by
      name).
- [x] Write test: new `tests/tdta_test.cpp` (this feature's test file
      going forward) — `test_task_priority_default_unset`.
- [x] Implement: `Makefile` — `EXTRA_SRCS.tdta_test := ` (empty for now;
      comment notes it grows as later phases add `ied.cpp`/`tdta.cpp`/
      `$(ALLOCATOR_SRCS)`).
- [C] Checkpoint: `make clean && make test` — `tdta_test` (1/1) passes
      alongside `dru_test`/`eru_test`/`test_flux`; `performance_test`
      fails identically to baseline. Purely additive field, no other
      binary's behavior changed. Recorded in `progress.md`.

## Phase 2: `ied::remove_invalid_edges` (commit 2)

- [x] Baseline: Phase 1 checkpoint green.
- [x] Write test: added to `tests/tdta_test.cpp` (kept in one file per
      feature, rather than a separate `ied_test.cpp`, since `ied.hpp`
      exists solely to support this feature — matches this file's own
      header comment):
      - Worked example: the 7-subtask/10-edge graph → exactly 9 edges,
        `e(1,4)` absent, every other edge present.
      - No-invalid-edges case: plain diamond `1→2,1→3,2→4,3→4` → unchanged
        (4 edges).
      - Two-independent-removals case: `1→2,1→3,2→3,2→4,3→4` → both
        `e(1,3)` and `e(2,4)` removed, leaving the linear chain `1→2→3→4`
        — catches a recompute-reachability-mid-algorithm bug (both
        removals must use the *original* graph's reachability).
- [x] Implement: `src/ied.hpp` + `src/ied.cpp` — `ied::remove_invalid_edges`:
      builds a throwaway `DAG` from `task_subtasks`/`task_connections`,
      computes "constrains" (reachability) once via BFS from each node on
      that original graph, then for every subtask with ≥2 direct
      predecessors and every ordered predecessor pair `(a,b)` where `a`
      constrains `b`, excludes `e(a,j)` from the returned connection list.
      Doxygen comments per RULES.md §10.
- [x] Implement: `Makefile` — `EXTRA_SRCS.tdta_test := src/dag.cpp
      src/ied.cpp`.
- [C] Checkpoint: `tests/tdta_test.cpp` — all 9 assertions pass (verified
      individually). `make clean && make test` — same pass/fail set as
      Phase 1, plus IED's cases. Cross-checked against spec.md's IED
      acceptance criterion and the worked example line-by-line. Recorded
      in `progress.md`.

## Phase 3: `tdta::detail::compute_levels` (commit 3)

- [x] Baseline: Phase 2 checkpoint green.
- [x] Write test: `tests/tdta_test.cpp` — added `worked_example_subtasks()`/
      `worked_example_reduced_edges()` helpers (reused from here through
      the rest of this feature) plus:
      - Worked example: levels `{1:0}, {2:1,3:1}, {4:2,5:2,6:2}, {7:3}`
        exactly.
      - Trivial single-edge case `1→2`: `{1:0, 2:1}`.
- [x] Implement: `src/tdta.hpp` + `src/tdta.cpp` (new files, this phase
      adds `compute_levels` only) — Eq. 5 via `DAG::topological_sort()`
      (processing nodes in that order guarantees every predecessor's level
      is already known) then `1 + max` over direct predecessors' levels
      (0 for the source). Doxygen comments per RULES.md §10.
- [x] Implement: `Makefile` — `EXTRA_SRCS.tdta_test` gains `src/tdta.cpp`.
- [C] Checkpoint: `tests/tdta_test.cpp` — all 14 assertions pass
      (verified individually). `make clean && make test` — same pass/fail
      set as Phase 2, plus the level cases. Cross-checked against the
      worked example. Recorded in `progress.md`.

## Phase 4: `tdta::detail::compute_earliest_start_times` (commit 4)

- [x] Baseline: Phase 3 checkpoint green.
- [x] Write test: `tests/tdta_test.cpp` — worked example: `δ =
      [0,3,3,8,8,8,13]` for `V1..V7` exactly.
- [x] Implement: `src/tdta.cpp` — `compute_earliest_start_times`, Eq. 8
      (source: 0; else max over direct predecessors of `δ(pred) +
      WCET(pred)`), same topological-order approach as `compute_levels`.
- [C] Checkpoint: `tests/tdta_test.cpp`'s δ cases pass (verified
      individually). `make clean && make test` — same pass/fail set as
      Phase 3. Recorded in `progress.md`.

## Phase 5: `tdta::detail::find_str_structures` (commit 5)

- [x] Baseline: Phase 4 checkpoint green.
- [x] Write test: `tests/tdta_test.cpp` — worked example: exactly two
      structures, `{2,3}` then `{4,5}` (parent-id ascending order), and
      `1`/`6`/`7` in neither.
- [x] Implement: `src/tdta.cpp` — `find_str_structures`, Definition 1: for
      each subtask `p` (ascending id order), `S = {c ∈ successors(p) :
      predecessors(c) == {p}}`; `|S| ≥ 2` emits `S` as one structure.
- [C] Checkpoint: `tests/tdta_test.cpp` — all 22 assertions pass (verified
      individually). `make clean && make test` — same pass/fail set as
      Phase 4. Cross-checked against spec.md's Definition-1 acceptance
      criterion. Recorded in `progress.md`.

## Phase 6: `tdta::detail::allocate_task` (commit 6)

- [x] Baseline: Phase 5 checkpoint green; `specs/eru-allocator/` fully
      implemented and tested.
- [x] Write test: `tests/tdta_test.cpp` — single-task, worked-example
      end-to-end case, `num_cores=2`, default capacity, `Ti=100`.
- [x] Implement: `src/tdta.cpp` — `allocate_task`: slices `plan.connections`
      to this task's own edges, calls `ied::remove_invalid_edges`, then
      `compute_levels`/`compute_earliest_start_times`/`find_str_structures`
      on the result; for each level ascending, partitions that level's
      (id-sorted) members into structure groups (deduped via which
      `structures[]` index each member belongs to) plus a leftover list;
      structure groups ordered by ascending min-`δ` (`std::stable_sort`,
      ties keep `structures`' own ascending-parent-id order); each
      structure group, then the leftover group **only if non-empty**, is
      passed to `eru::detail::equilibrium_remaining_utilization_place`.
- [x] Implement: `Makefile` — `EXTRA_SRCS.tdta_test` gains
      `$(ALLOCATOR_SRCS)` (needed transitively: `eru::detail::...` calls
      `allocator::detail::utilization`, and linking `allocator.cpp` pulls
      in `dru.cpp` too, per `eru-allocator`'s own discovered Makefile
      pattern).
- [C] Checkpoint: `tests/tdta_test.cpp`'s `allocate_task` case — **passes
      exactly** the hand-traced placement (`V1→0, V2→1, V3→0, V4→0,
      V5→1, V6→1, V7→0`), confirming both `Str`-structure groups split
      across different cores and the empty level-1 leftover group is
      correctly skipped. `make clean && make test` — same pass/fail set
      as Phase 5, plus this case (29 total `tdta_test` assertions).
      Cross-checked against spec.md's Algorithm-3 acceptance criterion.
      Recorded in `progress.md`.

## Phase 7: `tdta::apply_tdta_allocation` + dispatcher wiring (commit 7)

- [x] Baseline: Phase 6 checkpoint green.
- [x] Write test: `tests/tdta_test.cpp` —
      - Two-task priority-ordering case (`TASK_LOW` priority 0, subtask
        100; `TASK_HIGHNUM` priority 1, subtask 200; 2 cores): 100 →
        core0 (placed first), 200 → core1 (core0's real remaining
        capacity had already dropped by the time 200 was placed) —
        confirms ascending order and cross-task capacity carry-over
        together, not just "some order happened".
      - Missing-priority throw (2 tasks, one unset): throws, and both
        subtasks remain `CORE_UNASSIGNED` (throw precedes any placement).
      - Single-task plan, priority unset: still allocates normally.
      - `allocator::apply_auto_allocation` with `strategy = "tdta"`:
        reaches `tdta::apply_tdta_allocation`.
- [x] Implement: `src/tdta.cpp` — `apply_tdta_allocation`: throws first
      (if `plan.tasks.size() > 1` and any `priority == TASK_PRIORITY_UNSET`),
      *then* stable-sorts task pointers by ascending `priority`, then
      calls `detail::allocate_task` once per task in that order.
- [x] Implement: `src/allocator.cpp` — added the `"tdta"` branch
      (`#include "tdta.hpp"`); `src/allocator.hpp`'s Doxygen and
      `src/deployment_plan.hpp`'s `AllocationConfig` doc/comment updated
      to mention all three strategies.
- [x] Found during verification (own test bug this time, not a
      Makefile/linking issue): the priority-order test's subtasks used
      `make_subtask(id, 900)` intending utilization 0.9, but the helper's
      default `period_us` is 100, giving `900/100 = 9.0` — infeasible,
      crashed the test binary (`terminate` on an uncaught
      `std::runtime_error`). Fixed: passed `period_us=1000` explicitly.
      Caught by actually running the test, not just reading it — exactly
      why RULES.md §3 requires running tests, not just writing them.
- [C] Checkpoint: `tests/tdta_test.cpp` — all 35 assertions pass (verified
      individually). `make clean && make test` — `tdta_test` plus every
      prior binary (`dru_test`, `eru_test`, `test_flux`) green;
      `performance_test` fails identically to every prior baseline. All
      of spec.md's Acceptance Criteria checked off line by line (RULES.md
      §4). Recorded in `progress.md`.

      Note: `Makefile`'s `ALLOCATOR_SRCS`/`EXTRA_SRCS.tdta_test` were
      updated externally (not by this session) between Phase 6 and Phase
      7 to fold `ied.cpp`/`tdta.cpp` into `ALLOCATOR_SRCS` itself —
      functionally equivalent to (and consistent with) what this phase's
      own task called for, so no rework was needed.

## Build Verification

- [x] Full test suite: `make test` — `tdta_test` (35/35, IED's cases
      folded in per Phase 2's documented deviation — no separate
      `ied_test` file) and every other `tests/*.cpp` binary exits 0 (or
      fails identically to the recorded environmental baseline, e.g.
      `performance_test`'s PREEMPT_RT limitation).
- [x] Full build: `make clean && make test` from a clean `build/`.
- [C] Final checkpoint: all seven commits' worth of changes verified
      together against spec.md's Acceptance Criteria and the worked
      example, `tasks.md` marks audited against actually-passing tests,
      `progress.md` updated, ready for the author to stage and write
      commit messages (RULES.md §1 — assistant never executes `git
      add`/`commit`).

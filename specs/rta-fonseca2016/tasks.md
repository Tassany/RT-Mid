# Fonseca et al. 2016 RTA — Tasks

Test-first per RULES.md §3. Depends on `specs/rrc-analysis/` (reuses
`enumerate_complete_paths`/`path_length`) and `specs/tdta-allocator/`
(`ied::remove_invalid_edges`/`task_connections`, `TaskInfo::priority`),
both already implemented and tested. Lands as **6 commits**. No `[P]`
tasks.

**Worked example used throughout** (plan.md's own section, hand-traced):
τ_HIGH (`priority=0`, `T=D=100`, DAG: `Hs(c9,C=1)→{H1(c0,C=2),H2(c0,C=1)}→Hm(c9,C=1)`),
`R(τ_HIGH)=5`. τ_LOW (`priority=1`, `T=D=50`:
`W1(c0,C=2,src)→{W2(c1,C=3),W4(c0,C=1)}`, `W2→W3(c0,C=1,sink)`,
`W4→W3`), `λ1={W1,W2,W3}` (3 cores, `self={W4}`, `R=10`),
`λ2={W1,W4,W3}` (merges to 1 region, `R=7`), `R(τ_LOW)=max(10,7)=10`.

## Status Legend
- `[ ]` Not started · `[x]` Complete · `[~]` In progress · `[C]` Checkpoint

## Phase 1: `group_execution_regions` (commit 1)

- [x] Baseline: `make clean && make test` — matches `rrc-analysis`'s
      final checkpoint.
- [x] Write test: new `tests/rta_fonseca2016_test.cpp` — worked-example
      helpers (`high_subtasks()`/`high_edges()`/`low_subtasks()`/
      `low_edges()`, mirroring `tests/rrc_test.cpp`'s own pattern; ids
      `Hs=1,H1=2,H2=3,Hm=4`, `W1=11,W2=12,W3=13,W4=14`) plus 8
      assertions: `λ1={11,12,13}` (cores 0,1,0) → 3 regions, WCETs
      `[2,3,1]`; `λ2={11,14,13}` (cores 0,0,0) → 1 merged region, WCET
      `4`, keeping all 3 original ids.
- [x] Implement: `src/rta_fonseca2016.hpp` + `src/rta_fonseca2016.cpp`
      (new files) — `ExecutionRegion`, `group_execution_regions`. Found
      during writing (not verification this time): the first draft's
      `group_execution_regions` took only `core_by_id` and left `wcet`
      permanently `0` — caught before writing any tests, by re-reading
      the function against its own just-written Doxygen contract.
      Fixed: signature changed to take `task_subtasks` directly (matching
      `rrc::detail::path_length`'s own convention), building both
      core/WCET lookups internally. `plan.md`'s Interface Contract
      updated to match.
- [x] Implement: `Makefile` — `EXTRA_SRCS.rta_fonseca2016_test :=
      src/dag.cpp src/rrc.cpp src/rta_fonseca2016.cpp $(ALLOCATOR_SRCS)`.
- [C] Checkpoint: all 8 assertions pass (verified individually). `make
      clean && make test` — same pass/fail set as baseline, plus the new
      binary. Recorded in `progress.md`.

## Phase 2: `p_workload` + `self_interference_set` (commit 2)

- [x] Baseline: Phase 1 checkpoint green.
- [x] Write test: 5 new assertions — `p_workload` on core0/core1;
      `self_interference_set(λ1,...) = {W4}` (comment recording the
      documented Θ pessimism); `self_interference_set(λ2,...) = ∅`.
- [x] Implement: `src/rta_fonseca2016.cpp` — `p_workload` (direct sum)
      and `self_interference_set` (Lemma 1: per-core-in-`proc(path)`
      compute `Θ` via core-restricted ancestors-of-first/descendants-of-last
      — reusing the same small duplicated-BFS pattern `rrc.cpp` and
      `ied.cpp` already established — then subtract path members and `Θ`
      from "every subtask sharing a core with the path").
- [C] Checkpoint: all 13 assertions pass (verified individually), matching
      the hand trace exactly. Cross-checked against spec.md's Lemma-1
      acceptance criterion. Recorded in `progress.md`.

## Phase 3: `virtual_tasks_for_core` (commit 3)

- [ ] Baseline: Phase 2 checkpoint green.
- [ ] Write test: `virtual_tasks_for_core(tau_HIGH, core0, R=5)` returns
      exactly `{(C=2,J=3,T=100), (C=1,J=4,T=100)}` (order matching
      `H1`,`H2`'s own ids, or confirmed as a set if order isn't
      guaranteed — decide and document when implementing).
- [ ] Implement: `src/rta_fonseca2016.cpp` — `virtual_tasks_for_core`
      (Theorem 3: filter `hp_task.subtasks` to `core`, map each to
      `{C=wcet, T=that subtask's period_us, J=r_hp_task - wcet}`).
- [C] Checkpoint: Theorem-3 case passes. Recorded in `progress.md`.

## Phase 4: Recursive `path_analysis` + `response_time_of_path` (commit 4)

- [x] Baseline: Phase 3 checkpoint green.
- [x] Write test: the full worked example (`R(λ1)=10`, `R(λ2)=7`) plus
      missing-priority and iteration-cap throws, AND one test beyond the
      original plan: neither `λ1` nor `λ2` exercises Algorithm 1's
      "different cores" branch (lines 18-28) — `λ1`'s first/last regions
      share a core, `λ2` merges into one region — so added a dedicated
      minimal case (`X(core5,C=2)→Y(core6,C=3)`, single task, no
      hp/self): `R=[2-0]+[3-0]=5`, specifically to get real coverage of
      that branch rather than leaving it untested.
- [x] Implement: `src/rta_fonseca2016.cpp` — `solve_fixed_point` (the
      Eq. 7/Eq. 8-shared fixed-point shape, factored out since both
      equations have the identical right-hand-side structure), the
      private recursive `path_analysis` helper (`void`, populates a
      per-core `{R, S_ub}` memo by side effect — base case via Eq. 7,
      same-core-boundary via Eq. 6's explicit per-distinct-core
      summation over the memoized middle + Eq. 8/Joint, different-cores
      via pure recursion per Algorithm 1 lines 18-28), and a defensive
      throw if a core would be memoized twice (the explicitly-scoped
      "at most twice per core" limit from spec.md/plan.md). Found
      mid-implementation (not verification): `path_analysis`'s return
      value was never actually used by any caller — simplified from
      returning `RegionResult` to `void`, which also removed a
      speculative `core_memo.at(core)` lookup in the different-cores
      branch that wasn't needed. `response_time_of_path`: runs the
      recursion once, then applies Theorem 2 (Eq. 5) explicitly over the
      populated per-core memo.
- [C] Checkpoint: all 10 assertions pass (verified individually) — both
      `R(λ1)=10` and `R(λ2)=7` match the hand trace exactly on the first
      run, no debugging needed. Cross-checked against spec.md's
      Algorithm-1/Theorem-2/Theorem-3 acceptance criteria line by line
      (RULES.md §4). Recorded in `progress.md`.

## Phase 5: `compute_wcrt` + `check_schedulability` (commit 5)

- [x] Baseline: Phase 4 checkpoint green.
- [x] Write test: `compute_wcrt(tau_HIGH)=5`, `compute_wcrt(tau_LOW)=10`
      (confirms `max(10,7)` picks the right path); `check_schedulability`
      on the worked example returns empty; an engineered unschedulable
      case (shrink `tau_LOW`'s deadline to `8 < 10` — returns `[2]`).
- [x] Implement: `src/rta_fonseca2016.cpp` — `compute_wcrt` (written
      alongside Phase 4's own edit, per the pattern established in
      `rrc-analysis`: max over `rrc::detail::enumerate_complete_paths`,
      calling `response_time_of_path` per path) and `check_schedulability`
      (same shape as `rrc::check_schedulability`).
- [C] Checkpoint: all 4 new assertions pass (29 total, verified
      individually), matching the hand trace exactly. Recorded in
      `progress.md`.

## Phase 6: Full verification

- [x] `make clean && make test` — `rta_fonseca2016_test` (29/29) plus
      every prior binary green; `performance_test` fails identically to
      every prior baseline (pre-existing, unrelated).
- [C] Final checkpoint: all of spec.md's Acceptance Criteria checked off
      line by line (RULES.md §4), `tasks.md` marks audited against
      actually-passing tests, `progress.md` updated, ready for the
      author to stage and write commit messages (RULES.md §1).

## Build Verification

- [x] Full test suite: `make test` — `rta_fonseca2016_test` (29/29) and
      every other `tests/*.cpp` binary exits 0 (or fails identically to
      the recorded environmental baseline).
- [x] Full build: `make clean && make test` from a clean `build/`.
- [C] Final checkpoint recorded in `progress.md`.

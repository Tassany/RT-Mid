# RRC WCRT Analysis — Tasks

Test-first per RULES.md §3. Depends on `specs/tdta-allocator/` being
implemented (it is — `ied::remove_invalid_edges`,
`tdta::detail::find_str_structures`, `TaskInfo::priority` all exist and
are tested). Lands as **7 commits**, one per phase. No `[P]` tasks.

**Worked example used throughout** (plan.md's own section, hand-traced):
τ_HIGH (`priority=0`, `T=D=100`): `V1(C=2,p0)→V2(C=4,p0)`,
`V1→V3(C=3,p1)`, `V2→V4(C=1,p1)`, `V3→V4`; Str structure `ξ_{1,1}={V2,V3}`.
τ_LOW (`priority=1`, `T=D=50`): `W1(C=2,p0)→W2(C=2,p1)→W3(C=1,p0)`, one
path. `L(λ_{2,1})=5`, `self=∅`, `Ω^{2,1}_{1,1}=3`,
`I^h_1 ≤ ⌈R/100⌉·7`, fixed point `R⁰=5→R¹=12→R²=12`, `R(τ_LOW)=12≤D=50`.

## Status Legend
- `[ ]` Not started · `[x]` Complete · `[~]` In progress · `[C]` Checkpoint

## Phase 1: `ied::task_connections` extraction (commit 1)

- [x] Baseline: `make clean && make test` — matches `tdta-allocator`'s
      final checkpoint exactly.
- [x] Implement: `src/ied.hpp`/`src/ied.cpp` — added
      `ied::task_connections(const DeploymentPlan&, const TaskInfo&)`,
      body = the slicing loop moved verbatim out of
      `tdta::detail::allocate_task`.
- [x] Implement: `src/tdta.cpp` — `allocate_task` now calls
      `ied::task_connections(plan, task)` instead of its own inline loop
      (3 lines replacing 8).
- [C] Checkpoint: `make clean && make test` — `tdta_test` 35/35
      including `allocate_task`'s worked-example case
      (`V1..V7 → [0,1,0,0,1,1,0]`) byte-for-byte unchanged; same
      pass/fail set as baseline otherwise. Recorded in `progress.md`.

## Phase 2: `enumerate_complete_paths` + `path_length` (commit 2)

- [x] Baseline: Phase 1 checkpoint green.
- [x] Write test: new `tests/rrc_test.cpp` — 8 assertions:
      τ_LOW (one path `{11,12,13}`=`{W1,W2,W3}`, `L=5`); τ_HIGH (two
      paths `{1,2,4}` `L=7` and `{1,3,4}` `L=6`).
- [x] Implement: `src/rrc.hpp`/`src/rrc.cpp` (new files) —
      `enumerate_complete_paths` (recursive DFS from the source node,
      extending along every edge to every complete source-to-sink path)
      and `path_length` (sum of `wcet_us` over the path's ids). Doxygen
      comments per RULES.md §10.
- [x] Implement: `Makefile` — `EXTRA_SRCS.rrc_test := src/dag.cpp
      src/rrc.cpp $(ALLOCATOR_SRCS)`.
- [C] Checkpoint: `tests/rrc_test.cpp` — all 8 assertions pass (verified
      individually), matching the hand trace exactly. Same pass/fail set
      as Phase 1 otherwise. Recorded in `progress.md`.

## Phase 3: `self_interference_set` + `path_self_interference` (commit 3)

- [x] Baseline: Phase 2 checkpoint green.
- [x] Write test: `tests/rrc_test.cpp` — 6 new assertions: diamond same-core
      siblings (`self(2)∋3`, `self(3)∋2`, excludes precedence-related 1/4);
      diamond different-core siblings (both empty); worked example's
      τ_LOW chain (`path_self_interference` empty).
- [x] Implement: `src/rrc.cpp` — `self_interference_set` (reachability
      both directions — descendants via successors, ancestors via
      predecessors, same small BFS helper templated on which neighbor
      list to follow — on the IED-reduced graph, plus same-core check)
      and `path_self_interference` (union + dedup, sorted, over the
      path's members).
- [C] Checkpoint: `tests/rrc_test.cpp` — all 14 assertions pass (verified
      individually). Cross-checked against spec.md's Definition-3
      acceptance criterion. Recorded in `progress.md`.

## Phase 4: `omega_sum` (commit 4)

- [x] Baseline: Phase 3 checkpoint green.
- [x] Write test: `tests/rrc_test.cpp` — worked example
      (`omega_sum(ξ_{1,1}={V2,V3}, λ_{2,1})` = exactly `3`) and an
      `η = ∅` case (path on cores `{2,3}` never overlapping the
      structure's cores `{0,1}` — contributes `0`).
- [x] Implement: `src/rrc.cpp` — `omega_sum`: for each Str structure
      (via `tdta::detail::find_str_structures`), compute `η`
      (processor-set intersection), skip if empty, else `(|η|-1) · min`
      over `η` of that structure's per-processor WCET sum.
- [x] Found during verification (own mistake, not a logic bug): wrote
      both test functions but forgot to call them from `main()` — the
      binary still printed "All checks passed" (14/14 ran fine), which
      would have silently shipped two untested test functions. Caught by
      checking the actual assertion *count* (14, not the expected 16)
      against what the test file should produce, not just trusting the
      green exit code. Fixed by adding the two missing calls.
- [C] Checkpoint: `tests/rrc_test.cpp` — all 16 assertions pass (verified
      individually, and the count itself re-checked this time), matching
      the hand trace exactly. Recorded in `progress.md`.

## Phase 5: `response_time_of_path` + `compute_wcrt` (commit 5)

- [x] Baseline: Phase 4 checkpoint green.
- [x] Write test: `tests/rrc_test.cpp` — worked example
      (`response_time_of_path` for `λ_{2,1}` = exactly `12`;
      `compute_wcrt(τ_LOW)` = `12`); missing-priority throw; iteration-cap
      throw (engineered non-convergent-but-under-deadline case).
- [x] Implement: `src/rrc.cpp` — `response_time_of_path` (the fixed-point
      loop per plan.md's Key Decision, `kMaxIterations = 10000`,
      `kEpsilon = 1e-9` for the convergence comparison) and `compute_wcrt`
      (max over `enumerate_complete_paths`).
- [x] Found during verification (test-construction bug, not a production
      bug): the first attempt at the iteration-cap test used
      `period_us = wcet_us = 1e15`, intending `R` to grow linearly
      forever without converging. Instead it hit a genuine **floating-point
      precision artifact**: at magnitudes near double's exact-integer
      limit (2^53 ≈ 9e15), `new_R` silently rounded to bit-identical `R`,
      producing a *spurious* "converged" result around iteration 10 — not
      a real fixed point of the underlying recurrence. Caught by actually
      running the test (it failed) rather than trusting the hand-derived
      arithmetic; verified the fix numerically in Python before touching
      the test file again. Rebuilt with much smaller, double-safe
      magnitudes (`period_us = wcet_us = 1e6`, `deadline_us = 2e10`) —
      confirmed in Python to run all 10000 iterations without converging
      or exceeding the deadline before changing the C++.
- [C] Checkpoint: `tests/rrc_test.cpp` — all 20 assertions pass (verified
      individually), matching the hand trace exactly. Cross-checked
      against spec.md's Eq. 1–3 acceptance criteria. Recorded in
      `progress.md`.

## Phase 6: `check_schedulability` (commit 6)

- [x] Baseline: Phase 5 checkpoint green.
- [x] Write test: `tests/rrc_test.cpp` — worked example (both tasks
      schedulable: τ_HIGH `R=7≤100`, τ_LOW `R=12≤50`, confirmed by hand
      trace too); engineered unschedulable case (shrink τ_LOW's deadline
      to `10 < 12` — returns `[2]`).
- [x] Implement: `src/rrc.cpp` — `check_schedulability` (implemented
      alongside `compute_wcrt` in Phase 5's own edit): `compute_wcrt` per
      task, compare to `deadline_us`, collect failing ids.
- [C] Checkpoint: `tests/rrc_test.cpp` — all 22 assertions pass (verified
      individually). Recorded in `progress.md`.

## Phase 7: Full verification

- [x] `Makefile` — `EXTRA_SRCS.rrc_test := src/dag.cpp src/rrc.cpp
      $(ALLOCATOR_SRCS)` (`$(ALLOCATOR_SRCS)` already brought in
      `ied.cpp`/`tdta.cpp` from the `tdta-allocator` feature's own
      Makefile work — `rrc.cpp` itself is not a dispatcher-reachable
      strategy, so it's its own explicit entry rather than folded into
      `ALLOCATOR_SRCS`, which stays scoped to `allocator::apply_auto_allocation`'s
      own dispatch branches).
- [x] `make clean && make test` — `rrc_test` (22/22) plus every prior
      binary green; `performance_test` fails identically to every prior
      baseline (pre-existing, unrelated).
- [C] Final checkpoint: all of spec.md's Acceptance Criteria checked off
      line by line (RULES.md §4), `tasks.md` marks audited against
      actually-passing tests, `progress.md` updated, ready for the
      author to stage and write commit messages (RULES.md §1).

## Build Verification

- [x] Full test suite: `make test` — `rrc_test` (22/22) and every other
      `tests/*.cpp` binary exits 0 (or fails identically to the recorded
      environmental baseline, `performance_test`'s PREEMPT_RT limitation).
- [x] Full build: `make clean && make test` from a clean `build/`.
- [C] Final checkpoint recorded in `progress.md`.

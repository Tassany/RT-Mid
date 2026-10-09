# TDTA Core Allocator — Progress

## 2026-09-30

### CP: Phase 1 TaskInfo::priority + TASK_PRIORITY_UNSET — 2026-09-30
tests: tdta_test 1/1 pass, dru_test 19/19 pass, eru_test 12/12 pass, test_flux pass, performance_test fails (pre-existing, unrelated)
build: pass (`make clean && make test`)
done: Phase 1 (`TaskInfo::priority`, `TASK_PRIORITY_UNSET`, `tests/tdta_test.cpp` created)
rework: none
criteria_met: spec.md's `TaskInfo` gains a `priority` field acceptance criterion (the field itself; ordering/throw behavior is Phase 7)
issues: none

Not yet staged/committed (RULES.md §1). This phase's diff:
`src/deployment_plan.hpp` (`TASK_PRIORITY_UNSET`, `TaskInfo::priority`),
new `tests/tdta_test.cpp`, `Makefile`'s `EXTRA_SRCS.tdta_test`.

### CP: Phase 2 ied::remove_invalid_edges — 2026-09-30
tests: tdta_test 9/9 pass, dru_test 19/19 pass, eru_test 12/12 pass, test_flux pass, performance_test fails (pre-existing, unrelated)
build: pass (`make clean && make test`)
done: Phase 2 (`ied::remove_invalid_edges`, `src/ied.hpp`/`src/ied.cpp`)
rework: none
criteria_met: spec.md's IED acceptance criterion, verified against the paper's own worked example (Fig. 2 -> Fig. 3) plus two hand-built edge cases (no-op, two independent removals)
issues: none. Deviated from tasks.md's plan of a separate `tests/ied_test.cpp` — kept everything in `tests/tdta_test.cpp` instead, since `ied.hpp` exists solely in support of this one feature (no other consumer), matching this file's own stated scope.

Not yet staged/committed (RULES.md §1). This phase's diff: new
`src/ied.hpp`/`src/ied.cpp`, `tests/tdta_test.cpp`'s IED cases,
`Makefile`'s `EXTRA_SRCS.tdta_test`.

### CP: Phase 3 tdta::detail::compute_levels — 2026-09-30
tests: tdta_test 14/14 pass, dru_test 19/19 pass, eru_test 12/12 pass, test_flux pass, performance_test fails (pre-existing, unrelated)
build: pass (`make clean && make test`)
done: Phase 3 (`compute_levels`, `src/tdta.hpp`/`src/tdta.cpp` created, `worked_example_*()` test helpers)
rework: none
criteria_met: spec.md's Eq.-5 acceptance criterion, verified against the paper's own worked example (Fig. 3's 4 levels) plus a trivial case
issues: none

Not yet staged/committed (RULES.md §1). This phase's diff: new
`src/tdta.hpp`/`src/tdta.cpp`, `tests/tdta_test.cpp`'s level cases +
worked-example helpers, `Makefile`'s `EXTRA_SRCS.tdta_test`.

### CP: Phase 4 tdta::detail::compute_earliest_start_times — 2026-09-30
tests: tdta_test 18/18 pass, dru_test 19/19 pass, eru_test 12/12 pass, test_flux pass, performance_test fails (pre-existing, unrelated)
build: pass (`make clean && make test`)
done: Phase 4 (`compute_earliest_start_times`, Eq. 8)
rework: none
criteria_met: spec.md's Eq.-8 acceptance criterion, verified against the worked example's hand-traced delta values
issues: none. Also noted (not this session's work): `tools/eval/allocator_comparison_eval_main.cpp`/`plot_allocator_comparison.py` gained an "eru" comparison mode externally, consistent with `eru-allocator`'s contract — left as-is.

Not yet staged/committed (RULES.md §1). This phase's diff: `src/tdta.hpp`/
`src/tdta.cpp` (`compute_earliest_start_times`), `tests/tdta_test.cpp`'s
delta cases.

### CP: Phase 5 tdta::detail::find_str_structures — 2026-09-30
tests: tdta_test 22/22 pass, dru_test 19/19 pass, eru_test 12/12 pass, test_flux pass, performance_test fails (pre-existing, unrelated)
build: pass (`make clean && make test`)
done: Phase 5 (`find_str_structures`, Definition 1)
rework: none
criteria_met: spec.md's Definition-1 acceptance criterion, verified against the worked example's xi_1={2,3}/xi_2={4,5} and the non-membership of 1/6/7
issues: none

Not yet staged/committed (RULES.md §1). This phase's diff: `src/tdta.hpp`/
`src/tdta.cpp` (`find_str_structures`), `tests/tdta_test.cpp`'s
Str-detection cases.

### CP: Phase 6 tdta::detail::allocate_task — 2026-09-30
tests: tdta_test 29/29 pass, dru_test 19/19 pass, eru_test 12/12 pass, test_flux pass, performance_test fails (pre-existing, unrelated)
build: pass (`make clean && make test`)
done: Phase 6 (`allocate_task` -- full Algorithm 3 for one task)
rework: none
criteria_met: spec.md's Algorithm-3 acceptance criterion -- the end-to-end worked-example placement (V1..V7 -> [0,1,0,0,1,1,0]) matches the hand trace in tasks.md EXACTLY, first try
issues: none

Not yet staged/committed (RULES.md §1). This phase's diff: `src/tdta.hpp`/
`src/tdta.cpp` (`allocate_task`), `tests/tdta_test.cpp`'s end-to-end case
+ `worked_example_original_edges()` helper, `Makefile`'s
`EXTRA_SRCS.tdta_test` (`$(ALLOCATOR_SRCS)` added).

### CP: Phase 7 apply_tdta_allocation + dispatcher wiring — 2026-09-30
tests: tdta_test 35/35 pass, dru_test 19/19 pass, eru_test 12/12 pass, test_flux pass, performance_test fails (pre-existing, unrelated)
build: pass (`make clean && make test`)
done: Phase 7 (`apply_tdta_allocation`, `allocator.cpp`'s `"tdta"` branch, doc updates)
rework: none
criteria_met: all of spec.md's Acceptance Criteria -- TDTA is now a fully working, dispatcher-reachable `plan.allocation.strategy`
issues: own test-construction bug (not a design/implementation bug) -- `make_subtask(id, 900)` with the helper's default `period_us=100` gave utilization 9.0 instead of the intended 0.9, crashing the test binary on an uncaught infeasibility exception. Fixed by passing `period_us=1000` explicitly. Caught by running the test, not just reading it.

Not yet staged/committed (RULES.md §1). This phase's diff: `src/tdta.cpp`
(`apply_tdta_allocation`), `src/allocator.cpp`'s `"tdta"` branch,
`src/allocator.hpp`/`src/deployment_plan.hpp` doc updates,
`tests/tdta_test.cpp`'s Phase 7 cases.

## tdta-allocator feature complete

All 7 commits' worth of work verified together (`make clean && make
test`): `tdta_test` 35/35, `dru_test` 19/19, `eru_test` 12/12, `test_flux`
pass; `performance_test` fails identically to every prior baseline
(pre-existing, unrelated). Every spec.md Acceptance Criterion and
tasks.md checkpoint satisfied — including the end-to-end worked-example
placement matching the hand trace exactly on the first attempt (Phase 6).

Two things worth carrying into any future work on this codebase:
1. Any binary linking `allocator.cpp` transitively needs every strategy's
   own `.cpp` (`ALLOCATOR_SRCS` in the `Makefile` now bundles all four:
   `allocator.cpp dru.cpp eru.cpp ied.cpp tdta.cpp`) — discovered three
   times across `eru-allocator`/`tdta-allocator`, now centralized so a
   future strategy is a one-line change.
2. This codebase's `TaskInfo` now carries two independent priority
   concepts on purpose: `SubtaskInfo::priority` (runtime, `TeamManager`
   Dispatcher grouping) and `TaskInfo::priority` (allocation-time only,
   `tdta`'s task-ordering rule) — see `specs/tdta-allocator/spec.md`'s
   "Allocation unit vs. priority unit" note if this resurfaces confusion.

Ready for the author to stage and write commit messages (RULES.md §1) —
recommended 7-commit split per plan.md/tasks.md, mirroring
`eru-allocator`'s own precedent.

next_session: none pending for this feature. Natural follow-ups (not
started, not speced): an `allocator-comparison-eval`-style chart adding
`tdta` as a third comparison mode (the `eru` mode was already added
externally to that tool, so `tdta` would follow the same pattern); or
starting the WCRT/schedulability-analysis feature spec.md's Non-Goals
explicitly deferred (RRC algorithm, Eq. 1-4, Theorems 1-3).

## 2026-10-01 — post-completion fix: pre-assigned subtasks silently reassigned

### CP: Respect CORE_UNASSIGNED in allocate_task — 2026-10-01
tests: tdta_test 39/39 pass (new case added), dru_test/eru_test/test_flux pass, performance_test fails (pre-existing, unrelated)
build: pass (`make clean && make test`)
done: fixed `tdta::detail::allocate_task` (`src/tdta.cpp`) to skip any subtask with `core != CORE_UNASSIGNED` when forming the `V̄` groups passed to ERU, instead of blindly re-placing every subtask of the task (including already-pinned ones)
rework: none beyond the fix itself
criteria_met: new spec.md acceptance criterion ("a subtask with core != CORE_UNASSIGNED on entry is never reassigned")
issues: found while building a hand-worked example for the author (a small single-task plan with one pre-pinned subtask, for a paper figure) — `allocate_task` reassigned the pinned subtask anyway, silently violating `allocator::apply_auto_allocation`'s own documented contract. `dru`/`eru` already filtered to `CORE_UNASSIGNED` subtasks before placing; `tdta` never did, because every test and real usage so far (the worked example, the interference-eval scenarios) happened to start with *every* subtask unassigned, so the gap was invisible until a plan mixed pinned and unassigned subtasks in the same task.

Test added: `tests/tdta_test.cpp::test_allocate_task_respects_pre_assigned_subtasks`
(`Ls->{L0,L1}->Lm`, `Ls` pre-pinned to core2, 4 cores — confirms `Ls`
stays put, the other three get placed considering core2's reduced
capacity, and the level-0 group — now empty after filtering — is skipped
rather than passed to ERU as a no-op call).

Not yet staged/committed (RULES.md §1). Diff: `src/tdta.cpp` (the fix),
`tests/tdta_test.cpp` (new test), `specs/tdta-allocator/spec.md` (new
acceptance criterion), this file.

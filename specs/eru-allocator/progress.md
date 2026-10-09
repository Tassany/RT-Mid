# ERU Core Allocator — Progress

## 2026-09-30

### CP: Phase 1 File split — 2026-09-30
tests: dru_test 19/19 pass, test_flux pass, performance_test fails (pre-existing, no PREEMPT_RT kernel — unrelated)
build: pass (`make clean && make test`, plus a standalone link check of `tools/eval/allocator_comparison_eval_main.cpp`)
done: Phase 1 (file split, zero behavior change)
rework: none
criteria_met: spec.md's "Existing WF+DRU behavior, tests, and public call sites... are unaffected by the file split"
issues: found during verification — every binary linking `src/parser_json.cpp` needed `src/allocator.cpp`/`src/dru.cpp` added to its build line too, since WF+DRU is no longer header-only-inline. Fixed in `Makefile` (`dru_test`, `test_flux`, `performance_test`, `$(APP_BIN)`, `$(EVAL_BIN)`) and `tools/eval/plot_allocator_comparison.py`. See `tasks.md` Phase 1 for the full list.

Not yet staged/committed — per RULES.md §1, the author stages and writes
the commit message. This phase's diff: new `src/dru.hpp`/`src/dru.cpp`,
rewritten `src/allocator.hpp`, new `src/allocator.cpp`, renamed
`tests/allocator_test.cpp` → `tests/dru_test.cpp`, `Makefile` EXTRA_SRCS
updates, `tools/eval/plot_allocator_comparison.py` EXTRA_SRCS update,
`specs/wf-dru-allocator/progress.md` addendum.

### CP: Phase 2 ERU placement primitive — 2026-09-30
tests: eru_test 8/8 pass, dru_test 19/19 pass, test_flux pass, performance_test fails (pre-existing, unrelated)
build: pass (`make clean && make test`)
done: Phase 2 (`eru::detail::equilibrium_remaining_utilization_place`, `src/eru.hpp`/`src/eru.cpp`, `tests/eru_test.cpp`)
rework: none
criteria_met: spec.md's Algorithm-2 acceptance criterion (μ-selection, virtual scratch, WCET-first placement, tie-breaks, infeasibility throw) — verified against hand-traced expected outcomes, not just "no exception"
issues: found during verification — `allocator.cpp`'s dispatcher references every strategy unconditionally, so every binary linking it needs every strategy's `.cpp` linked, not just the one it uses. Fixed in `Makefile` (`EXTRA_SRCS.eru_test` gained `src/dru.cpp`); will recur for `tdta`/`ied` later.

Not yet staged/committed (RULES.md §1). This phase's diff: new
`src/eru.hpp`/`src/eru.cpp`, new `tests/eru_test.cpp`, `Makefile`
`EXTRA_SRCS.eru_test`.

### CP: Phase 3 Whole-plan entry point + dispatcher wiring — 2026-09-30
tests: eru_test 12/12 pass, dru_test 19/19 pass, test_flux pass, performance_test fails (pre-existing, unrelated)
build: pass (`make clean && make test`; standalone link check of `tools/eval/allocator_comparison_eval_main.cpp` against the new `ALLOCATOR_SRCS`)
done: Phase 3 (`eru::apply_eru_allocation`, `allocator.cpp`'s `"eru"` branch, `AllocationConfig` doc/comment update)
rework: none
criteria_met: all of spec.md's Acceptance Criteria — ERU is now a fully working, dispatcher-reachable `plan.allocation.strategy`
issues: found during verification — same class of issue as Phase 2 (allocator.cpp's dispatcher references every strategy unconditionally), this time breaking `dru_test` once the `"eru"` branch was added. Fixed properly this time: introduced `Makefile`'s `ALLOCATOR_SRCS` variable (`allocator.cpp dru.cpp eru.cpp`), used everywhere `allocator.cpp` is linked, so a future strategy is a one-line change. Applied the same fix to `tools/eval/plot_allocator_comparison.py`.

Not yet staged/committed (RULES.md §1). This phase's diff: `src/eru.cpp`
(`apply_eru_allocation`, already present from Phase 2's file write),
`src/allocator.cpp`'s `"eru"` branch, `src/allocator.hpp`'s Doxygen,
`src/deployment_plan.hpp`'s `AllocationConfig` doc/comment, `tests/eru_test.cpp`'s
Phase 3 cases, `Makefile`'s `ALLOCATOR_SRCS` refactor,
`tools/eval/plot_allocator_comparison.py`'s `EXTRA_SRCS`.

**eru-allocator feature complete.** All 3 commits' worth of work verified
together (`make clean && make test`): `dru_test` 19/19, `eru_test` 12/12,
`test_flux` pass; `performance_test` fails identically to every prior
baseline (pre-existing, unrelated). All spec.md Acceptance Criteria and
tasks.md checkpoints satisfied. Ready for the author to stage and write
commit messages (RULES.md §1) — recommended 3-commit split per plan.md:
(1) file split, (2) ERU primitive + its own tests, (3) whole-plan entry
point + dispatcher + Makefile `ALLOCATOR_SRCS` refactor.

next_session: `specs/tdta-allocator/` Phase 1 —
`TaskInfo::priority`/`TASK_PRIORITY_UNSET` (`src/deployment_plan.hpp`),
now unblocked since ERU is implemented and tested.

# WF+DRU Core Allocator — Progress

## 2026-09-29

- Grilled (design tree fully resolved in one round, Q1-Q8, plus one
  follow-up question on `AllocationConfig` defaults surfaced while writing
  `plan.md`). All decisions recorded in `plan.md` Key Decisions.
- spec.md / plan.md / tasks.md written.
- Implemented and verified, both phases:
  - `src/allocator.hpp`: `allocator::detail::utilization`,
    `allocator::detail::build_plan_dag`,
    `allocator::detail::decreasing_remaining_utilization_order`,
    `allocator::detail::worst_fit_place`, `allocator::apply_auto_allocation`.
    Stub removed entirely.
  - `src/deployment_plan.hpp`: `AllocationConfig` defaults changed to
    `sort_by=remaining_utilization_desc`, `weight=utilization`; Doxygen
    rewritten (103 words, RULES.md §10).
  - `plans/deployment_plan.json`: `"allocation"` block updated to declare
    the implemented WF+DRU triple explicitly.
  - `tests/allocator_test.cpp` (new): 19 assertions across both phases,
    all passing — utilization, DRU ordering (chain, fan-out/direct-vs-
    transitive, no-successors, already-assigned-but-counted, stable
    ties), Worst-Fit placement (diverges from First-Fit, pre-assigned
    capacity respected, infeasibility throws, unsupported config
    rejected), and an end-to-end smoke test against
    `plans/deployment_plan.json`.
  - Found during verification (not originally planned):
    `tests/plans/deployment_plan_test.json` also declared
    `sort_by: "none"`, which the new hard-error path rejects. That
    fixture's total utilization (1.08) exceeds one core's capacity
    regardless, and its test (`tests/test_flux.cpp`) exists to exercise
    `TeamManager`/`Dispatcher` sharing one core, not the allocator — so
    its subtasks are now explicitly pinned to `"core": 0` in the plan
    instead of relying on auto-allocation, and the now-unused
    `"allocation"` block was removed from that fixture.
    `tests/test_flux.cpp`'s comment explaining the single-Dispatcher
    assertion was updated to match.
  - `docs/backlog/implement-wf-dru-allocator.md` and
    `docs/backlog/allocator-correctness-checker.md` (now unblocked)
    statuses updated; `docs/backlog/README.md` index updated.
- Verification: `make clean && make test` — `allocator_test` (19/19) and
  `test_flux` pass. `performance_test` fails, but identically to the
  pre-modification baseline (no PREEMPT_RT kernel/`sudo` in this sandbox —
  confirmed pre-existing and unrelated to this change, not a regression).
- All spec.md Acceptance Criteria and tasks.md checkpoints satisfied.
  Not yet staged/committed — per RULES.md §1, the author stages and
  writes the commit messages. Recommended split (grill Q8): commit 1 =
  Phase 1 hunk of `src/allocator.hpp` + `tests/allocator_test.cpp`'s
  phase-1 test functions; commit 2 = everything else. Since both phases
  were written into `src/allocator.hpp` as one file in this session,
  splitting at commit time needs `git add -p` (or the author may prefer
  landing it as a single commit — their call).

## 2026-09-30 — relocated (specs/eru-allocator/)

- This implementation moved from `src/allocator.hpp` (header-only) to
  `src/dru.hpp`/`src/dru.cpp` (`namespace dru`), unchanged in behavior —
  part of `specs/eru-allocator/`'s file split so a second strategy (ERU)
  can live alongside WF+DRU. `allocator::detail::utilization`/
  `build_plan_dag` and the public `allocator::apply_auto_allocation`
  entry point stayed in `src/allocator.hpp`/`allocator.cpp`, which is now
  a small dispatcher rather than WF+DRU's own home. `tests/allocator_test.cpp`
  was renamed to `tests/dru_test.cpp` (namespace references updated,
  cases unchanged). See `specs/eru-allocator/plan.md`/`progress.md` for
  that change's own record.

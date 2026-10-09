# WF+DRU Core Allocator — Tasks

New functionality, not a refactor — every function gets new test coverage
per RULES.md §3, in a new `tests/allocator_test.cpp` (plain `expect()`
assertions, same style as `tests/performance_test.cpp`, no external test
framework).

Lands as **two commits** (grill Q8): Phase 1 alone, then Phases 2-4
together. Each stays well under RULES.md §1's ~150-line guideline.

## Status Legend
- `[ ]` Not started · `[x]` Complete · `[~]` In progress · `[C]` Checkpoint

## Phase 1: Utilization + DRU ordering (commit 1)

- [x] Baseline: `make clean && make test` on unmodified tree — recorded
      pre-existing state: `performance_test` fails on this environment
      (no PREEMPT_RT kernel/sudo — unrelated, pre-existing), `test_flux`
      passes. No allocator test existed yet.
- [x] Implement: `src/allocator.hpp` — added `allocator::detail::utilization`
      and `allocator::detail::decreasing_remaining_utilization_order`
      (see plan.md Interface Contracts). Whole-plan `DAG` built the same
      way `JsonParser::validate_structure` does. Doxygen comments per
      RULES.md §10, all ≤120 words.
- [x] Write test: `tests/allocator_test.cpp` — cases for phase 1 (all
      passing):
      - Simple chain (A→B→C): A's remaining utilization = U(B); B's =
        U(C); C's = 0.
      - Fan-out (A→B, A→C): A's remaining utilization = U(B)+U(C),
        confirming *direct* successors only (add a D behind B, i.e.
        A→B→D, and confirm D's utilization is NOT counted in A's
        remaining utilization — this is the direct-vs-transitive check).
      - Node with no successors: remaining utilization 0.
      - Ordering: given three subtasks with distinct remaining
        utilizations, confirms descending order; given two with equal
        remaining utilization, confirms stable tie-break (original plan
        order preserved).
      - Already-assigned subtasks (`core != CORE_UNASSIGNED`) are
        excluded from the returned ordered list but still contribute
        their utilization when they are a successor of an unassigned one.
- [x] Checkpoint: `tests/allocator_test.cpp` built standalone per its own
      `g++` header-comment line, all phase-1 assertions pass. Cross-checked
      against spec.md Acceptance Criteria items 1-3 line by line
      (RULES.md §4) — matches. Recorded in `progress.md`. (Full-suite
      `make test` run together with phase 2 below, since both phases
      landed in the same working-tree pass before the author stages
      commits.)

## Phase 2: Worst-Fit placement + apply_auto_allocation (commit 2)

- [x] Baseline: phase 1 green.
- [x] Implement: `src/allocator.hpp` — added `allocator::detail::worst_fit_place`
      and rewrote `allocator::apply_auto_allocation` to validate
      `plan.allocation` (throws if not exactly
      `strategy=worst_fit`/`sort_by=remaining_utilization_desc`/
      `weight=utilization`) then calls phase 1's ordering followed by
      `worst_fit_place`. Old stub body/comment removed entirely.
- [x] Implement: `src/deployment_plan.hpp` — `AllocationConfig` default
      member initializers changed to `sort_by = "remaining_utilization_desc"`,
      `weight = "utilization"`. Doxygen comment on `AllocationConfig`
      rewritten (≤120 words) to state only this combination is implemented.
- [x] Implement: `plans/deployment_plan.json` — `"allocation"` block
      updated to `{"strategy": "worst_fit", "sort_by":
      "remaining_utilization_desc", "weight": "utilization"}`.
      `tests/plans/deployment_plan_test.json` also needed a fix (found
      during verification, not originally planned): it also declared
      `sort_by: "none"`, which now throws. Since that fixture's total
      utilization (1.08) exceeds one core's capacity anyway, and its
      companion test (`tests/test_flux.cpp`) exists to exercise
      TeamManager/Dispatcher sharing one core — not the allocator — its
      subtasks are now explicitly pinned to `"core": 0` instead, and the
      now-unused `"allocation"` block was removed from that fixture.
- [x] Write test: `tests/allocator_test.cpp` — cases for phase 2 (all
      passing):
      - Two cores, three subtasks with known utilizations: confirms
        Worst-Fit picks the most-remaining-capacity core at each step,
        not just any core with room (construct a case where First-Fit and
        Worst-Fit would diverge).
      - Pre-assigned subtask's utilization is subtracted from its core's
        starting capacity before placement of unassigned ones begins
        (construct a case that would overcommit if this weren't true).
      - Infeasible plan (utilization sum exceeds `num_cores * capacity`)
        — confirms `std::runtime_error` is thrown, plan left partially
        mutated is acceptable (no rollback promised).
      - Rejection: `plan.allocation.strategy = "first_fit"` (or any
        `sort_by`/`weight` value other than the implemented combination)
        — confirms `apply_auto_allocation` throws before doing any
        placement.
      - End-to-end smoke: run `JsonParser::parse_for_codegen` (or
        `parse`) against `plans/deployment_plan.json` directly, confirm
        every subtask ends up with `core >= 0` and no core's assigned
        utilization exceeds 1.0.
- [x] Checkpoint: `tests/allocator_test.cpp` built and run, all 19
      assertions pass. `make clean && make test`: `allocator_test` and
      `test_flux` green; `performance_test` fails, but identically to the
      pre-modification baseline (no PREEMPT_RT kernel/sudo in this
      environment — unrelated to this change, confirmed by diffing
      against the Phase 1 baseline run). Cross-checked against spec.md
      Acceptance Criteria items 4-8 line by line — matches. Recorded in
      `progress.md`.

## Phase 3: Full verification

- [x] Grep sweep: `grep -rn "allocator::" src tools tests` — every call
      site accounted for: `src/parser_json.cpp` (real usage),
      `src/deployment_plan.hpp` (Doxygen references, including the
      pre-existing, still-unimplemented `pack_rta_guided` mention — out
      of scope per spec.md Non-Goals), `tests/allocator_test.cpp` (new
      tests). No stray/orphaned call sites.
- [x] `make clean && make test` — `allocator_test` and `test_flux` build
      and pass; `performance_test` fails identically to the
      pre-modification baseline (environmental, not a regression — see
      above).
- [C] Final checkpoint: all spec.md Acceptance Criteria checked off; both
      commits ready for the author to stage and write (RULES.md §1 —
      assistant never executes `git add`/`commit`).

## Build Verification

- [x] `make clean && make test`: `allocator_test` (19/19 assertions) and
      `test_flux` exit 0; `performance_test` fails on this environment for
      a pre-existing, unrelated reason (see Phase 3).
- [C] Final checkpoint recorded in `progress.md`.

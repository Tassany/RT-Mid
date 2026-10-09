# ERU Core Allocator — Tasks

Test-first per RULES.md §3. Lands as **three commits** (plan.md): (1) the
file split alone, zero behavior change; (2) ERU's placement primitive +
its tests; (3) the whole-plan entry point + dispatcher wiring + defaults
comment. No `[P]` tasks — core-contribution code, edits stay sequential
and explained one at a time per RULES.md §1.

## Status Legend
- `[ ]` Not started · `[x]` Complete · `[~]` In progress · `[C]` Checkpoint

## Phase 1: File split — `dru.hpp`/`dru.cpp`, `allocator.hpp` becomes a dispatcher (commit 1)

- [x] Baseline: `make clean && make test` on the unmodified tree —
      `allocator_test` and `test_flux` pass, `performance_test` fails
      (pre-existing, no PREEMPT_RT kernel in this environment — unrelated
      to this change).
- [x] Write test: renamed `tests/allocator_test.cpp` → `tests/dru_test.cpp`;
      changed every `allocator::detail::decreasing_remaining_utilization_order`/
      `allocator::detail::worst_fit_place` call to `dru::detail::...`;
      added `#include "dru.hpp"` alongside `"allocator.hpp"`.
      `allocator::detail::utilization` calls left as-is.
- [x] Implement: created `src/dru.hpp` + `src/dru.cpp` — moved
      `decreasing_remaining_utilization_order` and `worst_fit_place`
      bodies verbatim into `namespace dru::detail`; added
      `dru::apply_wf_dru_allocation` (today's `apply_auto_allocation`
      body, validation logic/error messages unchanged bar the
      `dru::`-prefixed error strings). `#include "allocator.hpp"` for
      `utilization()`/`build_plan_dag()`.
- [x] Implement: rewrote `src/allocator.hpp`/created `src/allocator.cpp` —
      kept `allocator::detail::utilization`/`build_plan_dag`;
      `allocator::apply_auto_allocation` is now a dispatcher: `strategy ==
      "worst_fit"` calls `dru::apply_wf_dru_allocation`, else throws (the
      `"eru"` branch is Phase 3). Doxygen comments rewritten per RULES.md
      §10 to describe only what's true as of this phase.
- [x] Implement: `Makefile` — replaced `EXTRA_SRCS.allocator_test` with
      `EXTRA_SRCS.dru_test := src/dag.cpp src/parser_json.cpp src/dru.cpp
      src/allocator.cpp`.
- [x] Found during verification (not originally planned): moving
      `apply_auto_allocation`/WF+DRU out of a header-only file into
      `.cpp` translation units means every binary that links
      `src/parser_json.cpp` (which calls `allocator::apply_auto_allocation`
      from `apply_allocation_if_needed`) now needs `src/allocator.cpp` +
      `src/dru.cpp` linked too — previously free via the header-only
      inline functions. Fixed: `Makefile`'s `EXTRA_SRCS.test_flux`,
      `EXTRA_SRCS.performance_test`, the `$(APP_BIN)` rule, and the
      `$(EVAL_BIN)` rule all gained `src/allocator.cpp src/dru.cpp`;
      `tools/eval/plot_allocator_comparison.py`'s `EXTRA_SRCS` list
      (which builds `allocator_comparison_eval_main.cpp` via subprocess,
      outside the Makefile) gained the same two files. Verified the
      eval binary still links with a standalone `g++` invocation.
- [x] Implement: `specs/wf-dru-allocator/progress.md` — addendum noting
      the implementation relocated to `src/dru.hpp`/`src/dru.cpp`.
- [C] Checkpoint: `make clean && make test` — `dru_test` (19/19
      assertions) and `test_flux` pass; `performance_test` fails
      identically to the Phase 1 baseline (same pre-existing,
      environmental reason — not a regression). Same pass/fail set as
      baseline, `dru_test` replacing `allocator_test`. Recorded in
      `progress.md`.

## Phase 2: ERU placement primitive (commit 2)

- [x] Baseline: Phase 1 checkpoint green.
- [x] Write test: new `tests/eru_test.cpp` (same `expect()`-style
      convention as `tests/dru_test.cpp`/`tests/performance_test.cpp`) —
      8 assertions across:
      - μ-selection (4 cores, distinct remaining capacities via pinned
        subtasks 0.9/0.5/0.7/0.3, group of 2): both placements land on
        `{core0, core2}` (the top-2), on two *different* cores.
      - Virtual-vs-real distinction (core0 real remaining 0.9, core1 real
        remaining 0.2): the larger subtask (0.6) lands on core0, but the
        smaller one (0.1) then lands on core1 despite core1's real
        remaining capacity (0.2) being far below core0's at that point
        (0.3) — proves placement follows the virtual scratch, not a
        real-capacity re-ranking (would be Worst-Fit, not ERU).
      - Tie-break on core index (3 fresh cores, group of 1): core0 wins.
      - Tie-break on WCET (2 fresh cores, equal-WCET group, `group_ids =
        {31, 30}`): 31 (listed first) → core0, 30 (listed second) → core1.
      - Infeasible group (1 core, subtask needs 1.5): throws.
- [x] Implement: `src/eru.hpp` + `src/eru.cpp` —
      `eru::detail::equilibrium_remaining_utilization_place`: resolves
      `num_cores`/capacity (same convention as
      `dru::detail::worst_fit_place`), computes real remaining capacity
      per core from every subtask currently assigned in the plan, selects
      θ via one stable sort (top-μ by remaining capacity, ties lowest
      index — documented as equivalent to Algorithm 2 lines 8–11's
      iterative argmax-and-remove), inits virtual `Û[μ]` to per-core
      capacity, sorts the group by descending WCET via one stable sort
      (ties: original `group_ids` order — documented as equivalent to
      line 13's per-iteration argmax), then loops placing each onto
      `argmax(Û)` (ties lowest slot index), updating `Û` and
      `SubtaskInfo::core`, throwing on infeasibility. Doxygen comments per
      RULES.md §10.
- [x] Found during verification (not originally planned): `allocator.cpp`'s
      dispatcher body references `dru::apply_wf_dru_allocation`
      unconditionally (it's compiled in regardless of which branch runs at
      runtime), so any binary linking `allocator.cpp` — including
      `eru_test`, which never calls `dru::` itself — also needs
      `src/dru.cpp` linked, or the link step fails on an undefined
      reference. Fixed: `Makefile`'s `EXTRA_SRCS.eru_test` includes
      `src/dru.cpp` too, with a comment explaining why (this will recur
      for every future strategy `allocator.cpp` dispatches to, e.g.
      `tdta`/`ied`).
- [C] Checkpoint: `tests/eru_test.cpp` — all 8 assertions pass (verified
      individually, not just the aggregate "All checks passed"). `make
      clean && make test`: `dru_test`, `eru_test`, `test_flux` pass;
      `performance_test` fails identically to the Phase 1 baseline.
      Cross-checked line-by-line against spec.md's Acceptance Criteria
      (μ-selection, virtual scratch, placement loop, output) and plan.md's
      Interface Contract — matches. Recorded in `progress.md`.

## Phase 3: Whole-plan entry point + dispatcher wiring (commit 3)

- [x] Baseline: Phase 2 checkpoint green.
- [x] Implement: `src/eru.hpp`/`src/eru.cpp` — `eru::apply_eru_allocation`
      (written alongside the detail function in Phase 2's file creation,
      ahead of its own tests — noted here rather than silently glossed
      over; its tests below were still written and run before this phase
      was marked done): collects every `CORE_UNASSIGNED` subtask id
      across `plan.tasks` in original plan order into one `group_ids`,
      calls `detail::equilibrium_remaining_utilization_place(plan,
      group_ids)`.
- [x] Implement: `src/allocator.cpp` — added the `"eru"` branch (`#include
      "eru.hpp"`); `src/allocator.hpp`'s Doxygen on `apply_auto_allocation`
      rewritten to mention both strategies.
- [x] Implement: `src/deployment_plan.hpp` — `AllocationConfig::strategy`
      inline comment gains `| eru`; the struct's own `@brief`/body rewritten
      to state `eru` doesn't consult `sort_by`/`weight` at all (those are
      `worst_fit`-specific).
- [x] Write test: `tests/eru_test.cpp` — added cases:
      - Whole-plan smoke test (3 subtasks, 2 cores, hand-built): every
        subtask ends up `core >= 0`, no core's utilization exceeds 1.0.
      - Dispatcher reaches `eru::apply_eru_allocation` for
        `strategy="eru"`.
      - Dispatcher still throws for `strategy="first_fit"`.
- [x] Found during verification (not originally planned, second instance
      of Phase 2's discovery): adding the `"eru"` branch means
      `allocator.cpp` now references **both** `dru::`/`eru::`
      unconditionally, so `dru_test` (which never calls `eru::`) broke
      until `src/eru.cpp` was linked into it too. Refactored the
      `Makefile`: introduced `ALLOCATOR_SRCS := src/allocator.cpp
      src/dru.cpp src/eru.cpp`, used everywhere `allocator.cpp` is linked
      (`dru_test`, `eru_test`, `test_flux`, `performance_test`,
      `$(APP_BIN)`, `$(EVAL_BIN)`) so a future strategy (`tdta`/`ied`) is
      a one-line change instead of N. Applied the same fix to
      `tools/eval/plot_allocator_comparison.py`'s `EXTRA_SRCS`.
- [C] Checkpoint: `tests/eru_test.cpp` — all 12 assertions pass
      (verified individually). `make clean && make test` — `dru_test`
      (19/19), `eru_test` (12/12), `test_flux` pass; `performance_test`
      fails identically to the Phase 1 baseline. Standalone `g++` link
      check of `tools/eval/allocator_comparison_eval_main.cpp` against
      the new 3-file `ALLOCATOR_SRCS` succeeds. All of spec.md's
      Acceptance Criteria checked off line by line. Recorded in
      `progress.md`.

## Build Verification

- [x] Full test suite: `make test` — `dru_test` (19/19), `eru_test`
      (12/12), `test_flux` exit 0; `performance_test` fails identically
      to the recorded environmental baseline (PREEMPT_RT limitation, not
      a regression).
- [x] Full build: `make clean && make test` from a clean `build/` — no
      stale-object false positives.
- [C] Final checkpoint: all three commits' worth of changes verified
      together, `tasks.md` marks audited against actual passing tests,
      `progress.md` updated, ready for the author to stage and write
      commit messages (RULES.md §1 — assistant never executes `git
      add`/`commit`).

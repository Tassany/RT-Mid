# Single-Core vs WF+DRU Response-Time Comparison

## Overview

A response-time comparison chart: the same fixed pipeline topology (source
→ 4 parallel intermediates → sink, the demo shape already used in
`plans/deployment_plan.json`/`tests/test_flux.cpp`), run under two core
placements — everything forced onto core 0 ("single core", the old stub's
behavior) vs the real `allocator::apply_auto_allocation` (WF+DRU) spreading
subtasks across 4 cores — swept across source frequency, plotted as average
end-to-end response time per frequency for both.

Supporting/eval tooling, not core-contribution code (RULES.md §2's
supporting-mechanism bar, not §4's stricter one) — but still elaborate under
§9 (new C++ program + new Python script, >150 lines combined, spans the
eval-tooling and analysis layers), so it gets this same spec→plan→tasks
treatment.

**Blocker found and resolved during scoping:** `tools/codegen/` was deleted
from this tree (confirmed: `git status` shows `D tools/codegen/codegen_main.cpp`,
and the directory no longer exists) — `make codegen` and the existing
`tools/eval/run_priority_benchmark.py` (which depends on it) currently
cannot run. Per the author's direction, this comparison uses a new,
hand-wired C++ program instead (same approach `tests/test_flux.cpp` and
`tests/performance_test.cpp` already use since that deletion — no codegen,
components wired directly via `rtmid::wire_component`), not a codegen
restoration.

## User Stories

- As the author, I want a chart showing WF+DRU's actual measured benefit
  over naive single-core placement, using the project's own real
  Dispatcher/TeamManager execution path (not an analytic estimate), to
  support a claim in the paper.

## Acceptance Criteria

- [ ] New `tools/eval/allocator_comparison_eval_main.cpp`: hand-wired
      (no codegen, no JSON plan file — the `DeploymentPlan` is built
      in-memory), one task, 6 subtasks (`Ts → {T0,T1,T2,T3} → Tm`) using
      `BenchSource`/`BenchIntermediate` (×4)/`BenchSink4`
      (`src/mcflow_bench_components.hpp`), workload_us = 900/1800/1800/
      1800/1800/900 (Table I's "high" column values — already proven
      representative in this codebase).
  - CLI: `allocator_comparison_eval <single_core|wf_dru> <freq_hz> [jobs]`
    (`jobs` default 100).
  - `period_us = deadline_us = round(1e6 / freq_hz)` for all 6 subtasks
    (implicit deadline, matching the paper's own assumption).
  - `single_core`: every subtask's `core` set to 0 directly (bypasses the
    allocator entirely — the old stub's literal behavior).
  - `wf_dru`: every subtask's `core` left `CORE_UNASSIGNED`;
    `plan.allocation = {worst_fit, remaining_utilization_desc,
    utilization, num_cores=4}`; `allocator::apply_auto_allocation(plan)`
    called for real.
  - Drives `jobs` periodic releases via `tm.notify(source_id)` at the
    computed period (same absolute-time `clock_nanosleep` pattern as
    `tests/performance_test.cpp`'s driver, single-threaded here — one
    task, one source).
  - Output: one CSV line per completed job to stdout
    (`job_index,release_ns,finish_ns,response_us`), summary
    (avg response time, miss ratio) to stderr — same convention as
    `tools/eval/latency_eval_main.cpp`.
- [ ] New `tools/eval/plot_allocator_comparison.py`: builds the binary
      once (subprocess + g++, same `CXXFLAGS` convention as
      `run_priority_benchmark.py`), then for each frequency in
      `50,100,150,200,250,300` Hz runs it once per mode (`single_core`,
      `wf_dru`), 100 jobs each, parses the CSV, computes each point's
      average response time, and plots both as lines (matplotlib) —
      x-axis frequency (Hz), y-axis average response time (µs or ms).
      Frequency list and jobs-per-point are CLI-overridable (defaults as
      above), per the author's request. Saves a PNG.
- [ ] No changes to `src/allocator.hpp`, `src/deployment_plan.hpp`, or any
      other core-contribution file — this only calls the existing,
      already-implemented `allocator::apply_auto_allocation`.

## Non-Goals

- Restoring `tools/codegen/` — separate, larger, explicitly out of scope
  per the author's direction.
- Deadline-miss-ratio plotting — only response time was asked for; the
  eval binary's stderr summary still reports miss ratio (cheap, same data
  already computed) but the Python script does not plot it.
- Real-time-accurate numbers from this sandbox — no `PREEMPT_RT`
  kernel/`sudo` here (same limitation already hit by
  `tests/performance_test.cpp`); this environment can only validate that
  the tool *runs end-to-end*, not that its numbers are publication-grade.
  Real numbers need the author's own machine.
- Sweeping anything other than source frequency (e.g. workload size,
  branch count) — out of scope for this request.

## Dependencies

- `src/allocator.hpp` (`allocator::apply_auto_allocation`, already
  implemented and tested — [specs/wf-dru-allocator/](../wf-dru-allocator/)).
- `src/mcflow_bench_components.hpp` (`BenchSource`/`BenchIntermediate`/
  `BenchSink4`) — reused as-is, no changes.
- `src/adapter.hpp`/`src/ring_buffer.hpp`/`src/team_manager.hpp` — reused
  exactly as `tests/performance_test.cpp` already uses them for the same
  `Ts → {T0..T3} → Tm` shape.

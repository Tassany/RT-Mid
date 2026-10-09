# Single-Core vs WF+DRU Response-Time Comparison — Plan

## Technical Approach

Reuses `tests/performance_test.cpp`'s existing wiring pattern for exactly
this topology (`wire_task`, `TaskBuffers`, driver-thread release loop) —
that file already hand-wires `Ts → {T0..T3} → Tm` with these exact
components for 3 concurrent tasks; this tool needs the same shape for
just 1 task, so the wiring code is a direct, smaller derivative of it, not
a new design.

Two new files, no changes to any existing source file:

1. `tools/eval/allocator_comparison_eval_main.cpp` — the hand-wired
   runner + driver + CSV/summary output.
2. `tools/eval/plot_allocator_comparison.py` — builds it once, runs it
   twice per frequency point (both modes), parses CSV, plots.

## Key Decisions

| Decision | Choice | Rationale |
|---|---|---|
| Topology | `Ts → {T0,T1,T2,T3} → Tm`, `BenchSource`/`BenchIntermediate`×4/`BenchSink4` | Matches the demo plan's shape; `BenchSink4`'s 4-fan-in is already exactly this shape, no new component needed |
| Workload | 900/1800/1800/1800/1800/900 µs | Table I's own "high" values — already proven representative in this codebase, not invented |
| `wf_dru` num_cores | Fixed 4 (not `hardware_concurrency()`) | Matches the topology's own fan-out width and this project's existing convention (`gen_priority_plans.py`'s Table I also uses cores 0-3); keeps the benchmark's demonstrated ceiling reproducible across machines with different core counts |
| `single_core` mode | Every subtask's `core` set to 0 directly, allocator never called | The user's chosen baseline definition: naive/no-parallelism placement, not a different heuristic |
| Plan construction | Built in-memory (`DeploymentPlan` populated directly in C++) | No JSON file, no `JsonParser` dependency needed — this is a fixed, hardcoded topology, not a user-supplied plan |
| Driver | Single thread, absolute-time `clock_nanosleep`, same realign-on-fallen-behind logic as `performance_test.cpp` (simplified: one task, so one driver, no per-driver core pinning needed since there's no cross-task contention to isolate) | Proven pattern already in this codebase; one source means no multi-driver coordination needed |
| Sweep range/jobs | 50-300Hz step 50, 100 jobs/point, both CLI-overridable | Matches the author's chosen defaults; single-core's critical path (~9000µs serial) and WF+DRU's (~3600µs, still parallel even with 6 subtasks packed onto 4 cores) predict divergence starting around ~111Hz vs ~278Hz — this range should show it clearly |
| Output format | CSV per job to stdout, summary to stderr | Same convention as `tools/eval/latency_eval_main.cpp` |

## Implementation Phases

1. `tools/eval/allocator_comparison_eval_main.cpp` — build, wire, drive,
   output. Verify standalone: run once for each mode at one frequency,
   confirm plausible response times (single_core's should be visibly
   higher than wf_dru's at the same frequency).
2. `tools/eval/plot_allocator_comparison.py` — build+run sweep, parse,
   plot, save PNG. Verify: run the full default sweep in this sandbox
   (numbers won't be real-time-accurate without `PREEMPT_RT`/`sudo`, but
   the tool must run end-to-end and produce a sane-looking chart).

Single commit — both files are new, additive, and don't touch any
existing source; nothing to split.

## Risks and Mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| Without `sudo`/RT priority (this sandbox), CFS scheduling noise could make single-core's numbers erratic or even occasionally beat wf_dru at a given point | Misleading chart if run here and trusted as final | Documented in spec.md Non-Goals; verify only that the tool runs and produces a plausible-shaped chart here, note real numbers need the author's own machine |
| `single_core` mode with all 6 subtasks sharing one Dispatcher could, under heavy CFS contention (no real SCHED_FIFO), technically deadlock or run arbitrarily slowly at high frequency | Sweep hangs | Reuse `performance_test.cpp`'s bail-out valve (stop issuing releases after N consecutive late periods) in the driver loop |

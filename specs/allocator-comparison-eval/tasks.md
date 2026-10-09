# Single-Core vs WF+DRU Response-Time Comparison — Tasks

Single commit — both files new and additive.

## Status Legend
- `[ ]` Not started · `[x]` Complete · `[~]` In progress · `[C]` Checkpoint

## Phase 1: C++ eval binary

- [x] Implemented `tools/eval/allocator_comparison_eval_main.cpp` per
      plan.md.
- [C] Checkpoint: builds cleanly (`-Wall -Wextra`, no warnings from this
      file). Ran once per mode at 100Hz/20 jobs: single_core avg 9223us,
      wf_dru avg 3997us — matches the ~9000/~3600us hand-estimate in
      plan.md.

## Phase 2: Python plotting script

- [x] Implemented `tools/eval/plot_allocator_comparison.py` per plan.md.
- [C] Checkpoint: full default sweep (50-300Hz step 50, 100 jobs/point)
      ran end-to-end in this sandbox and produced a PNG. As expected
      without a PREEMPT_RT kernel/sudo, single_core's numbers show a
      backlog cliff (the bail-out valve triggering) rather than a smooth
      curve, and wf_dru stays flat since this sweep never approaches its
      real ceiling here — real numbers need the author's own machine
      (see spec.md Non-Goals). Handed off with the run command.

# Write the concurrency ordering argument

**Status:** 🟡 Elaborate — but tied to RULES.md §5, not just the size gate

## What needs to be done

Write a explicit ordering/happens-before argument for:

- The `Dispatcher`'s queue and timer handoff between its main RT thread and
  the (now shared, per-core) idle thread.
- `CoreIdleController`'s access to a `Dispatcher`'s `queue_mutex_`/`efd_` —
  in particular the shutdown-ordering invariant that lets it touch those
  safely (TeamManager stops every `CoreIdleController` before stopping any
  `Dispatcher`).

## Why

RULES.md §5: concurrency bugs are the hardest to catch by running the
system and the most damaging to the paper's credibility if they surface
after submission. "Passed a test run" doesn't count as acceptance here —
only a written argument does.

## Relevant files

`src/dispatcher.hpp`, `src/core_idle_controller.hpp`, `src/team_manager.cpp`
(shutdown ordering).

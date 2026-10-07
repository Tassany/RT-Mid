# Lifecycle and fault-injection stress test

**Status:** 🟡 Elaborate — triggers RULES.md §9

## What needs to be done

- Repeated start/stop of `TeamManager` (hundreds of cycles), checking
  thread count and open-fd count (`/proc/<pid>/task`, `/proc/<pid>/fd`)
  before/after each cycle.
- Fault injection: send `SIGTERM` at random points in the lifecycle,
  verify the termination protocol (MCFlow Section V-A) always completes
  without hanging.

## Why

Crosses `TeamManager`, `Dispatcher`, and `CoreIdleController` — more than
one mapped layer — and is concurrency/lifecycle-sensitive (RULES.md §5).
Catches a pthread/eventfd/timerfd leak that only shows up after many runs,
not the first one. Full rationale:
[../testing-strategy.md](../testing-strategy.md) (item 6).

## Relevant files

`src/team_manager.hpp`/`.cpp`, `src/dispatcher.hpp`, `src/core_idle_controller.hpp`.

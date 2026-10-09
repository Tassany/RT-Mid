# Add runtime scheduling invariants

**Status:** 🟡 Elaborate — triggers RULES.md §9

## What needs to be done

Add assert-based invariant checks that don't exist today:

- No subtask starts before its computed `next_release_ns`.
- Every subtask has `core >= 0` and within the configured core count after
  allocation; no duplicate subtask ids.
- The DAG has no cycles and no orphan subtask (promote from "covered by one
  test file" to "breaks the build if it ever fails").
- The ring buffer never silently drops a write/read.

## Why

Crosses DAG core, allocator, and `Dispatcher` — more than one mapped layer
— and touches concurrency (RULES.md §5 applies independently of the size
gate). Full rationale: [../testing-strategy.md](../testing-strategy.md)
(item 2).

## Relevant files

`src/dag.hpp`, `src/allocator.hpp`, `src/dispatcher.hpp`, `src/ring_buffer.hpp`.

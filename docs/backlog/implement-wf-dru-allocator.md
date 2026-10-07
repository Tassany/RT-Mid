# Implement the real WF+DRU core allocator

**Status:** ⚪ Done — see [specs/wf-dru-allocator/](../../specs/wf-dru-allocator/) (spec, plan, tasks, progress)

## What needs to be done

Replace `src/allocator.hpp` (currently a stub that assigns every unassigned
subtask to core 0) with the real Worst-Fit with Decreasing Remaining
Utilisation heuristic — the paper's main scientific contribution (Section
4.5): sort subtasks by descending remaining utilization of successors,
then place each on the core with the most remaining capacity.

## Why

This is the claim the paper is built around. RULES.md §4 requires writing
the algorithm as pseudocode/a precise restatement of the paper's model
*before* touching the C++, and checking the implementation against that
spec line by line — not inferring a spec by reading generated code.

## Blocks

- [allocator-correctness-checker.md](allocator-correctness-checker.md)

## Relevant files

`src/allocator.hpp`, `src/deployment_plan.hpp` (`AllocationConfig`, already
has fields like `guided`/`validate` with `rta`/`rta_v` modes that go beyond
what's implemented — check those against the paper too before assuming
they're needed).

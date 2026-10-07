# Fix per-job sequencing in the latency eval

**Status:** 🟢 Ready

## What needs to be done

`tools/eval/latency_eval_main.cpp` currently correlates a job's release
time with its finish time by arrival order, not a real per-job sequence
number, because `adapter.hpp`'s generated pipeline uses one shared
`round_seq_` per pipeline rather than a true per-job counter. Thread a real
per-job sequence number through `wire_component`/codegen so this holds even
when a new release happens before the previous job has fully drained.

## Why

Safe today only for the demo plan (near-instant processing, multi-ms
periods) — not safe for the paper's own high-frequency sweep (Tables 3/4),
where this tool's numbers would become silently meaningless. Deliberately
scoped out when the eval tool was written; still needed before it's used
for anything beyond the demo plan.

## Relevant files

`tools/eval/latency_eval_main.cpp` (see its own header comment for the
exact failure mode), `src/adapter.hpp`, `tools/codegen/codegen_main.cpp`.

# Add a timing regression fence

**Status:** 🟢 Ready

## What needs to be done

`tools/eval/run_priority_benchmark.py` already compares RT-Mid's numbers
against MCFlow's published ones. Add an automatic fence: version the
`latency_eval` CSVs as a saved baseline, and fail (non-zero exit) if the
latest run's deadline-miss ratio drifts from that baseline by more than a
set margin (e.g. 5 percentage points) at any frequency. Compare with
median + IQR, not a plain z-score — RT jitter tends to have a long tail.

## Why

Turns the existing empirical oracle into a regression *guard* instead of
something that only gets read by a human after the fact. Full rationale:
[../testing-strategy.md](../testing-strategy.md) (item 3).

## Relevant files

`tools/eval/run_priority_benchmark.py`, `tools/eval/latency_eval_main.cpp`.

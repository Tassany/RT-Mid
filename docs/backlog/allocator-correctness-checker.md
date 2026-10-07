# Independent WF+DRU correctness checker

**Status:** 🟢 Ready to implement directly — unblocked, the real allocator now
exists ([implement-wf-dru-allocator.md](implement-wf-dru-allocator.md),
[specs/wf-dru-allocator/](../../specs/wf-dru-allocator/)).

## What needs to be done

A script that reads a deployment plan plus the allocator's output and
re-implements the paper's own placement rule independently (descending
remaining-utilization sort, most-remaining-capacity placement), then diffs
its result against what the allocator actually produced.

## Why

"Ran without crashing" isn't the same as "implements the model." An
allocator that looks plausible can still diverge from the paper's rule —
this is an oracle that isn't just re-running the same code, per RULES.md
§4's spirit. Full rationale and how this fits alongside other test/anomaly
work: [../testing-strategy.md](../testing-strategy.md) (item 4).


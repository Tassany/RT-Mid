# Add sanitizer builds to the test suite

**Status:** 🟢 Ready — escalates to 🟡 if a real finding needs a multi-layer fix

## What needs to be done

Add a `make test-sanitized` target that builds the existing test binaries
(and a repeated start/stop cycle of `TeamManager`) with `-fsanitize=thread`
and separately with `-fsanitize=address,undefined`, then runs them.

## Why

`Dispatcher`/`CoreIdleController`/the ring buffer are the highest
concurrency risk in the codebase (RULES.md §5). Sanitizers catch real data
races and memory bugs without depending on a written argument catching
everything — same tests, different class of anomaly. Full rationale:
[../testing-strategy.md](../testing-strategy.md) (item 1).

## Relevant files

`Makefile` (new target), all of `src/`.

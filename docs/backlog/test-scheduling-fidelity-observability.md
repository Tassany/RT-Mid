# Observe real scheduling fidelity

**Status:** 🟢 Ready

## What needs to be done

During a real run, use `perf sched` or read
`/proc/<pid>/task/<tid>/stat` to compare each `SCHED_FIFO` thread's actual
wake time against its expected release time.

## Why

Detects priority inversion or excess context switching directly, rather
than inferring it from deadline-miss numbers. Directly relevant to the
earlier finding about per-core vs. per-(core,priority) idle threads, now
partially addressed by `CoreIdleController` — this is how to check whether
that fix actually reduced contention. Full rationale:
[../testing-strategy.md](../testing-strategy.md) (item 7).

## Relevant files

None in-repo — external analysis tooling/scripts against a running
`rt_mid` process.

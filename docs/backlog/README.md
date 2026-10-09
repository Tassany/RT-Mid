# Backlog

One file per demand. Update a file's **Status** line in place as it moves;
this index tracks what exists, not history (git history is the record of
what changed and when).

Status legend: 🔴 Blocked · 🟡 Elaborate (RULES.md §9: grill-with-docs →
spec-driven-development) · 🟢 Ready to implement directly (still subject to
RULES.md §3, and §5 where concurrency is involved) · ⬜ Undecided · ⚪ Done

## Core contribution

- ⚪ [implement-wf-dru-allocator.md](implement-wf-dru-allocator.md) — done ([specs/wf-dru-allocator/](../../specs/wf-dru-allocator/))
- 🟢 [allocator-correctness-checker.md](allocator-correctness-checker.md)

## Architecture reconciliation

- ⚪ [reconcile-component-layer.md](reconcile-component-layer.md) — resolved: keep it (ADR-0001)

## Citation hygiene (RULES.md §6)

- 🟢 [citation-hygiene-verification.md](citation-hygiene-verification.md)

## Concurrency (RULES.md §5)

- 🟡 [concurrency-ordering-argument.md](concurrency-ordering-argument.md)

## Known limitations (eval tooling / main.cpp)

- 🟢 [eval-per-job-sequencing.md](eval-per-job-sequencing.md)
- 🟢 [aperiodic-source-triggering.md](aperiodic-source-triggering.md)

## Testing / anomaly detection

Rationale and full survey: [../testing-strategy.md](../testing-strategy.md).

- 🟢 [test-sanitizers.md](test-sanitizers.md)
- 🟡 [test-runtime-scheduling-invariants.md](test-runtime-scheduling-invariants.md)
- 🟢 [test-timing-regression-fence.md](test-timing-regression-fence.md)
- 🟢 [test-parser-fuzzing.md](test-parser-fuzzing.md)
- 🟡 [test-lifecycle-fault-injection.md](test-lifecycle-fault-injection.md)
- 🟢 [test-scheduling-fidelity-observability.md](test-scheduling-fidelity-observability.md)

(The allocator correctness checker is also a testing item — see
[allocator-correctness-checker.md](allocator-correctness-checker.md) under
Core contribution above; not duplicated here.)

## Process (done)

- ⚪ `development` branch created from unborn `main`, 13 layered commits, pushed to `origin`.
- ⚪ RULES.md §9 (grill-then-spec workflow) added.
- ⚪ `mattpocock-skills` and `spec-driven-development` plugins installed and confirmed live.
- ⚪ [Anatomia RT-Mid](https://claude.ai/artifact/Lqq2LfSZfy7g1iSE5fH3sb) architecture artifact published.

# RRC WCRT Analysis — Progress

## 2026-10-01

### CP: Phase 1 ied::task_connections extraction — 2026-10-01
tests: tdta_test 35/35 pass, dru_test 19/19 pass, eru_test 12/12 pass, test_flux pass, performance_test fails (pre-existing, unrelated)
build: pass (`make clean && make test`)
done: Phase 1 (`ied::task_connections`, `tdta.cpp` updated to use it)
rework: none
criteria_met: plan.md's extraction decision, zero behavior change confirmed
issues: none

Not yet staged/committed (RULES.md §1). This phase's diff: `src/ied.hpp`/
`src/ied.cpp` (`task_connections`), `src/tdta.cpp` (3-line simplification).

### CP: Phase 2 enumerate_complete_paths + path_length — 2026-10-01
tests: rrc_test 8/8 pass, dru_test/eru_test/tdta_test/test_flux pass, performance_test fails (pre-existing, unrelated)
build: pass (`make clean && make test`)
done: Phase 2 (`enumerate_complete_paths`, `path_length`, new `src/rrc.hpp`/`src/rrc.cpp`/`tests/rrc_test.cpp`)
rework: none
criteria_met: spec.md's path-enumeration acceptance criterion, matching the hand-traced worked example exactly
issues: none

Not yet staged/committed (RULES.md §1). This phase's diff: new
`src/rrc.hpp`/`src/rrc.cpp`, new `tests/rrc_test.cpp`, `Makefile`'s
`EXTRA_SRCS.rrc_test`.

### CP: Phase 3 self_interference_set + path_self_interference — 2026-10-01
tests: rrc_test 14/14 pass, dru_test/eru_test/tdta_test/test_flux pass, performance_test fails (pre-existing, unrelated)
build: pass (`make clean && make test`)
done: Phase 3 (`self_interference_set`, `path_self_interference`, Definition 3)
rework: none
criteria_met: spec.md's Definition-3 acceptance criterion, verified against the diamond (same/different core) and the worked example's chain
issues: none

### CP: Phase 4 omega_sum — 2026-10-01
tests: rrc_test 16/16 pass, dru_test/eru_test/tdta_test/test_flux pass, performance_test fails (pre-existing, unrelated)
build: pass (`make clean && make test`)
done: Phase 4 (`omega_sum`, Eq. 4)
rework: none
criteria_met: spec.md's Eq.-4 acceptance criterion, matching the hand-traced Omega=3 exactly, plus the eta=empty=0 case
issues: own mistake caught during verification -- two new test functions were defined but never called from main(); fixed, see tasks.md Phase 4 for how it was caught (assertion count mismatch, not just the green exit code)

### CP: Phase 5 response_time_of_path + compute_wcrt — 2026-10-01
tests: rrc_test 20/20 pass, dru_test/eru_test/tdta_test/test_flux pass, performance_test fails (pre-existing, unrelated)
build: pass (`make clean && make test`)
done: Phase 5 (`response_time_of_path`, `compute_wcrt`, Eq. 1-3 fixed point)
rework: none
criteria_met: spec.md's Eq. 1-3 acceptance criteria, matching the hand-traced R(lambda_{2,1})=12 exactly
issues: test-construction bug caught by running the test, not trusting hand arithmetic -- an engineered iteration-cap case at WCET/period=1e15 hit a genuine floating-point precision artifact near double's 2^53 exact-integer limit, producing a spurious early "convergence" instead of real non-convergent growth. Fixed with much smaller (1e6-scale), Python-verified magnitudes. Not a production-code bug -- realistic WCET/period values never approach 1e15us (~32,000 years), so this doesn't affect real plans, only the test's own engineered extreme.

Not yet staged/committed (RULES.md §1). This phase's diff: `src/rrc.hpp`/
`src/rrc.cpp` (`response_time_of_path`, `compute_wcrt`),
`tests/rrc_test.cpp`'s R/WCRT cases.

### CP: Phase 6 check_schedulability — 2026-10-01
tests: rrc_test 22/22 pass, dru_test/eru_test/tdta_test/test_flux pass, performance_test fails (pre-existing, unrelated)
build: pass (`make clean && make test`)
done: Phase 6 (`check_schedulability`, implemented in Phase 5's own edit, tested here)
rework: none
criteria_met: spec.md's schedulability-check acceptance criterion
issues: none

## rrc-analysis feature complete

All 7 phases' worth of work verified together (`make clean && make
test`): `rrc_test` 22/22, `dru_test` 19/19, `eru_test` 12/12, `tdta_test`
35/35, `test_flux` pass; `performance_test` fails identically to every
prior baseline (pre-existing, unrelated). Every spec.md Acceptance
Criterion and tasks.md checkpoint satisfied, including the full
from-scratch worked example (hand-traced in plan.md) matching the
implementation's output exactly at every stage: paths, self-interference,
Omega=3, R(lambda_{2,1})=12, compute_wcrt=12, schedulable.

Two real issues surfaced and fixed during verification (both documented
in their own phase's tasks.md entry): a test wired up but never called
from `main()` (Phase 4), and a floating-point precision artifact in an
engineered iteration-cap test case that produced spurious convergence
near double's 2^53 exact-integer limit (Phase 5) — neither affects
production code, both were caught by actually running tests rather than
trusting that writing them was enough.

This closes the loop the whole session was building toward: the question
that started `rrc-analysis` ("why does TDTA show lower response time
than WF+DRU on `interference_eval.png`?") can now be answered by
*computing* `rrc::compute_wcrt` for `τ_LOW` under each allocator's output
plan, not only by measuring it on real hardware. That comparison itself
is explicitly out of scope here (spec.md's Non-Goals) — a natural, small
follow-up once this feature is reviewed/committed.

Not yet staged/committed (RULES.md §1). Recommended 7-commit split per
plan.md/tasks.md, mirroring `tdta-allocator`'s own precedent.

### Follow-up: wcrt_eval tool — 2026-10-01
Built `tools/eval/wcrt_eval_main.cpp` + `tools/eval/plot_wcrt_eval.py`,
computing `rrc::compute_wcrt` for `interference_eval_main.cpp`'s own
HIGH/LOW plan under each mode -- no execution needed (pure calculation),
so no PREEMPT_RT dependency unlike the measured tool. Extracted the
shared plan-building logic both tools now use into
`tools/eval/interference_topology.hpp`/`.cpp` (was inline in
`interference_eval_main.cpp`) specifically so the two tools can never
silently analyze different topologies while both claiming to be about
"the interference_eval scenario" -- verified `interference_eval_main.cpp`
still behaves identically after the extraction (same numbers as before,
smoke-tested).

**Finding, worth remembering**: the theoretical bound and the earlier
*measured* result (`interference_eval.png`, where TDTA showed the lowest
average response time) do not agree in this scenario -- `wcrt_eval.png`
shows WF+DRU with the lowest/most-favorable `R(tau_LOW)` bound and the
widest schedulable frequency range, TDTA in the middle, ERU generally
highest (worst) until crossing below TDTA at 90 Hz. This is not a
contradiction: Eq. 1-4 compute a deliberately conservative *worst-case*
upper bound (Eq. 3's `ceil(R/T_j)` assumes maximal interference every
window), while the measured plot reflects *typical-case* behavior across
whatever phase alignments an actual run happened to produce. A strategy
can be better on one axis and not the other without either number being
wrong -- this is itself the kind of insight `rrc-analysis` existed to
surface, not a bug to chase.

Not yet staged/committed (RULES.md §1). New files: `tools/eval/wcrt_eval_main.cpp`,
`tools/eval/plot_wcrt_eval.py`, `tools/eval/interference_topology.hpp`/`.cpp`
(extracted); modified `tools/eval/interference_eval_main.cpp` (now
includes the shared header instead of its own copy) and
`tools/eval/plot_interference_eval.py` (`EXTRA_SRCS` gained the new
shared `.cpp`).

next_session: none pending.

# Fonseca et al. 2016 RTA — Progress

## 2026-10-01

### CP: Phase 1 group_execution_regions — 2026-10-01
tests: rta_fonseca2016_test 8/8 pass, dru_test/eru_test/rrc_test/tdta_test/test_flux pass, performance_test fails (pre-existing, unrelated)
build: pass (`make clean && make test`)
done: Phase 1 (`ExecutionRegion`, `group_execution_regions`, new `src/rta_fonseca2016.hpp`/`.cpp`/`tests/rta_fonseca2016_test.cpp`)
rework: none (one self-caught signature bug, fixed before any test ran — see tasks.md)
criteria_met: spec.md's execution-region grouping acceptance criterion, matching the hand-traced worked example exactly
issues: none

Not yet staged/committed (RULES.md §1). This phase's diff: new
`src/rta_fonseca2016.hpp`/`.cpp`, new `tests/rta_fonseca2016_test.cpp`,
`Makefile`'s `EXTRA_SRCS.rta_fonseca2016_test`, `plan.md`'s Interface
Contract corrected to match the fixed signature.

### CP: Phase 2 p_workload + self_interference_set — 2026-10-01
tests: rta_fonseca2016_test 13/13 pass, others unchanged from Phase 1's pass/fail set
build: pass (`make clean && make test`)
done: Phase 2 (`p_workload`, `self_interference_set`, Lemma 1)
rework: none
criteria_met: spec.md's Lemma-1 acceptance criterion, matching the hand-traced self(λ1)={W4}/self(λ2)=∅ exactly
issues: none

### CP: Phase 3 virtual_tasks_for_core — 2026-10-01
tests: rta_fonseca2016_test 16/16 pass, others unchanged
build: pass
done: Phase 3 (Theorem 3)
rework: none
criteria_met: spec.md's Theorem-3 acceptance criterion, matching the hand trace exactly
issues: none

### CP: Phase 4 recursive path_analysis + response_time_of_path — 2026-10-01
tests: rta_fonseca2016_test 10/10 new (26 total) pass, dru_test/eru_test/rrc_test/tdta_test/test_flux pass, performance_test fails (pre-existing, unrelated)
build: pass (`make clean && make test`)
done: Phase 4 -- the core of this feature: Algorithm 1's recursion + Theorem 2's explicit assembly
rework: none
criteria_met: spec.md's Algorithm-1/Theorem-2/Theorem-3 acceptance criteria -- R(lambda1)=10 and R(lambda2)=7 matched the hand trace EXACTLY on the first run, no debugging needed, for the most intricate algorithm built this session
issues: own simplification caught mid-implementation (not a correctness bug) -- path_analysis's return value was never used by any caller; simplified RegionResult-returning function to void, removing an unnecessary core_memo lookup along the way. Added one test beyond the original tasks.md plan: a dedicated minimal case for Algorithm 1's "different cores" branch, since neither worked-example path exercises it (lambda1 hits the same-core branch directly, lambda2 fully merges) -- without it, that branch would have shipped untested.

Not yet staged/committed (RULES.md §1). This phase's diff: `src/rta_fonseca2016.cpp` (`solve_fixed_point`, `Context`, `RegionResult`, `path_analysis`, `collect_virtual_tasks`, `response_time_of_path`), `src/rta_fonseca2016.hpp` (`response_time_of_path` declaration + scope-note Doxygen), `tests/rta_fonseca2016_test.cpp`'s Phase 4 cases.

### CP: Phase 5 compute_wcrt + check_schedulability — 2026-10-01
tests: rta_fonseca2016_test 29/29 pass, all siblings unchanged
build: pass
done: Phase 5
rework: none
criteria_met: spec.md's Corollary-1/schedulability acceptance criteria
issues: none

### CP: Phase 6 Full verification — 2026-10-01
tests: rta_fonseca2016_test 29/29, dru_test 19/19, eru_test 12/12, rrc_test 22/22, tdta_test 35/35, test_flux pass; performance_test fails identically to every prior baseline (pre-existing, unrelated)
build: pass (`make clean && make test`)
done: Phase 6 — feature complete

## rta-fonseca2016 feature complete

All 6 phases verified together. Every spec.md Acceptance Criterion and
tasks.md checkpoint satisfied, including the full from-scratch worked
example (hand-traced in plan.md, with a self-caught correction along the
way — the Theorem-2-not-Equation-1 mis-reading, recorded in plan.md's
Technical Approach) matching the implementation's output exactly on the
first run: `R(λ1)=10` (3-core recursion, both Algorithm-1 branches,
Theorem 3's DAG-higher-priority expansion, non-empty `self`), `R(λ2)=7`
(execution-region merging), plus a dedicated test added beyond the
original plan for Algorithm 1's "different cores" branch (neither
worked-example path exercised it on its own).

Two self-caught issues along the way (both documented in their own
phase's `tasks.md` entry, neither a correctness bug once fixed): Phase
1's `group_execution_regions` initially left `wcet` unpopulated (caught
by re-reading its own just-written contract, before any test ran); Phase
4's `path_analysis` returned an unused `RegionResult`, simplified to
`void`.

This project now has **three independent WCRT analyses** coexisting:
`rrc-analysis` (TDTA paper's own RRC), `rta_fonseca2016` (this feature),
and the measured baseline (`tools/eval/interference_eval_main.cpp`). A
natural follow-up (not started, not speced): extend
`tools/eval/wcrt_eval_main.cpp`/`plot_wcrt_eval.py` to also compute and
plot this paper's `R(τ_LOW)` bound alongside RRC's and the measured one,
for a genuine three-way comparison on `interference_topology.hpp`'s own
scenario.

Not yet staged/committed (RULES.md §1). Recommended 6-commit split per
plan.md/tasks.md.

### CP: Post-delivery correctness fix — 2026-10-01
tests: rta_fonseca2016_test 30/30 pass (was 29), all siblings unchanged
build: pass (`make clean && make test`)
done: fixed a real correctness gap found via `tools/eval/fonseca_wcrt_eval_main.cpp` on the actual `interference_topology.hpp` scenario (not this feature's own worked example)
rework: `response_time_of_path`'s internal memoization
criteria_met: n/a (bug fix, not a new acceptance criterion)
issues: TDTA's allocation of the HIGH task produced a path with an
  ALTERNATING core pattern (core0,core1,core0,core1 -- e.g.
  `Ts(c0)->A(c1)->A2(c0)->Tm(c1)`), which the original per-core-keyed
  memo (`unordered_map<int, RegionResult>`) couldn't represent: core0
  gets resolved over range [Ts,A2] (using A's own resolution as its
  remote suspension source) while core1 is ALSO resolved over range
  [A,Tm] (using A2's resolution as ITS remote source) -- two genuinely
  different, non-overlapping resolutions that happen to share the same
  core id, which a core-keyed map collides on. The implementation's own
  defensive check ("core appears more than twice") caught this correctly
  rather than silently computing a wrong number -- exactly what that
  guard was for -- but the underlying *scope* restriction was tighter
  than necessary. **Fixed properly, not worked around**: re-keyed the
  memo by `(first,last)` region-index range (`std::map<std::pair<int,int>,
  RegionResult>`), matching the paper's own RTs-matrix design (indexed
  by subtask pairs) instead of my earlier simplification (indexed by
  core alone, which plan.md had already flagged as "a deliberate
  simplification... confirm the equivalence argument is convincing" —
  it wasn't, for this pattern). Verified against a from-scratch
  hand-derivation before touching the code (X(c0,C=2)->Y(c1,C=3)->
  Z(c0,C=1)->W(c1,C=4), no hp/self: with zero external interference,
  R must equal len(path)=10 exactly -- confirmed both by hand and in
  Python before adding it as `test_response_time_of_path_alternating_cores`).
  `rta_fonseca2016.hpp`'s Doxygen updated to drop the now-stale
  "at most twice" scope note.

This phase's diff: `src/rta_fonseca2016.hpp`/`.cpp` (memo re-keyed,
`local_first_last` added, defensive throw removed since it's no longer
needed), `tests/rta_fonseca2016_test.cpp`'s new alternating-cores case,
new `tools/eval/fonseca_wcrt_eval_main.cpp` (Fonseca-only sibling of
`wcrt_eval_main.cpp`, built per the author's explicit request for
Fonseca-only results, not a combined RRC+Fonseca report).

next_session: none pending for this feature. `fonseca_wcrt_eval_main.cpp`
is built but its own plotting script (`plot_fonseca_wcrt_eval.py`,
mirroring `plot_wcrt_eval.py`) hasn't been written yet if a visual
comparison is wanted later.

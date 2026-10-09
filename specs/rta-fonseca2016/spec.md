# Fonseca et al. 2016 RTA (Self-Suspending-Task WCRT)

## Overview

Implements a second, independent WCRT analysis for this codebase's DAG
tasks — Fonseca, Nelissen, Nélis & Pinho, "Response Time Analysis of
Sporadic DAG Tasks under Partitioned Scheduling" (RTCSA 2016,
CISTER/INESC-TEC) — alongside `specs/rrc-analysis/` (the TDTA paper's own
RRC technique), not replacing it. The two were already anticipated as
siblings in this codebase before this feature existed:
`AllocationConfig::validate`/`guided` (`src/deployment_plan.hpp`) already
declare `"rta"`/`"rta_v"` as recognized-but-unimplemented values pointing
at a `src/rta_fonseca2016.hpp` that didn't exist until this feature — the
filenames below deliberately match that existing, pre-declared reference
rather than inventing a new name.

**Where this differs from RRC, conceptually:** RRC (Eq. 1–4 of the TDTA
paper) bounds interference with a single closed-form inequality per path.
This paper instead *reformulates* the problem: each path, restricted to
one core, is modeled as a classical uniprocessor **self-suspending
task** — a sequence of execution regions (where that core's own subtasks
run) separated by suspension regions (time spent waiting on subtasks of
the *same* path running on *other* cores). The WCRT of the whole path is
then assembled from the WCRTs of these per-core self-suspending tasks
(Theorem 2), each of which is solved by *any* existing uniprocessor
self-suspending-task RTA technique — the paper surveys three (Joint,
Split, MILP); this feature implements **Joint only** (simplest, closed-
form, no new external dependency — resolved during grilling, see Key
Decisions in `plan.md`).

**Self-interference here is a *different* computation than RRC's**, even
though both features have something called "self-interference" — worth
stating plainly so the two are never conflated:
- RRC (Definition 3 of the TDTA paper): a *pairwise* relation — two
  subtasks self-interfere if neither constrains the other AND they share
  a core.
- This paper (Lemma 1): a *bound on total interfering workload* per path,
  derived by taking every subtask sharing a core with the path and
  subtracting out the ones *provably* non-interfering via `Θᵢ,ₖ`
  (Eq. 4) — predecessors of the path's first subtask on a core, and
  successors of its last subtask on that core, for every core the path
  touches.

Both are legitimate upper bounds on the same real phenomenon, computed
differently, from different papers — this feature does not try to
reconcile or share code between the two self-interference computations
beyond what's genuinely identical (path enumeration, `path_length`/`len`,
which are the same quantity under both papers and already implemented in
`rrc.cpp` — reused, not reimplemented).

## Scope, resolved during grilling

- **Joint only** (Eq. 8) for the per-core self-suspending-task WCRT —
  not Split (Eq. 9) or MILP (Eq. 10–13, which would need an external
  optimization solver this codebase has no infrastructure for at all).
- **Section VI's DAG-higher-priority generalization included** (Theorem
  3) — necessary to analyze this codebase's own scenarios (e.g.
  `tools/eval/interference_topology.hpp`'s HIGH task is itself a DAG, not
  sequential), without which this feature couldn't be applied to anything
  already built this session.
- Not in scope: Split, MILP, Lemma 2/3 and Property 1 (which only refine
  Split's per-region suspension bounds — irrelevant once Split itself is
  out), the "simplest suspension-oblivious" per-region variant mentioned
  in passing (Eq. 9's own alternative framing).

## User Stories

- As the author, I want a second, independently-sourced WCRT technique
  computable on the same plans RRC already analyzes, so a claim about
  TDTA's benefit can be checked against two different papers' analyses,
  not just one.
- As the author, I want this paper's own claimed tightness advantage
  ("significantly tightens the worst-case response time... when the most
  accurate technique is chosen") checkable against RRC's bound on the
  same inputs, even knowing Joint (the simplest of the three variants
  this paper offers) gives up some of that tightness relative to Split/MILP.

## Key Notation (carried over verbatim from the paper, Sections III–VI)

- **Path** (`Definition 1`): sequence of subtasks `λᵢ,ₖ` connected by
  direct precedence constraints, source to sink. `proc(λᵢ,ₖ)`: the
  distinct cores the path touches. `v^p_a`/`v^p_z`: the path's first/last
  subtask assigned to core `p`.
- **Length** (`Definition 2`, `len`): sum of WCET over a path's subtasks
  — identical quantity to `rrc::detail::path_length`, reused as-is.
- **p-Workload** (`Definition 3`, `Wᵢᵖ`): sum of WCET over *all* of `τᵢ`'s
  subtasks assigned to core `p` (not path-restricted).
- **Inter-Task Interference** (`Definition 5`, `I_j(λᵢ,ₖ)`) and
  **Self-Interference** (`Definition 6`, `I_i(λᵢ,ₖ)`): the two
  interference contributions `Theorem 1`'s response-time equation sums.

## Core Equations (in implementation order)

1. **Theorem 1 (Eq. 1)**: `R(λᵢ,ₖ) = len(λᵢ,ₖ) + Iᵢ(λᵢ,ₖ) + Σ_{j<i} Iⱼ(λᵢ,ₖ)`.
2. **Corollary 1 (Eq. 2)**: `R(τᵢ) = maxₖ R(λᵢ,ₖ)`.
3. **Lemma 1 (Eq. 3–4)**: `Iᵢ(λᵢ,ₖ) ≤ Σ_{p∈proc(λᵢ,ₖ)} [Wᵢᵖ − len(λᵢ,ₖ)] − Σ_{v_ℓ∈Θᵢ,ₖ} C_ℓ`,
   where `Θᵢ,ₖ = ⋃_{p∈proc(λᵢ,ₖ)} pred(v^p_a, p) ∪ succ(v^p_z, p)` (direct-or-transitive
   predecessors of the path's first subtask on `p`, union direct-or-transitive
   successors of its last subtask on `p`, computed per core — these are
   `τᵢ`'s own subtasks on a shared core that are *provably* ordered
   relative to the path and therefore cannot interfere). `self(λᵢ,ₖ)`
   (named, used by Algorithm 1) is the resulting set itself: every
   subtask sharing a core with the path, excluding the path's own members
   and excluding `Θᵢ,ₖ`.
4. **Section V's self-suspending-task model** (per `p ∈ proc(λ)`): the
   path's own subtasks on core `p`, in path order, form `q` execution
   regions (maximal runs of consecutive-on-`p` subtasks — in practice,
   since each path visits a sequence of subtasks and a "region" is a
   maximal same-core run, `q` is usually 1 per core for the paths this
   codebase's topologies produce, but the model and Algorithm 1 handle
   `q > 1` too), separated by `q−1` suspension regions bounded by
   `S^{p,ub}`.
5. **Eq. 6 (`S^{p,ub}`)**: `S^{p,ub} = Σ_{o∈proc(λ),o≠p} [R(τ_ss^o) − S_ss^{o,ub}]`
   — the total suspension time `τᵖ` experiences is bounded by the summed
   WCRT (minus their own suspension) of the "remote" self-suspending
   tasks formed by `λ`'s subtasks on every *other* core. Recursive: this
   needs `R(τ_ss^o)` for other cores computed first — Algorithm 1's job.
6. **Eq. 7 (base case, a single subtask)**:
   `R(λ_ss) = C^1st + Σ_{τⱼ∈hp(λ_ss)} ⌈(R(λ_ss)+Jⱼ)/Tⱼ⌉ × Cⱼ + Σ_{v_ℓ∈self(λ_ss)} C_ℓ`.
7. **Eq. 8 (Joint — in scope)**, for a full self-suspending task `τᵖ`
   (all of `λ`'s subtasks on core `p`, `q` execution regions):
   `R(τᵖ) = Σ_{h=1}^{q} C^p_h + S^{p,ub} + Σ_{τⱼ∈hp(τᵖ)} ⌈(R(τᵖ)+Jⱼ)/Tⱼ⌉ × Cⱼ + Σ_{v_ℓ∈self(τᵖ)} C_ℓ`
   — a fixed point in `R(τᵖ)` exactly like RRC's own recurrence and
   `rrc::detail::response_time_of_path`'s existing iteration (same
   standard RTA technique, reusable pattern, not reusable code — the
   right-hand side's shape differs).
8. **Algorithm 1 (`PathAnalysis`)**: the exact recursive procedure —
   transcribed precisely in `plan.md`'s Interface Contracts/Technical
   Approach, since getting this right (not a paraphrase of it) is the
   crux of this feature's RULES.md §4 fidelity bar. Divides a path by
   core boundaries, solving leaf (single-subtask) cases via Eq. 7 and
   same-core-spanning cases via Eq. 8 (Joint, in this feature's scope),
   memoizing into the `RTs` response-time matrix, recursing so a
   self-suspending task's `S^{p,ub}` always has its cross-core
   dependencies already solved by the time it's needed.
9. **Section VI, Theorem 3 (DAG higher-priority generalization)**: a
   higher-priority *DAG* task `τⱼ` sharing core `p` with the path under
   analysis is replaced, for interference purposes, by one independent
   virtual sequential task `τⱼ,ₒ` per subtask `vₒ` of `τⱼ` assigned to
   `p`: `Cⱼ,ₒ = Cₒ`; `Dⱼ,ₒ`/`Tⱼ,ₒ` inherited from `τⱼ` itself;
   `Jⱼ,ₒ = R(τⱼ) − Cₒ` (release jitter — needs `τⱼ`'s own WCRT computed
   first, same strict-priority-order dependency `specs/rrc-analysis/`
   already established via `TaskInfo::priority`). Every `Σ_{τⱼ∈hp(...)} ⌈(R+Jⱼ)/Tⱼ⌉×Cⱼ`
   term in Eq. 7/8 becomes `Σ_{τⱼ∈hp(...)} Σ_{vₒ∈Vⱼᵖ} ⌈(R+Jⱼ,ₒ)/Tⱼ,ₒ⌉×Cⱼ,ₒ`
   under this generalization.

## Acceptance Criteria

- [ ] `len`/path enumeration reused from `rrc::detail` — no reimplementation.
- [ ] `p`-workload (`Wᵢᵖ`, Definition 3) and `Θᵢ,ₖ`/`self(λᵢ,ₖ)` (Lemma 1)
      implemented and independently testable.
- [ ] `Algorithm 1` (`PathAnalysis`) implemented faithfully to the
      transcribed pseudocode (`plan.md`), including both recursive cases
      (same-core boundary vs. different-core boundary) and the `RTs`
      memoization matrix.
- [ ] Eq. 7 (base case) and Eq. 8 (Joint) implemented as fixed-point
      iterations, reusing the iteration-cap/epsilon convention
      `rrc-analysis` already established (same class of defensive guard,
      same rationale).
- [ ] Theorem 3's DAG-higher-priority generalization: higher-priority DAG
      tasks expanded into per-subtask virtual sequential tasks with the
      derived `Cⱼ,ₒ`/`Jⱼ,ₒ` before Eq. 7/8 are evaluated.
- [ ] Top-level entry points mirroring `rrc-analysis`'s own shape:
      `R(τᵢ)` (Eq. 2) and a schedulability check — exact signatures in
      `plan.md`.
- [ ] Verified against a from-scratch hand-traced worked example
      (`plan.md`) specifically exercising: a path spanning ≥3 cores (so
      the *middle* core is genuinely suspended while the boundary cores
      execute — Algorithm 1's recursive split only shows its real
      behavior with ≥3 cores, not 2), at least one higher-priority DAG
      task (Theorem 3), and a non-empty `self(λᵢ,ₖ)`.

## Non-Goals

- Split (Eq. 9), MILP (Eq. 10–13), Lemma 2/3, Property 1 — resolved
  during grilling, see Scope above.
- Replacing or modifying `specs/rrc-analysis/` in any way — the two
  coexist; nothing in `src/rrc.*` changes.
- A three-way comparison tool (measured vs. RRC vs. this paper) for
  `tools/eval/interference_topology.hpp`'s own scenario — natural
  follow-up once this feature's own correctness is established
  (mirrors how `tools/eval/wcrt_eval_main.cpp` followed `rrc-analysis`),
  not part of it.
- Multi-source/multi-sink, cyclic-dependency validation, or any
  assumption this codebase's other DAG-handling code doesn't already make.

## Dependencies

- `specs/rrc-analysis/` (`rrc::detail::enumerate_complete_paths`,
  `path_length` — reused directly) and, transitively, `specs/tdta-allocator/`
  (`ied::remove_invalid_edges`, `TaskInfo::priority`).
- Fonseca, Nelissen, Nélis & Pinho, "Response Time Analysis of Sporadic
  DAG Tasks under Partitioned Scheduling" (RTCSA 2016) —
  `/home/tassany/Zotero/storage/VD7GCRGH/Fonseca et al. - 2016 - Response time analysis of sporadic DAG tasks under partitioned scheduling.pdf`,
  Sections III–VI (Definitions 1–6, Theorems 1–3, Corollary 1, Lemma 1,
  Eq. 1–8, Algorithm 1).
- `src/deployment_plan.hpp`'s existing (currently unimplemented)
  `AllocationConfig::validate`/`guided` `"rta"` hook, and the
  `src/rta_fonseca2016.hpp` filename it already names — both predate this
  feature and are honored by it (filenames match; the hook itself wiring
  `validate`/`guided` to actually call this analysis is a separate,
  future concern, not required for this feature to be useful via its own
  direct API the way `rrc::compute_wcrt` already is).

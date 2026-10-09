# TDTA Core Allocator

## Overview

Builds the full topology-based allocation strategy — **TDTA**, Algorithm 3
of Wu, Zhang, Guan & Ma, "TDTA: Topology-Based Real-Time DAG Task
Allocation on Identical Multiprocessor Platforms" (IEEE TPDS 34(11), 2023)
— on top of the ERU placement primitive already spec'd in
`specs/eru-allocator/` (Algorithm 2). Exposes a third
`plan.allocation.strategy` value, `"tdta"`, alongside WF+DRU (`"worst_fit"`)
and standalone ERU (`"eru"`).

Where standalone ERU treats a plan's unassigned subtasks as one flat group,
TDTA is the paper's actual namesake contribution: it analyzes each DAG
task's own topology to decide the size, composition, and order of the
groups handed to ERU, and it processes DAG tasks strictly in **task
priority order** — smallest `π(τᵢ)` first, per the paper's own convention
("the smaller value of `π(τᵢ)`, the higher priority", Sect. III) — so a
lower-priority task's placement can never distort load a higher-priority
task already established. Concretely, per task τᵢ:

1. **IED** (Sect. V, Algorithm 1) removes redundant ("invalid") precedence
   edges from τᵢ's own DAG.
2. **Levels** (Eq. 5) assign each subtask its longest-path-from-source
   distance on the IED-reduced graph.
3. **Str structures** (Definition 1) identify "fork" groups — subtasks
   that share one, and only one, common direct predecessor.
4. **Earliest start time** (Eq. 8) orders groups within a level.
5. **Algorithm 3** walks levels ascending; within each level, calls ERU
   once per Str-structure group (ordered by earliest start time) and once
   more for whatever's left over (subtasks in no Str structure).

This is the paper's own advertised result, so RULES.md §4's strictest bar
applies: every piece here (IED, level assignment, Str detection, group
ordering, the outer loop) must be traceable line-by-line to the paper's
Algorithm 1 / Eq. 5 / Definition 1 / Eq. 8 / Algorithm 3 — not to a
plausible-sounding paraphrase of them.

**Allocation unit vs. priority unit — resolved during grilling, worth
stating explicitly since it's easy to misread:** the thing TDTA *allocates*
is always the subtask (`P_i,j`, one core per subtask — see Acceptance
Criteria); the thing that has a *priority* is always the whole DAG task,
`π(τᵢ)`, per the paper's own formal model (Sect. III: `τᵢ = (Vᵢ, Eᵢ, Dᵢ,
Tᵢ, π(τᵢ))`). These are different axes, not a contradiction: `π(τᵢ)` only
decides the *order* in which tasks' subtasks get processed (highest
priority task's subtasks fully placed before the next task starts); it
never implies a task itself lands on one processor. The paper is explicit
that subtasks have no independent priority of their own — "the subtasks
within the same DAG task cannot preempt each other because they have the
same priority" (Sect. III) — a subtask's priority, where the paper's WCRT
analysis needs one (e.g. `hp(τᵢ)` in Eq. 2), is always its parent task's
`π(τᵢ)`, inherited, not a separate value.

**Paper errata, carried forward and new:**
- Algorithm 2's pseudocode box is captioned "The IED Method" (a copy-paste
  of Algorithm 1's own caption) — already noted in
  `specs/eru-allocator/spec.md`; irrelevant here except as a reminder the
  two algorithms' captions in this paper cannot be trusted at face value.
- Algorithm 3's own header is internally inconsistent: the "Input:" line
  above the pseudocode lists `Vᵢ, Eᵢ, Tᵢ, Cᵢ, Ū`, but the `function
  TDTA(...)` line right below it lists `τᵢ, 𝕃ᵢ, ξᵢ, Ū` — different
  parameters, and 𝕃ᵢ/ξᵢ (levels/Str-structures) are treated as if
  pre-computed inputs even though line 5 of the body ("Divide subtasks
  into levels...") computes them itself. **This implementation follows the
  body's actual, self-contained behavior** — compute δ (Eq. 8), compute
  levels (Eq. 5), then per level divide into Str-based groups (Definition
  1) — not the inconsistent signature/input lines.

## User Stories

- As the author, I want the allocator the paper's title and abstract
  actually describe (TDTA) implemented end-to-end, not just its ERU
  sub-routine, so a claim like "TDTA improves on WF+DRU" is something this
  codebase can reproduce or measure, not just describe.
- As the author, I want IED/level/Str detection implemented as small,
  independently testable functions (mirroring how WF+DRU already splits
  into `decreasing_remaining_utilization_order`/`worst_fit_place`), so
  each piece can be checked directly against the paper's own worked
  example (Fig. 2 → Fig. 3) instead of only end-to-end.

## Acceptance Criteria

- [ ] `TaskInfo` gains a `priority` field (`π(τᵢ)`, allocation-time-only
      metadata — see Non-Goals). `plan.allocation.strategy == "tdta"`
      processes `plan.tasks` strictly by **ascending** `TaskInfo::priority`
      (smaller value = higher priority, per the paper's convention — see
      Overview; ties: original plan order — stable); a task's subtasks are
      fully placed before the next task starts.
- [ ] **Per-task DAG** `Gᵢ = (Vᵢ, Eᵢ)`: built from exactly τᵢ's own
      subtasks and only the `plan.connections` entries whose upstream and
      downstream both belong to τᵢ — not the whole-plan merged graph
      `allocator::detail::build_plan_dag` uses today for WF+DRU/ERU.
- [ ] **IED, Algorithm 1**: for every subtask `Vᵢ,ⱼ` with ≥ 2 direct
      predecessors, for every ordered pair of its direct predecessors
      `(Vᵢ,ₐ, Vᵢ,ᵦ)`, a ≠ b, if `Vᵢ,ₐ` constrains `Vᵢ,ᵦ` (`Vᵢ,ᵦ` is
      reachable from `Vᵢ,ₐ` in `Gᵢ`), remove edge `e(Vᵢ,ₐ, Vᵢ,ⱼ)`. Verified
      against the paper's own worked example: Fig. 2 → Fig. 3 removes
      exactly `e(Vᵢ,1, Vᵢ,4)`.
- [ ] **Levels, Eq. 5**: `𝕃(Vᵢ,ⱼ) = 0` if `Vᵢ,ⱼ` is the source subtask;
      else `1 + max` over `Vᵢ,ⱼ`'s direct predecessors' levels. Computed
      on the **post-IED** graph. Verified against Fig. 3's 4 levels
      (0 through 3).
- [ ] **Str-structure detection, Definition 1**: "The subtasks that have
      and only have one common predecessor are called an Str structure."
      Implementation: for each subtask `p`, let `S = {c ∈ successors(p) :
      predecessors(c) == {p}}` (direct successors of `p` whose *entire*
      predecessor set is exactly `{p}`); if `|S| ≥ 2`, `S` is one Str
      structure. Verified against Fig. 2/3: one Str structure `ξᵢ,1 =
      {Vᵢ,2, Vᵢ,3}` before IED, a second `ξᵢ,2` exposed after IED removes
      `e(Vᵢ,1, Vᵢ,4)`.
- [ ] **Earliest start time, Eq. 8**: `δ(Vᵢ,ⱼ) = 0` if source; else `max`
      over direct predecessors of `δ(predecessor) + WCET(predecessor)`.
- [ ] **TDTA orchestration, Algorithm 3**: for each level, ascending,
      partition that level's subtasks into groups — one group per Str
      structure present at that level, plus one final group of subtasks
      belonging to no Str structure at that level. The Str-structure
      groups (not the final group) are visited in ascending order of the
      minimum `δ` within each group. Each group, in that order, becomes
      `V̄` passed to the ERU primitive from `specs/eru-allocator/` together
      with the running per-core `Ū`; `Ū` and the group's placements
      (`P̄`) accumulate into τᵢ's result and carry forward into the next
      group/level/task.
- [ ] Exposed as `plan.allocation.strategy == "tdta"`, a third branch in
      `allocator::apply_auto_allocation` alongside `"worst_fit"` (dru) and
      `"eru"`.
- [ ] `num_cores`/per-core capacity resolution identical to the other two
      strategies (`plan.allocation.num_cores`/`capacity`, same defaults).
- [ ] A subtask with `core != CORE_UNASSIGNED` on entry is never reassigned
      by `tdta::detail::allocate_task` — it is still accounted for in
      every core's remaining capacity, but left exactly as pinned. Matches
      `dru::detail::worst_fit_place`/`eru::apply_eru_allocation`'s own
      convention and `allocator::apply_auto_allocation`'s documented
      contract ("assigns a core to every subtask with core ==
      CORE_UNASSIGNED"), which `tdta` must honor too since it shares the
      same dispatcher entry point.
- [ ] Throws `std::runtime_error` for: any ERU-level placement
      infeasibility (same convention as WF+DRU/ERU); the missing-priority
      case in Open Questions below, per whatever that's resolved to.
- [ ] Unit tests reproduce the paper's own worked example (Fig. 2 → Fig. 3)
      for IED, level assignment, and Str detection independently of each
      other and of ERU, plus an end-to-end TDTA placement test and
      regression tests confirming WF+DRU/ERU are unaffected by the
      per-task-DAG and `TaskInfo::priority` additions.

## Non-Goals

- WCRT/schedulability analysis (the RRC algorithm, Eq. 1–4, Theorems 1–3).
  This feature is the *allocation* strategy only — proving or measuring
  TDTA's claimed WCRT/schedulability benefit over WF+DRU is a separate,
  future evaluation feature (same boundary `allocator-comparison-eval`
  already respects for WF+DRU vs. single-core).
- Multi-source/multi-sink DAG-task normalization (Sect. III's "a DAG task
  with two or more source subtasks can be transformed by adding a dummy
  source/sink subtask"). Out of scope; each task's `Gᵢ` is required to
  already have exactly one source and one sink, the same implicit
  assumption WF+DRU's DAG handling already makes.
- Changing `SubtaskInfo::priority` or `TeamManager`'s existing
  `(core, priority)` Dispatcher-thread grouping. The new
  `TaskInfo::priority` is allocation-time-only metadata consumed solely by
  TDTA's task-ordering step; runtime scheduling is untouched.
- Re-deriving or changing the ERU primitive itself — this feature only
  calls it per-group, per `specs/eru-allocator/`'s own contract.

## Open Questions

- [NEEDS CLARIFICATION: missing task priority] If
  `plan.allocation.strategy == "tdta"` and a task omits `priority`, should
  `apply_auto_allocation` throw immediately (ambiguous `π(τᵢ)` ordering
  whenever more than one task exists), or is an unset/default priority
  (e.g. `0`) acceptable when the plan has only one task (today's only real
  usage — `plans/deployment_plan.json` has a single task)? Leaning throw
  whenever `plan.tasks.size() > 1` and any task's `priority` is unset, to
  avoid a silent, arbitrary tie-break the author didn't ask for — confirm.

## Dependencies

- `specs/eru-allocator/` — ERU as the `V̄`-group placement primitive
  Algorithm 3 calls; this feature does not implement ERU itself.
- Sections III (system model: `𝕃(Vᵢ,ⱼ)` notation, `π(τᵢ)`), V (IED —
  Algorithm 1, Definition 4, Fig. 2/3 worked example), and VI (Definition
  1 (Str), Eq. 8, Algorithm 3) of
  `TDTA_Topology-Based_Real-Time_DAG_Task_Allocation_on_Identical_Multiprocessor_Platforms.pdf`.

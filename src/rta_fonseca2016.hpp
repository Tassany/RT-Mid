#pragma once

/**
 * @file rta_fonseca2016.hpp
 *
 * A second, independent WCRT analysis for DAG tasks, alongside
 * specs/rrc-analysis/ (not replacing it) — Fonseca, Nelissen, Nélis &
 * Pinho, "Response Time Analysis of Sporadic DAG Tasks under Partitioned
 * Scheduling" (RTCSA 2016). See specs/rta-fonseca2016/spec.md for the
 * algorithm, its scope (Joint variant only, Section VI's DAG-higher-
 * priority generalization included; Split/MILP excluded), and
 * specs/rta-fonseca2016/plan.md for the from-scratch worked example this
 * is tested against, including a mis-reading caught and corrected during
 * planning: R(lambda) is assembled via Theorem 2 (Eq. 5) as an EXPLICIT
 * final step over every core the path touches — Algorithm 1's own
 * recursion only populates each core's own self-suspending-task R/S^ub,
 * it does not itself produce a path-wide value.
 *
 * Allocation-strategy-agnostic, like rrc.hpp: operates on a
 * DeploymentPlan whose subtasks already have `core` assigned.
 *
 * "Self-interference" here is a DIFFERENT computation from
 * rrc::detail::self_interference_set, despite the same name in a
 * sibling namespace — see spec.md's Overview. Do not assume parity
 * between the two beyond the name.
 */

#include "deployment_plan.hpp"
#include <unordered_map>
#include <vector>

namespace rta_fonseca2016 {

namespace detail {

/**
 * @brief One maximal run of consecutive-same-core subtasks within a
 *        path (Sect. V's own stated simplifying assumption — required,
 *        not just convenient, for Algorithm 1's same-core-boundary case
 *        to stay well-defined; see plan.md's Key Decisions).
 */
struct ExecutionRegion {
    int core;
    std::vector<int> subtask_ids; // contiguous run, in path order
    double wcet;                  // sum of wcet_us over subtask_ids
};

/**
 * @brief Groups a path into maximal consecutive-same-core execution
 *        regions, summing WCET within each run.
 *
 * @param path Subtask ids from source to sink (one of a task's complete
 *        paths).
 * @param task_subtasks Subtasks of the path's own DAG task (for
 *        core/WCET lookup) — same convention as
 *        rrc::detail::path_length.
 * @return One ExecutionRegion per maximal same-core run, in path order.
 */
std::vector<ExecutionRegion> group_execution_regions(
    const std::vector<int>& path, const std::vector<SubtaskInfo>& task_subtasks);

/**
 * @brief Definition 3 (p-Workload): sum of WCET over ALL of
 *        @p task_subtasks assigned to @p core (not restricted to any
 *        one path).
 *
 * @param task_subtasks Subtasks of one DAG task.
 * @param core Core to sum WCET for.
 * @return `Wᵢᵖ`.
 */
double p_workload(const std::vector<SubtaskInfo>& task_subtasks, int core);

/**
 * @brief Lemma 1 / Eq. 3-4: self(λᵢ,ₖ) — every subtask sharing a core
 *        with @p path, excluding the path's own members and the
 *        provably non-interfering set `Θᵢ,ₖ`.
 *
 * `Θᵢ,ₖ` is computed per core `p` touched by the path: the (direct or
 * transitive) predecessors of the path's *first* subtask on `p`,
 * restricted to those also on `p`, union the (direct or transitive)
 * successors of the path's *last* subtask on `p`, similarly restricted.
 * A DIFFERENT computation from `rrc::detail::self_interference_set`
 * despite the same name — see this file's header comment and
 * specs/rta-fonseca2016/spec.md's Overview.
 *
 * @param path Subtask ids from source to sink.
 * @param task_subtasks Subtasks of the path's own DAG task.
 * @param reduced_connections That task's connections, post-IED.
 * @return Ids of every subtask self-interfering with @p path.
 */
std::vector<int> self_interference_set(
    const std::vector<int>& path,
    const std::vector<SubtaskInfo>& task_subtasks,
    const std::vector<ConnectionInfo>& reduced_connections);

/**
 * @brief Theorem 3 (Sect. VI): one virtual sequential task per subtask
 *        of @p hp_task assigned to @p core.
 *
 * `C = wcet_us`, `T` inherited from the subtask's own `period_us`
 * (equal to `hp_task`'s own period, same convention every task in this
 * codebase already uses), `J = r_hp_task - C` (release jitter).
 *
 * @param hp_task The higher-priority DAG task.
 * @param core Core to filter @p hp_task's subtasks to.
 * @param r_hp_task `hp_task`'s own WCRT, already computed (caller's
 *        responsibility — enforced by ascending-`TaskInfo::priority`
 *        processing order).
 * @return One VirtualTask per @p hp_task subtask assigned to @p core.
 */
struct VirtualTask {
    double c;
    double t;
    double j;
};
std::vector<VirtualTask> virtual_tasks_for_core(const TaskInfo& hp_task, int core, double r_hp_task);

/**
 * @brief Algorithm 1 + Theorem 2: R(lambda_i,k).
 *
 * Runs the recursive per-core unfolding (Eq. 6/7/8 — Joint, this
 * feature's only in-scope variant) once, populating a per-core
 * {R(tau^p), S^{p,ub}} result for every distinct core the path touches,
 * then assembles R(lambda) by applying Theorem 2 (Eq. 5) explicitly:
 * R(lambda) = sum over proc(lambda) of [R(tau^p) - S^{p,ub}]. See
 * plan.md's Technical Approach for why this is an explicit final step,
 * not something Algorithm 1's own recursion returns directly.
 *
 * Handles a core appearing any number of times in the path, including
 * non-adjacent/alternating patterns (e.g. core0,core1,core0,core1) —
 * the per-range memoization (keyed by region-index pair, matching the
 * paper's own RTs matrix) is what makes this safe: two different
 * resolutions of the same core, over two different ranges, never
 * collide. An earlier version of this function keyed its memo by core
 * alone and threw on a third occurrence; that restriction is gone (see
 * plan.md's Risks table for the worked derivation that found the gap
 * and the fix).
 *
 * @param plan Whole plan — supplies every higher-priority task and
 *        every subtask's assigned core.
 * @param task @p path's own DAG task.
 * @param path Subtask ids from source to sink.
 * @param task_subtasks @p task's own subtasks.
 * @param reduced_connections @p task's connections, post-IED.
 * @return R(lambda_i,k).
 * @throws std::runtime_error if plan.tasks.size() > 1 and any task's
 *         priority is TASK_PRIORITY_UNSET, or if a fixed point doesn't
 *         converge within the iteration cap.
 */
double response_time_of_path(
    const DeploymentPlan& plan,
    const TaskInfo& task,
    const std::vector<int>& path,
    const std::vector<SubtaskInfo>& task_subtasks,
    const std::vector<ConnectionInfo>& reduced_connections);

} // namespace detail

/**
 * @brief Corollary 1 (Eq. 2): R(tau_i), the max response time over
 *        @p task's own complete paths (via
 *        rrc::detail::enumerate_complete_paths, reused as-is).
 *
 * @param plan Whole plan.
 * @param task Task to compute the WCRT for.
 * @return R(tau_i).
 * @throws std::runtime_error — see detail::response_time_of_path.
 */
double compute_wcrt(const DeploymentPlan& plan, const TaskInfo& task);

/**
 * @brief R(tau_i) <= D_i for every task in @p plan.
 *
 * @param plan Plan to check.
 * @return Ids of tasks whose computed WCRT exceeds their own deadline
 *         (empty = the whole plan is schedulable).
 * @throws std::runtime_error — see detail::response_time_of_path.
 */
std::vector<int> check_schedulability(const DeploymentPlan& plan);

} // namespace rta_fonseca2016

#pragma once

/**
 * @file rrc.hpp
 *
 * RRC worst-case response time (WCRT) analysis — Sect. IV-A, Eq. 1-4, Wu,
 * Zhang, Guan & Ma, "TDTA: Topology-Based Real-Time DAG Task Allocation
 * on Identical Multiprocessor Platforms" (IEEE TPDS 34(11), 2023). See
 * specs/rrc-analysis/spec.md for the algorithm, its disclosed paper gap
 * (the fixed-point iteration procedure Eq. 2-3 imply isn't spelled out by
 * this paper's own text), and specs/rrc-analysis/plan.md for the
 * from-scratch worked example this is tested against.
 *
 * Allocation-strategy-agnostic: operates on a DeploymentPlan whose
 * subtasks already have `core` assigned (by any strategy, or hand-authored),
 * and never calls allocator::apply_auto_allocation itself.
 *
 * Built in the same dependency order as the equations: complete-path
 * enumeration (Sect. III's lambda_i) -> self-interference (Definition 3)
 * -> Omega (Eq. 4) -> I^h_j/R(lambda) (Eq. 2-3) -> R(tau) (Eq. 1) ->
 * schedulability.
 */

#include "deployment_plan.hpp"
#include <unordered_map>
#include <vector>

namespace rrc {

namespace detail {

/**
 * @brief Enumerates every complete source-to-sink path (Sect. III's
 *        lambda_i) of one DAG task.
 *
 * Operates on @p task_subtasks' own (already IED-reduced) graph via
 * @p reduced_connections. Assumes exactly one source (no predecessors)
 * and at least one sink (no successors) — the same single-source
 * assumption every other feature built on this graph already carries.
 *
 * @param task_subtasks Subtasks of exactly one DAG task.
 * @param reduced_connections This task's connections, post-IED.
 * @return Every complete path, each as subtask ids from source to sink.
 */
std::vector<std::vector<int>> enumerate_complete_paths(
    const std::vector<SubtaskInfo>& task_subtasks,
    const std::vector<ConnectionInfo>& reduced_connections);

/**
 * @brief L(lambda): sum of WCET over a path's own subtasks.
 * @param path Subtask ids from source to sink.
 * @param task_subtasks Subtasks of the path's own DAG task (for WCET
 *        lookup).
 * @return Sum of wcet_us over @p path.
 */
double path_length(const std::vector<int>& path, const std::vector<SubtaskInfo>& task_subtasks);

/**
 * @brief Definition 3: self(V_i,j), restricted to @p task_subtasks (same
 *        DAG task only).
 *
 * A subtask V_i,b self-interferes with @p subtask_id if (1) neither
 * constrains the other (not reachable from one to the other, either
 * direction, on the IED-reduced graph) and (2) they share the same
 * assigned core (SubtaskInfo::core).
 *
 * @param subtask_id Subtask to compute the self-interference set for.
 * @param task_subtasks Subtasks of @p subtask_id's own DAG task.
 * @param reduced_connections That task's connections, post-IED.
 * @return Ids of every subtask self-interfering with @p subtask_id.
 */
std::vector<int> self_interference_set(
    int subtask_id,
    const std::vector<SubtaskInfo>& task_subtasks,
    const std::vector<ConnectionInfo>& reduced_connections);

/**
 * @brief self(lambda): union of self_interference_set() over a path's
 *        own subtasks, deduplicated.
 *
 * @param path Subtask ids from source to sink.
 * @param task_subtasks Subtasks of the path's own DAG task.
 * @param reduced_connections That task's connections, post-IED.
 * @return Deduplicated ids self-interfering with some subtask on
 *         @p path.
 */
std::vector<int> path_self_interference(
    const std::vector<int>& path,
    const std::vector<SubtaskInfo>& task_subtasks,
    const std::vector<ConnectionInfo>& reduced_connections);

/**
 * @brief Eq. 4, summed over every Str structure of a higher-priority
 *        task that overlaps @p path's own processors.
 *
 * For each Str structure xi_r of the higher-priority task (from
 * tdta::detail::find_str_structures on its own reduced graph):
 * eta = rho(xi_r) intersected with rho(path) (via @p core_by_id); if
 * eta is empty that structure contributes 0 (Eq. 4's own stated
 * precondition); otherwise contributes (|eta|-1) times the minimum,
 * over eta's processors, of xi_r's own cumulative WCET on that one
 * processor.
 *
 * @param hp_task_subtasks The higher-priority task's own subtasks.
 * @param hp_task_reduced That task's connections, post-IED.
 * @param path The lower-priority task's path (subtask ids).
 * @param core_by_id SubtaskInfo::core for every subtask involved (both
 *        the higher-priority task's and the path's own).
 * @return Sum of Omega over every Str structure with eta != empty.
 */
double omega_sum(
    const std::vector<SubtaskInfo>& hp_task_subtasks,
    const std::vector<ConnectionInfo>& hp_task_reduced,
    const std::vector<int>& path,
    const std::unordered_map<int, int>& core_by_id);

/**
 * @brief Eq. 2 + 3 combined: the fixed-point R(lambda_i,k).
 *
 * Iterates the standard response-time-analysis recurrence (this paper's
 * own text defers the exact procedure to an uncited original RRC
 * article — see specs/rrc-analysis/spec.md's disclosed gap):
 * `R^0 = L(path) + self-interference`; each step recomputes every
 * higher-priority task's `I^h_j` using the *previous* iterate inside
 * `ceil(R/T_j)`, then `R' = L + self + sum(I^h_j)`; stops at a fixed
 * point (`R' == R`) or once `R' > task`'s own deadline (that path is
 * already proven unschedulable, no need to keep iterating). Throws
 * `std::runtime_error` if neither happens within a generous iteration
 * cap (spec.md's resolved open question), or if `plan.tasks.size() > 1`
 * and any task's priority is `TASK_PRIORITY_UNSET`.
 *
 * @param plan Whole plan — supplies every higher-priority task and every
 *        subtask's assigned core.
 * @param task @p path's own DAG task.
 * @param path Subtask ids from source to sink (one of @p task's complete
 *        paths).
 * @param task_subtasks @p task's own subtasks (for L/self-interference).
 * @param reduced_connections @p task's connections, post-IED.
 * @return `R(lambda_i,k)`.
 */
double response_time_of_path(
    const DeploymentPlan& plan,
    const TaskInfo& task,
    const std::vector<int>& path,
    const std::vector<SubtaskInfo>& task_subtasks,
    const std::vector<ConnectionInfo>& reduced_connections);

} // namespace detail

/**
 * @brief Eq. 1: R(tau_i), the max response time over @p task's own
 *        complete paths.
 *
 * @param plan Whole plan (passed through to detail::response_time_of_path).
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

} // namespace rrc

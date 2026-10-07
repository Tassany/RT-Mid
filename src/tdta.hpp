#pragma once

/**
 * @file tdta.hpp
 *
 * TDTA — Algorithm 3, Wu, Zhang, Guan & Ma, "TDTA: Topology-Based
 * Real-Time DAG Task Allocation on Identical Multiprocessor Platforms"
 * (IEEE TPDS 34(11), 2023), Sect. VI. See specs/tdta-allocator/spec.md
 * for the algorithm, its worked example (paper Fig. 2 -> Fig. 3), and the
 * Sect. VI header-line inconsistency this implementation resolves by
 * following the algorithm body rather than its own "Input:"/function
 * signature lines.
 *
 * Everything here operates downstream of ied::remove_invalid_edges
 * (Sect. V) on one DAG task's own (already-reduced) subtasks/connections.
 * This file is built up in the same order Algorithm 3's body computes
 * things: levels (Eq. 5), then earliest start time (Eq. 8), then Str
 * structures (Definition 1), then the per-level/per-group orchestration
 * that calls eru::detail::equilibrium_remaining_utilization_place, then
 * the outer loop across a whole plan's tasks by ascending
 * TaskInfo::priority.
 */

#include "deployment_plan.hpp"
#include <unordered_map>
#include <vector>

namespace tdta {

/**
 * @brief Assigns a core to every subtask of every task in @p plan, via
 *        TDTA (Algorithm 3), processed task by task in ascending
 *        TaskInfo::priority order (smaller value = higher priority).
 *
 * If @p plan has more than one task, every task's priority must be set
 * (!= TASK_PRIORITY_UNSET) — checked before any sorting or placement, so
 * an unset priority (numerically -1, the smallest possible value) can
 * never silently sort as "highest priority". A single-task plan proceeds
 * regardless, since there is no ordering ambiguity with only one task.
 *
 * @param plan Deployment plan whose subtasks are mutated in place.
 * @return void
 * @throws std::runtime_error if more than one task exists and any task's
 *         priority is unset, or — see detail::allocate_task /
 *         eru::detail::equilibrium_remaining_utilization_place — if
 *         placement is infeasible.
 */
void apply_tdta_allocation(DeploymentPlan& plan);

namespace detail {

/**
 * @brief Level assignment, Eq. 5, over one DAG task's (already
 *        IED-reduced) subtasks/connections.
 *
 * Level 0 is the source subtask; every other subtask's level is 1 + the
 * maximum level among its direct predecessors. Requires exactly one
 * source (no incoming edges) among @p task_subtasks.
 *
 * @param task_subtasks Subtasks of exactly one DAG task.
 * @param reduced_connections This task's connections, post-IED.
 * @return Level per subtask id.
 */
std::unordered_map<int, int> compute_levels(
    const std::vector<SubtaskInfo>& task_subtasks,
    const std::vector<ConnectionInfo>& reduced_connections);

/**
 * @brief Earliest start time, Eq. 8, over one DAG task's (already
 *        IED-reduced) subtasks/connections.
 *
 * The source subtask's earliest start time is 0; every other subtask's is
 * the maximum, over its direct predecessors, of that predecessor's
 * earliest start time plus its own WCET.
 *
 * @param task_subtasks Subtasks of exactly one DAG task.
 * @param reduced_connections This task's connections, post-IED.
 * @return Earliest start time (delta) per subtask id.
 */
std::unordered_map<int, double> compute_earliest_start_times(
    const std::vector<SubtaskInfo>& task_subtasks,
    const std::vector<ConnectionInfo>& reduced_connections);

/**
 * @brief Str-structure detection, Definition 1, over one DAG task's
 *        (already IED-reduced) subtasks/connections.
 *
 * For each subtask p, considered in ascending id order (tie-break for
 * structures whose earliest-start-time ends up equal — see
 * specs/tdta-allocator/plan.md), the set of p's direct successors whose
 * own direct-predecessor set is exactly {p} forms one Str structure, kept
 * only when it has >= 2 members (a single member can't self-interfere).
 *
 * @param task_subtasks Subtasks of exactly one DAG task.
 * @param reduced_connections This task's connections, post-IED.
 * @return One vector of subtask ids per Str structure found, each of
 *         size >= 2, ordered by ascending id of the structure's common
 *         predecessor.
 */
std::vector<std::vector<int>> find_str_structures(
    const std::vector<SubtaskInfo>& task_subtasks,
    const std::vector<ConnectionInfo>& reduced_connections);

/**
 * @brief Algorithm 3 for exactly one DAG task.
 *
 * Runs ied::remove_invalid_edges on @p task's own connections (sliced
 * from plan.connections to this task's subtasks), then computes levels
 * (Eq. 5), earliest start time (Eq. 8), and Str structures (Definition 1)
 * on the result. For each level ascending, partitions that level's
 * subtasks into one group per Str structure present there (ordered by
 * ascending minimum earliest-start-time within the group; ties by the
 * structure's own order, i.e. ascending parent id) plus one final group
 * of subtasks belonging to no structure — omitted entirely when empty.
 * Each group, in order, is placed via
 * eru::detail::equilibrium_remaining_utilization_place.
 *
 * @param plan Plan mutated in place — only @p task's own subtasks'
 *        SubtaskInfo::core are assigned.
 * @param task The DAG task to allocate.
 * @return void
 * @throws std::runtime_error — see
 *         eru::detail::equilibrium_remaining_utilization_place.
 */
void allocate_task(DeploymentPlan& plan, const TaskInfo& task);

} // namespace detail
} // namespace tdta

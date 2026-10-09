#pragma once

/**
 * @file ied.hpp
 *
 * Invalid-Edge Deletion (IED) — Algorithm 1, Wu, Zhang, Guan & Ma, "TDTA:
 * Topology-Based Real-Time DAG Task Allocation on Identical Multiprocessor
 * Platforms" (IEEE TPDS 34(11), 2023), Sect. V. See
 * specs/tdta-allocator/spec.md for the algorithm and its worked example
 * (paper Fig. 2 -> Fig. 3).
 *
 * remove_invalid_edges operates on exactly one DAG task's own subtasks
 * and connections (not the whole-plan graph dru.hpp/eru.hpp build) —
 * building that per-task slice is the caller's responsibility (see
 * tdta.cpp). "Constrains" (reachability) is computed once on the
 * original, pre-removal edge set; Theorem 1 (an invalid edge is
 * redundant with an existing alternate path) guarantees this is
 * equivalent to recomputing it incrementally as edges are removed.
 */

#include "deployment_plan.hpp"
#include <vector>

namespace ied {

/**
 * @brief Slices @p plan's connections to exactly @p task's own subtasks.
 *
 * Every entry of the result has both endpoints in @p task.subtasks;
 * cross-task connections (none in this codebase's own plans, but not
 * assumed impossible) are excluded. This is @ref remove_invalid_edges's
 * usual input, computed once per task by every caller that needs a
 * task's own (pre-IED) graph.
 *
 * @param plan Plan to slice connections from.
 * @param task Task whose own connections to extract.
 * @return Connections whose upstream and downstream both belong to
 *         @p task.
 */
std::vector<ConnectionInfo> task_connections(const DeploymentPlan& plan, const TaskInfo& task);

/**
 * @brief Removes invalid edges (Algorithm 1) from one DAG task's own
 *        connections.
 *
 * For every subtask with >= 2 direct predecessors, and every ordered pair
 * (a, b) of its direct predecessors with a != b: if a constrains b (b is
 * reachable from a in the original graph), the edge from a into that
 * subtask is invalid and excluded from the result.
 *
 * @param task_subtasks Subtasks of exactly one DAG task.
 * @param task_connections Connections whose endpoints are both in
 *        @p task_subtasks (caller's responsibility).
 * @return The reduced connection list; @p task_connections itself is
 *         never mutated.
 */
std::vector<ConnectionInfo> remove_invalid_edges(
    const std::vector<SubtaskInfo>& task_subtasks,
    const std::vector<ConnectionInfo>& task_connections);

} // namespace ied

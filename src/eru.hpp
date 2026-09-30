#pragma once

/**
 * @file eru.hpp
 *
 * Equilibrium Remaining Utilization (ERU) — Algorithm 2, Wu, Zhang, Guan &
 * Ma, "TDTA: Topology-Based Real-Time DAG Task Allocation on Identical
 * Multiprocessor Platforms" (IEEE TPDS 34(11), 2023). See
 * specs/eru-allocator/spec.md for the algorithm this implements and its
 * paper-caption errata note.
 *
 * equilibrium_remaining_utilization_place implements Algorithm 2 over one
 * caller-supplied group of subtasks (the paper's V̄): it selects the mu =
 * min(num_cores, |group|) cores with the greatest current remaining
 * capacity, then places the group's subtasks largest-WCET-first onto
 * whichever of those mu cores currently has the most *virtual* remaining
 * capacity — a per-call scratch value that restarts at full capacity for
 * all mu cores regardless of their real remaining capacity. This is what
 * distinguishes ERU from Worst-Fit: it balances one group's load evenly
 * across exactly mu cores, decoupled from pre-existing load skew between
 * them.
 *
 * apply_eru_allocation is the whole-plan entry point (one call, one group
 * = every unassigned subtask) exposed as plan.allocation.strategy ==
 * "eru". specs/tdta-allocator/ calls the detail function directly, once
 * per topology-chosen group, instead.
 */

#include "allocator.hpp"
#include "deployment_plan.hpp"
#include <vector>

namespace eru {

namespace detail {

/**
 * @brief Implements Algorithm 2 (ERU) over exactly the subtasks named by
 *        @p group_ids.
 *
 * num_cores/capacity resolve from plan.allocation, same convention as
 * dru::detail::worst_fit_place. Real remaining capacity per core is
 * (re)computed from every subtask currently assigned in @p plan (not
 * limited to @p group_ids), so repeated calls — one per group, as
 * specs/tdta-allocator/ makes — see each other's prior placements without
 * any extra state threaded between calls. mu = min(num_cores,
 * group_ids.size()) cores are selected once by that real remaining
 * capacity (descending; ties broken by lowest core index) into a fixed
 * mapping "theta" for this call. A virtual scratch array, one entry per
 * selected core, is initialized to per-core capacity (independent of that
 * core's real remaining capacity) and tracks only this call's own
 * balance. @p group_ids' subtasks are placed largest-WCET-first (ties:
 * @p group_ids' own order) onto whichever selected core currently has the
 * largest virtual remaining capacity (ties: lowest core index).
 *
 * @param plan Plan mutated in place (SubtaskInfo::core assigned for ids
 *        in @p group_ids).
 * @param group_ids Subtask ids to place in this call (the paper's V̄).
 * @return void
 * @throws std::runtime_error if num_cores can't be determined, a
 *         pre-assigned subtask names a core outside [0, num_cores), or
 *         placing a subtask would drive its selected core's virtual
 *         remaining capacity negative.
 */
void equilibrium_remaining_utilization_place(DeploymentPlan& plan,
                                              const std::vector<int>& group_ids);

} // namespace detail

/**
 * @brief Assigns a core to every subtask with core == CORE_UNASSIGNED, via
 *        one Algorithm 2 (ERU) call over the whole plan as a single group.
 *
 * See this file's header comment and specs/eru-allocator/spec.md. Only
 * plan.allocation.strategy == "eru" is expected to route here (via
 * allocator::apply_auto_allocation).
 *
 * @param plan Deployment plan whose unassigned subtasks are mutated in
 *        place.
 * @return void
 * @throws std::runtime_error — see detail::equilibrium_remaining_utilization_place.
 */
void apply_eru_allocation(DeploymentPlan& plan);

} // namespace eru

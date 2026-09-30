#pragma once

/**
 * @file dru.hpp
 *
 * Worst-Fit with Decreasing Remaining Utilisation (WF+DRU) — Verucchi,
 * Sañudo Olmedo & Bertogna 2023, Sect. 4 intro + 7.11/8.2 (see
 * specs/wf-dru-allocator/spec.md and specs/eru-allocator/plan.md for why
 * this lives in its own file rather than allocator.hpp).
 *
 * Two independently-testable steps: decreasing_remaining_utilization_order
 * sorts unassigned subtasks by descending sum of their direct successors'
 * utilization in the whole-plan DAG; worst_fit_place walks that order,
 * placing each on the core with the most remaining capacity.
 *
 * apply_wf_dru_allocation implements only this one AllocationConfig
 * combination — its other declared values exist for the paper's own
 * baseline comparisons, not for use here, and are rejected.
 */

#include "allocator.hpp"
#include "deployment_plan.hpp"
#include <vector>

namespace dru {

namespace detail {

/**
 * @brief Orders unassigned subtasks by Decreasing Remaining Utilisation.
 *
 * Remaining utilization of a subtask is the sum of
 * allocator::detail::utilization() over its direct successors in the
 * whole-plan DAG (not the full transitive descendant set — the paper's
 * own notation distinguishes "successors" from "descendants", and DRU's
 * definition uses the former). Only subtasks with core == CORE_UNASSIGNED
 * are ordered; already-assigned subtasks are excluded from the result but
 * still count toward their successors' remaining utilization. Ties are
 * broken by original plan order (stable sort).
 *
 * @param plan Plan to compute the ordering for.
 * @return Ids of CORE_UNASSIGNED subtasks, descending by remaining
 *         utilization.
 */
std::vector<int> decreasing_remaining_utilization_order(const DeploymentPlan& plan);

/**
 * @brief Places subtasks by Worst-Fit: most-remaining-capacity core first.
 *
 * num_cores/capacity resolve from plan.allocation (0 = hardware_concurrency()
 * / 1.0). Each core's starting remaining capacity already subtracts
 * subtasks pre-assigned to it. Walks @p ordered_ids, placing each on the
 * core with the most remaining capacity (ties: lowest index).
 *
 * @param plan Plan mutated in place (SubtaskInfo::core assigned for ids
 *        in @p ordered_ids).
 * @param ordered_ids Subtask ids in placement order, e.g. from
 *        decreasing_remaining_utilization_order.
 * @return void
 * @throws std::runtime_error if num_cores can't be determined, a
 *         pre-assigned subtask names a core outside [0, num_cores), or no
 *         core has enough remaining capacity for some subtask.
 */
void worst_fit_place(DeploymentPlan& plan, const std::vector<int>& ordered_ids);

} // namespace detail

/**
 * @brief Assigns a core to every subtask with core == CORE_UNASSIGNED,
 *        via Worst-Fit with Decreasing Remaining Utilisation (WF+DRU).
 *
 * See this file's header comment and specs/wf-dru-allocator/spec.md. Only
 * plan.allocation == {strategy: "worst_fit", sort_by:
 * "remaining_utilization_desc", weight: "utilization"} is supported (the
 * default AllocationConfig); any other combination is rejected, since
 * this codebase implements exactly the paper's chosen heuristic, not the
 * general comparison framework AllocationConfig's other fields imply.
 *
 * @param plan Deployment plan whose unassigned subtasks are mutated in
 *        place.
 * @return void
 * @throws std::runtime_error if plan.allocation names an unimplemented
 *         strategy/sort_by/weight, or if placement is infeasible (see
 *         detail::worst_fit_place).
 */
void apply_wf_dru_allocation(DeploymentPlan& plan);

} // namespace dru

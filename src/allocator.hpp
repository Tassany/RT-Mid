#pragma once

/**
 * @file allocator.hpp
 *
 * Shared subtask/DAG helpers used by every allocation strategy, plus the
 * public dispatcher: apply_auto_allocation reads plan.allocation.strategy
 * and calls the matching strategy's own entry point (dru::apply_wf_dru_allocation,
 * eru::apply_eru_allocation, ...). Each strategy's own algorithm lives in
 * its own file (dru.hpp/dru.cpp, eru.hpp/eru.cpp, ...) — this header only
 * owns what more than one of them needs.
 */

#include "deployment_plan.hpp"
#include "dag.hpp"

namespace allocator {

namespace detail {

/**
 * @brief Utilization of a single subtask: wcet_us / period_us.
 * @param st Subtask to compute utilization for.
 * @return WCET divided by period, as a double.
 */
double utilization(const SubtaskInfo& st);

/**
 * @brief Builds the whole-plan dependency graph.
 *
 * All subtasks across every task in @p plan become one shared node set;
 * plan.connections become edges — the same construction
 * JsonParser::validate_structure uses, so allocation sees the same graph
 * the cycle check already validated.
 *
 * @param plan Plan to build the graph from.
 * @return DAG with one node per subtask and one edge per connection.
 */
DAG build_plan_dag(const DeploymentPlan& plan);

} // namespace detail

/**
 * @brief Assigns a core to every subtask with core == CORE_UNASSIGNED.
 *
 * Dispatches on plan.allocation.strategy: "worst_fit" calls
 * dru::apply_wf_dru_allocation (WF+DRU); "eru" calls
 * eru::apply_eru_allocation; "tdta" calls tdta::apply_tdta_allocation.
 * Any other strategy value is rejected — this codebase implements exactly
 * these named heuristics, not a general allocation-strategy framework.
 *
 * @param plan Deployment plan whose unassigned subtasks are mutated in
 *        place.
 * @return void
 * @throws std::runtime_error if plan.allocation.strategy is none of
 *         "worst_fit", "eru", "tdta", or if the chosen strategy itself
 *         reports infeasible placement.
 */
void apply_auto_allocation(DeploymentPlan& plan);

} // namespace allocator

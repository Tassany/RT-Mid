#pragma once

/**
 * @file allocator.hpp
 *
 * STUB — placeholder only, NOT the real allocation heuristic.
 *
 * The real automatic core-allocation algorithm (Worst-Fit with Decreasing
 * Remaining Utilisation, paper Section 4.5 — this project's main scientific
 * contribution) is being rewritten from scratch, spec-first, with the
 * scrutiny RULES.md §4 requires for the core contribution. It does not
 * belong in this file and should not be drafted here.
 *
 * This stub exists only so parser_json.cpp compiles and its parse/validate
 * pipeline (including the "no core given in the plan" path) can be tested
 * end-to-end before the real allocator exists. It assigns every unassigned
 * subtask to core 0 — not a scheduling decision of any kind, just enough to
 * unblock JsonParser::parse(). Replace this whole file when the real
 * allocator is written; nothing here should survive that.
 */

#include "deployment_plan.hpp"

namespace allocator {

/**
 * @brief STUB allocator: assigns every unassigned subtask to core 0.
 *
 * Walks every task and subtask in @p plan and, for each subtask whose
 * core is still CORE_UNASSIGNED, sets it to core 0. This is not a real
 * scheduling decision — it exists only to unblock JsonParser::parse() so
 * the parse/validate pipeline can be exercised end-to-end before the real
 * Worst-Fit allocator (paper Section 4.5) is written.
 *
 * @param plan Deployment plan whose tasks/subtasks are mutated in place.
 * @return void
 */
inline void apply_auto_allocation(DeploymentPlan& plan) {
    for (auto& task : plan.tasks)
        for (auto& st : task.subtasks)
            if (st.core == CORE_UNASSIGNED)
                st.core = 0; // STUB: everything on core 0, not a real strategy
}

} // namespace allocator

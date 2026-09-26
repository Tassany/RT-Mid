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

inline void apply_auto_allocation(DeploymentPlan& plan) {
    for (auto& task : plan.tasks)
        for (auto& st : task.subtasks)
            if (st.core == CORE_UNASSIGNED)
                st.core = 0; // STUB: everything on core 0, not a real strategy
}

} // namespace allocator

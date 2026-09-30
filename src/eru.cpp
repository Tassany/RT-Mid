#include "eru.hpp"
#include <algorithm>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>

namespace eru {

namespace detail {

/** @copydoc equilibrium_remaining_utilization_place */
void equilibrium_remaining_utilization_place(DeploymentPlan& plan,
                                              const std::vector<int>& group_ids) {
    constexpr double kEpsilon = 1e-9;

    int num_cores = plan.allocation.num_cores;
    if (num_cores == 0)
        num_cores = static_cast<int>(std::thread::hardware_concurrency());
    if (num_cores <= 0)
        throw std::runtime_error(
            "eru::equilibrium_remaining_utilization_place: could not determine num_cores "
            "(hardware_concurrency() returned 0); set allocation.num_cores explicitly");

    double per_core_capacity = plan.allocation.capacity;
    if (per_core_capacity == 0.0)
        per_core_capacity = 1.0;

    // Real remaining capacity per core, from every subtask currently
    // assigned anywhere in the plan (not just group_ids) -- this is what
    // lets repeated calls (one per topology-chosen group) see each
    // other's prior placements with no extra state threaded between them.
    std::vector<double> remaining_capacity(static_cast<size_t>(num_cores), per_core_capacity);
    std::unordered_map<int, SubtaskInfo*> subtask_by_id;
    for (auto& task : plan.tasks) {
        for (auto& st : task.subtasks) {
            subtask_by_id[st.id] = &st;
            if (st.core != CORE_UNASSIGNED) {
                if (st.core < 0 || st.core >= num_cores)
                    throw std::runtime_error(
                        "eru::equilibrium_remaining_utilization_place: subtask " +
                        std::to_string(st.id) + " is pre-assigned to core " +
                        std::to_string(st.core) + " which is outside [0, " +
                        std::to_string(num_cores) + ")");
                remaining_capacity[static_cast<size_t>(st.core)] -= allocator::detail::utilization(st);
            }
        }
    }

    int mu = std::min(num_cores, static_cast<int>(group_ids.size()));

    // theta: the mu core indices with the largest real remaining capacity,
    // descending, ties broken by lowest index -- equivalent to Algorithm
    // 2 lines 8-11's iterative argmax-and-remove loop, computed as one
    // stable sort instead (see specs/eru-allocator/plan.md's Key Decisions).
    std::vector<int> core_order(static_cast<size_t>(num_cores));
    std::iota(core_order.begin(), core_order.end(), 0);
    std::stable_sort(core_order.begin(), core_order.end(), [&](int a, int b) {
        return remaining_capacity[static_cast<size_t>(a)] > remaining_capacity[static_cast<size_t>(b)];
    });
    std::vector<int> theta(core_order.begin(), core_order.begin() + mu);

    // Virtual scratch: one slot per selected core, reset to full per-core
    // capacity for this call only, independent of that core's real
    // remaining capacity (the property that distinguishes ERU from
    // Worst-Fit -- see this file's header comment).
    std::vector<double> virtual_remaining(static_cast<size_t>(mu), per_core_capacity);

    // Largest-WCET-first order, ties broken by group_ids' own order --
    // equivalent to Algorithm 2 line 13's per-iteration argmax over C̄,
    // computed as one stable sort instead (WCETs don't change during the
    // loop, unlike virtual_remaining).
    std::vector<int> placement_order = group_ids;
    std::stable_sort(placement_order.begin(), placement_order.end(), [&](int a, int b) {
        return subtask_by_id.at(a)->wcet_us > subtask_by_id.at(b)->wcet_us;
    });

    for (int id : placement_order) {
        SubtaskInfo& st = *subtask_by_id.at(id);
        double u = allocator::detail::utilization(st);

        int best_slot = 0;
        for (int s = 1; s < mu; ++s)
            if (virtual_remaining[static_cast<size_t>(s)] > virtual_remaining[static_cast<size_t>(best_slot)])
                best_slot = s;

        if (virtual_remaining[static_cast<size_t>(best_slot)] + kEpsilon < u)
            throw std::runtime_error(
                "eru::equilibrium_remaining_utilization_place: no selected core has enough "
                "remaining capacity for subtask " + std::to_string(id) + " (needs " +
                std::to_string(u) + ", best available core " + std::to_string(theta[static_cast<size_t>(best_slot)]) +
                " has " + std::to_string(virtual_remaining[static_cast<size_t>(best_slot)]) + " remaining)");

        st.core = theta[static_cast<size_t>(best_slot)];
        virtual_remaining[static_cast<size_t>(best_slot)] -= u;
    }
}

} // namespace detail

/** @copydoc apply_eru_allocation */
void apply_eru_allocation(DeploymentPlan& plan) {
    std::vector<int> group_ids;
    for (const auto& task : plan.tasks)
        for (const auto& st : task.subtasks)
            if (st.core == CORE_UNASSIGNED)
                group_ids.push_back(st.id);

    detail::equilibrium_remaining_utilization_place(plan, group_ids);
}

} // namespace eru

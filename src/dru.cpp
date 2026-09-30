#include "dru.hpp"
#include <algorithm>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>

namespace dru {

namespace detail {

/** @copydoc decreasing_remaining_utilization_order */
std::vector<int> decreasing_remaining_utilization_order(const DeploymentPlan& plan) {
    DAG dag = allocator::detail::build_plan_dag(plan);

    std::unordered_map<int, double> utilization_by_id;
    std::unordered_map<int, const DAG::Node*> node_by_id;
    std::vector<int> unassigned_ids; // in original plan order

    for (const auto& task : plan.tasks) {
        for (const auto& st : task.subtasks) {
            utilization_by_id[st.id] = allocator::detail::utilization(st);
            if (st.core == CORE_UNASSIGNED)
                unassigned_ids.push_back(st.id);
        }
    }
    for (const auto& node : dag.nodes())
        node_by_id[node.id] = &node;

    std::vector<std::pair<int, double>> remaining; // (id, remaining utilization)
    remaining.reserve(unassigned_ids.size());
    for (int id : unassigned_ids) {
        double sum = 0.0;
        for (int succ_id : node_by_id.at(id)->successors)
            sum += utilization_by_id.at(succ_id);
        remaining.emplace_back(id, sum);
    }

    std::stable_sort(remaining.begin(), remaining.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });

    std::vector<int> ordered_ids;
    ordered_ids.reserve(remaining.size());
    for (const auto& [id, u] : remaining)
        ordered_ids.push_back(id);
    return ordered_ids;
}

/** @copydoc worst_fit_place */
void worst_fit_place(DeploymentPlan& plan, const std::vector<int>& ordered_ids) {
    constexpr double kEpsilon = 1e-9;

    int num_cores = plan.allocation.num_cores;
    if (num_cores == 0)
        num_cores = static_cast<int>(std::thread::hardware_concurrency());
    if (num_cores <= 0)
        throw std::runtime_error(
            "dru::worst_fit_place: could not determine num_cores "
            "(hardware_concurrency() returned 0); set allocation.num_cores explicitly");

    double per_core_capacity = plan.allocation.capacity;
    if (per_core_capacity == 0.0)
        per_core_capacity = 1.0;

    std::vector<double> remaining_capacity(static_cast<size_t>(num_cores), per_core_capacity);
    std::unordered_map<int, SubtaskInfo*> subtask_by_id;

    for (auto& task : plan.tasks) {
        for (auto& st : task.subtasks) {
            subtask_by_id[st.id] = &st;
            if (st.core != CORE_UNASSIGNED) {
                if (st.core < 0 || st.core >= num_cores)
                    throw std::runtime_error(
                        "dru::worst_fit_place: subtask " + std::to_string(st.id) +
                        " is pre-assigned to core " + std::to_string(st.core) +
                        " which is outside [0, " + std::to_string(num_cores) + ")");
                remaining_capacity[static_cast<size_t>(st.core)] -= allocator::detail::utilization(st);
            }
        }
    }

    for (int id : ordered_ids) {
        SubtaskInfo& st = *subtask_by_id.at(id);
        double u = allocator::detail::utilization(st);

        int best_core = 0;
        for (int c = 1; c < num_cores; ++c)
            if (remaining_capacity[static_cast<size_t>(c)] > remaining_capacity[static_cast<size_t>(best_core)])
                best_core = c;

        if (remaining_capacity[static_cast<size_t>(best_core)] + kEpsilon < u)
            throw std::runtime_error(
                "dru::worst_fit_place: no core has enough remaining capacity for subtask " +
                std::to_string(id) + " (needs " + std::to_string(u) + ", best available core " +
                std::to_string(best_core) + " has " +
                std::to_string(remaining_capacity[static_cast<size_t>(best_core)]) + " remaining)");

        st.core = best_core;
        remaining_capacity[static_cast<size_t>(best_core)] -= u;
    }
}

} // namespace detail

/** @copydoc apply_wf_dru_allocation */
void apply_wf_dru_allocation(DeploymentPlan& plan) {
    const auto& cfg = plan.allocation;
    if (cfg.strategy != "worst_fit" ||
        cfg.sort_by  != "remaining_utilization_desc" ||
        cfg.weight   != "utilization")
        throw std::runtime_error(
            "dru::apply_wf_dru_allocation: this codebase implements only "
            "strategy=worst_fit, sort_by=remaining_utilization_desc, "
            "weight=utilization (Worst-Fit with Decreasing Remaining Utilisation, "
            "Verucchi et al. 2023); got strategy=" + cfg.strategy +
            ", sort_by=" + cfg.sort_by + ", weight=" + cfg.weight);

    std::vector<int> ordered_ids = detail::decreasing_remaining_utilization_order(plan);
    detail::worst_fit_place(plan, ordered_ids);
}

} // namespace dru

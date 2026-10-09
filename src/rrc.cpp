#include "rrc.hpp"
#include "dag.hpp"
#include "ied.hpp"
#include "tdta.hpp"
#include <algorithm>
#include <cmath>
#include <queue>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace rrc {

namespace detail {

namespace {

// Recursive DFS from `node`, extending `current` to every complete
// source-to-sink path and appending finished paths to `out`.
void extend_path(int node, const std::unordered_map<int, const DAG::Node*>& node_by_id,
                  std::vector<int>& current, std::vector<std::vector<int>>& out) {
    const DAG::Node* n = node_by_id.at(node);
    if (n->successors.empty()) {
        out.push_back(current);
        return;
    }
    for (int succ : n->successors) {
        current.push_back(succ);
        extend_path(succ, node_by_id, current, out);
        current.pop_back();
    }
}

// BFS reachability from `start` following `neighbors_of(node)` (either
// successors, for descendants, or predecessors, for ancestors) -- same
// pattern as ied.cpp's own local reachability helper, duplicated rather
// than shared per this codebase's established precedent (two small,
// independent 15-line BFS helpers beat a shared abstraction with one
// caller each).
template <typename NeighborsFn>
std::unordered_set<int> reachable_via(int start, const std::unordered_map<int, const DAG::Node*>& node_by_id,
                                       NeighborsFn neighbors_of) {
    std::unordered_set<int> visited;
    std::queue<int> frontier;
    frontier.push(start);
    while (!frontier.empty()) {
        int cur = frontier.front();
        frontier.pop();
        auto it = node_by_id.find(cur);
        if (it == node_by_id.end())
            continue;
        for (int next : neighbors_of(it->second))
            if (visited.insert(next).second)
                frontier.push(next);
    }
    return visited;
}

} // namespace

/** @copydoc enumerate_complete_paths */
std::vector<std::vector<int>> enumerate_complete_paths(
    const std::vector<SubtaskInfo>& task_subtasks,
    const std::vector<ConnectionInfo>& reduced_connections) {

    DAG dag;
    for (const auto& st : task_subtasks)
        dag.add_node(st.id);
    for (const auto& c : reduced_connections)
        dag.add_edge(c.upstream, c.downstream);

    std::unordered_map<int, const DAG::Node*> node_by_id;
    for (const auto& node : dag.nodes())
        node_by_id[node.id] = &node;

    int source = task_subtasks.front().id;
    for (const auto& st : task_subtasks) {
        if (node_by_id.at(st.id)->predecessors.empty()) {
            source = st.id;
            break;
        }
    }

    std::vector<std::vector<int>> paths;
    std::vector<int> current{source};
    extend_path(source, node_by_id, current, paths);
    return paths;
}

/** @copydoc path_length */
double path_length(const std::vector<int>& path, const std::vector<SubtaskInfo>& task_subtasks) {
    std::unordered_map<int, uint64_t> wcet_by_id;
    for (const auto& st : task_subtasks)
        wcet_by_id[st.id] = st.wcet_us;

    double sum = 0.0;
    for (int id : path)
        sum += static_cast<double>(wcet_by_id.at(id));
    return sum;
}

/** @copydoc self_interference_set */
std::vector<int> self_interference_set(
    int subtask_id,
    const std::vector<SubtaskInfo>& task_subtasks,
    const std::vector<ConnectionInfo>& reduced_connections) {

    DAG dag;
    for (const auto& st : task_subtasks)
        dag.add_node(st.id);
    for (const auto& c : reduced_connections)
        dag.add_edge(c.upstream, c.downstream);

    std::unordered_map<int, const DAG::Node*> node_by_id;
    for (const auto& node : dag.nodes())
        node_by_id[node.id] = &node;

    auto descendants = reachable_via(subtask_id, node_by_id,
        [](const DAG::Node* n) -> const std::vector<int>& { return n->successors; });
    auto ancestors = reachable_via(subtask_id, node_by_id,
        [](const DAG::Node* n) -> const std::vector<int>& { return n->predecessors; });

    std::unordered_map<int, int> core_by_id;
    for (const auto& st : task_subtasks)
        core_by_id[st.id] = st.core;
    int this_core = core_by_id.at(subtask_id);

    std::vector<int> result;
    for (const auto& st : task_subtasks) {
        if (st.id == subtask_id)
            continue;
        if (descendants.count(st.id) || ancestors.count(st.id))
            continue; // precedence constraint exists -- not self-interference
        if (st.core == this_core)
            result.push_back(st.id);
    }
    return result;
}

/** @copydoc path_self_interference */
std::vector<int> path_self_interference(
    const std::vector<int>& path,
    const std::vector<SubtaskInfo>& task_subtasks,
    const std::vector<ConnectionInfo>& reduced_connections) {

    std::unordered_set<int> dedup;
    for (int id : path)
        for (int other : self_interference_set(id, task_subtasks, reduced_connections))
            dedup.insert(other);

    std::vector<int> result(dedup.begin(), dedup.end());
    std::sort(result.begin(), result.end());
    return result;
}

/** @copydoc omega_sum */
double omega_sum(
    const std::vector<SubtaskInfo>& hp_task_subtasks,
    const std::vector<ConnectionInfo>& hp_task_reduced,
    const std::vector<int>& path,
    const std::unordered_map<int, int>& core_by_id) {

    std::unordered_map<int, uint64_t> wcet_by_id;
    for (const auto& st : hp_task_subtasks)
        wcet_by_id[st.id] = st.wcet_us;

    std::unordered_set<int> path_cores;
    for (int id : path)
        path_cores.insert(core_by_id.at(id));

    auto structures = tdta::detail::find_str_structures(hp_task_subtasks, hp_task_reduced);

    double total = 0.0;
    for (const auto& structure : structures) {
        std::unordered_set<int> structure_cores;
        for (int id : structure)
            structure_cores.insert(core_by_id.at(id));

        std::vector<int> eta;
        for (int c : structure_cores)
            if (path_cores.count(c))
                eta.push_back(c);
        if (eta.empty())
            continue; // Eq. 4's own precondition: eta != empty

        double min_per_core = -1.0;
        for (int c : eta) {
            double sum = 0.0;
            for (int id : structure)
                if (core_by_id.at(id) == c)
                    sum += static_cast<double>(wcet_by_id.at(id));
            if (min_per_core < 0.0 || sum < min_per_core)
                min_per_core = sum;
        }

        total += static_cast<double>(eta.size() - 1) * min_per_core;
    }
    return total;
}

namespace {
constexpr int kMaxIterations = 10000;
constexpr double kEpsilon = 1e-9;
}

/** @copydoc response_time_of_path */
double response_time_of_path(
    const DeploymentPlan& plan,
    const TaskInfo& task,
    const std::vector<int>& path,
    const std::vector<SubtaskInfo>& task_subtasks,
    const std::vector<ConnectionInfo>& reduced_connections) {

    if (plan.tasks.size() > 1)
        for (const auto& t : plan.tasks)
            if (t.priority == TASK_PRIORITY_UNSET)
                throw std::runtime_error(
                    "rrc::response_time_of_path: task " + std::to_string(t.id) +
                    " has no priority set, but the plan has " + std::to_string(plan.tasks.size()) +
                    " tasks -- hp(tau_i) is undefined without a total priority order");

    double L = path_length(path, task_subtasks);

    std::unordered_map<int, uint64_t> self_wcet_by_id;
    for (const auto& st : task_subtasks)
        self_wcet_by_id[st.id] = st.wcet_us;
    double self_sum = 0.0;
    for (int id : path_self_interference(path, task_subtasks, reduced_connections))
        self_sum += static_cast<double>(self_wcet_by_id.at(id));

    std::unordered_map<int, int> core_by_id;
    for (const auto& t : plan.tasks)
        for (const auto& st : t.subtasks)
            core_by_id[st.id] = st.core;

    std::unordered_set<int> path_cores;
    for (int id : path)
        path_cores.insert(core_by_id.at(id));

    // Per higher-priority task (pi(tau_j) < pi(task)): everything Eq. 3
    // needs that does NOT depend on R is precomputed once, outside the
    // iteration loop -- only the ceil(R/T_j) factor changes per step.
    struct HpInfo {
        double sum_in_rho; // sum C_j,t over subtasks whose core is in rho(path)
        double omega;      // Eq. 4, summed over tau_j's own Str structures
        uint64_t period_us;
    };
    std::vector<HpInfo> hp_infos;
    for (const auto& t : plan.tasks) {
        if (t.id == task.id || t.priority >= task.priority)
            continue;
        auto t_reduced = ied::remove_invalid_edges(t.subtasks, ied::task_connections(plan, t));

        double sum_in_rho = 0.0;
        for (const auto& st : t.subtasks)
            if (path_cores.count(core_by_id.at(st.id)))
                sum_in_rho += static_cast<double>(st.wcet_us);

        HpInfo info;
        info.sum_in_rho = sum_in_rho;
        info.omega = omega_sum(t.subtasks, t_reduced, path, core_by_id);
        info.period_us = t.subtasks.front().period_us;
        hp_infos.push_back(info);
    }

    double deadline_us = static_cast<double>(task_subtasks.front().deadline_us);

    double R = L + self_sum;
    for (int iter = 0; iter < kMaxIterations; ++iter) {
        double interference = 0.0;
        for (const auto& info : hp_infos) {
            double ceil_term = std::ceil(R / static_cast<double>(info.period_us));
            interference += ceil_term * (info.sum_in_rho - info.omega);
        }
        double new_R = L + self_sum + interference;

        if (std::fabs(new_R - R) < kEpsilon)
            return new_R; // converged
        if (new_R > deadline_us)
            return new_R; // already proven unschedulable; no need to keep iterating
        R = new_R;
    }
    throw std::runtime_error(
        "rrc::response_time_of_path: fixed-point iteration did not converge within " +
        std::to_string(kMaxIterations) + " iterations");
}

} // namespace detail

/** @copydoc compute_wcrt */
double compute_wcrt(const DeploymentPlan& plan, const TaskInfo& task) {
    auto reduced = ied::remove_invalid_edges(task.subtasks, ied::task_connections(plan, task));
    auto paths = detail::enumerate_complete_paths(task.subtasks, reduced);

    double max_r = 0.0;
    for (const auto& path : paths) {
        double r = detail::response_time_of_path(plan, task, path, task.subtasks, reduced);
        max_r = std::max(max_r, r);
    }
    return max_r;
}

/** @copydoc check_schedulability */
std::vector<int> check_schedulability(const DeploymentPlan& plan) {
    std::vector<int> unschedulable;
    for (const auto& task : plan.tasks) {
        double r = compute_wcrt(plan, task);
        double deadline_us = static_cast<double>(task.subtasks.front().deadline_us);
        if (r > deadline_us)
            unschedulable.push_back(task.id);
    }
    return unschedulable;
}

} // namespace rrc

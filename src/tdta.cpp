#include "tdta.hpp"
#include "dag.hpp"
#include "ied.hpp"
#include "eru.hpp"
#include <algorithm>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace tdta {

namespace detail {

/** @copydoc compute_levels */
std::unordered_map<int, int> compute_levels(
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

    std::unordered_map<int, int> level;
    // Processing nodes in topological order guarantees every predecessor's
    // level is already known by the time a node is reached (Eq. 5).
    for (int id : dag.topological_sort()) {
        const DAG::Node* node = node_by_id.at(id);
        if (node->predecessors.empty()) {
            level[id] = 0;
            continue;
        }
        int max_pred_level = 0;
        for (int pred : node->predecessors)
            max_pred_level = std::max(max_pred_level, level.at(pred));
        level[id] = max_pred_level + 1;
    }
    return level;
}

/** @copydoc compute_earliest_start_times */
std::unordered_map<int, double> compute_earliest_start_times(
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

    std::unordered_map<int, double> wcet_by_id;
    for (const auto& st : task_subtasks)
        wcet_by_id[st.id] = static_cast<double>(st.wcet_us);

    std::unordered_map<int, double> delta;
    for (int id : dag.topological_sort()) {
        const DAG::Node* node = node_by_id.at(id);
        if (node->predecessors.empty()) {
            delta[id] = 0.0;
            continue;
        }
        double max_finish = 0.0;
        for (int pred : node->predecessors)
            max_finish = std::max(max_finish, delta.at(pred) + wcet_by_id.at(pred));
        delta[id] = max_finish;
    }
    return delta;
}

/** @copydoc find_str_structures */
std::vector<std::vector<int>> find_str_structures(
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

    std::vector<int> ids;
    ids.reserve(task_subtasks.size());
    for (const auto& st : task_subtasks)
        ids.push_back(st.id);
    std::sort(ids.begin(), ids.end());

    std::vector<std::vector<int>> structures;
    for (int p : ids) {
        std::vector<int> members;
        for (int c : node_by_id.at(p)->successors) {
            const auto& c_preds = node_by_id.at(c)->predecessors;
            if (c_preds.size() == 1 && c_preds[0] == p)
                members.push_back(c);
        }
        if (members.size() >= 2)
            structures.push_back(std::move(members));
    }
    return structures;
}

namespace {

double min_delta(const std::vector<int>& ids, const std::unordered_map<int, double>& delta) {
    double m = delta.at(ids.front());
    for (int id : ids)
        m = std::min(m, delta.at(id));
    return m;
}

} // namespace

/** @copydoc allocate_task */
void allocate_task(DeploymentPlan& plan, const TaskInfo& task) {
    auto reduced = ied::remove_invalid_edges(task.subtasks, ied::task_connections(plan, task));
    auto levels = detail::compute_levels(task.subtasks, reduced);
    auto delta = detail::compute_earliest_start_times(task.subtasks, reduced);
    auto structures = detail::find_str_structures(task.subtasks, reduced);

    // subtask id -> index into `structures`, for subtasks that belong to one.
    std::unordered_map<int, int> structure_of;
    for (size_t i = 0; i < structures.size(); ++i)
        for (int id : structures[i])
            structure_of[id] = static_cast<int>(i);

    // Topology (levels/delta/Str) is computed over every subtask above --
    // dependency structure doesn't care about assignment state. But only
    // CORE_UNASSIGNED subtasks may actually be (re)placed: a subtask with
    // core already set is pinned and must be left untouched, exactly like
    // dru::detail::worst_fit_place / eru::apply_eru_allocation already
    // behave, and what allocator::apply_auto_allocation's own contract
    // promises ("assigns a core to every subtask with core ==
    // CORE_UNASSIGNED"). Its utilization still counts against its core's
    // remaining capacity -- that happens automatically, inside
    // eru::detail::equilibrium_remaining_utilization_place's own capacity
    // scan of the whole plan.
    std::unordered_map<int, const SubtaskInfo*> subtask_by_id;
    for (const auto& st : task.subtasks)
        subtask_by_id[st.id] = &st;
    auto unassigned_only = [&](const std::vector<int>& ids) {
        std::vector<int> result;
        for (int id : ids)
            if (subtask_by_id.at(id)->core == CORE_UNASSIGNED)
                result.push_back(id);
        return result;
    };

    int max_level = 0;
    for (const auto& [id, lvl] : levels)
        max_level = std::max(max_level, lvl);

    for (int lvl = 0; lvl <= max_level; ++lvl) {
        std::vector<int> level_members;
        for (const auto& [id, l] : levels)
            if (l == lvl)
                level_members.push_back(id);
        std::sort(level_members.begin(), level_members.end());

        std::vector<int> structure_indices_here;
        std::unordered_set<int> seen_structures;
        std::vector<int> leftover;
        for (int id : level_members) {
            auto it = structure_of.find(id);
            if (it != structure_of.end()) {
                if (seen_structures.insert(it->second).second)
                    structure_indices_here.push_back(it->second);
            } else {
                leftover.push_back(id);
            }
        }

        // Ascending by min earliest-start-time within the group; ties keep
        // `structures`' own order (ascending parent id) via stable_sort.
        std::stable_sort(structure_indices_here.begin(), structure_indices_here.end(),
                          [&](int a, int b) {
                              return min_delta(structures[a], delta) < min_delta(structures[b], delta);
                          });

        for (int si : structure_indices_here) {
            auto group = unassigned_only(structures[si]);
            if (!group.empty())
                eru::detail::equilibrium_remaining_utilization_place(plan, group);
        }

        auto leftover_unassigned = unassigned_only(leftover);
        if (!leftover_unassigned.empty())
            eru::detail::equilibrium_remaining_utilization_place(plan, leftover_unassigned);
    }
}

} // namespace detail

/** @copydoc apply_tdta_allocation */
void apply_tdta_allocation(DeploymentPlan& plan) {
    if (plan.tasks.size() > 1) {
        for (const auto& task : plan.tasks)
            if (task.priority == TASK_PRIORITY_UNSET)
                throw std::runtime_error(
                    "tdta::apply_tdta_allocation: task " + std::to_string(task.id) +
                    " has no priority set, but the plan has " + std::to_string(plan.tasks.size()) +
                    " tasks -- TaskInfo::priority is required (ambiguous processing order "
                    "otherwise) whenever more than one task exists");
    }

    std::vector<const TaskInfo*> ordered;
    ordered.reserve(plan.tasks.size());
    for (const auto& task : plan.tasks)
        ordered.push_back(&task);
    // Ascending: smaller TaskInfo::priority = higher priority (paper Sect. III).
    std::stable_sort(ordered.begin(), ordered.end(), [](const TaskInfo* a, const TaskInfo* b) {
        return a->priority < b->priority;
    });

    for (const TaskInfo* task : ordered)
        detail::allocate_task(plan, *task);
}

} // namespace tdta

#include "ied.hpp"
#include "dag.hpp"
#include <queue>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace ied {

namespace {

// BFS reachability from start over dag's successor edges (the paper's
// "constrains" relation): the set of node ids reachable from start,
// start itself excluded.
std::unordered_set<int> reachable_from(int start,
                                        const std::unordered_map<int, const DAG::Node*>& node_by_id) {
    std::unordered_set<int> visited;
    std::queue<int> frontier;
    frontier.push(start);
    while (!frontier.empty()) {
        int cur = frontier.front();
        frontier.pop();
        auto it = node_by_id.find(cur);
        if (it == node_by_id.end())
            continue;
        for (int succ : it->second->successors)
            if (visited.insert(succ).second)
                frontier.push(succ);
    }
    return visited;
}

} // namespace

/** @copydoc task_connections */
std::vector<ConnectionInfo> task_connections(const DeploymentPlan& plan, const TaskInfo& task) {
    std::unordered_set<int> task_ids;
    for (const auto& st : task.subtasks)
        task_ids.insert(st.id);

    std::vector<ConnectionInfo> result;
    for (const auto& c : plan.connections)
        if (task_ids.count(c.upstream) && task_ids.count(c.downstream))
            result.push_back(c);
    return result;
}

/** @copydoc remove_invalid_edges */
std::vector<ConnectionInfo> remove_invalid_edges(
    const std::vector<SubtaskInfo>& task_subtasks,
    const std::vector<ConnectionInfo>& task_connections) {

    DAG dag;
    for (const auto& st : task_subtasks)
        dag.add_node(st.id);
    for (const auto& c : task_connections)
        dag.add_edge(c.upstream, c.downstream);

    std::unordered_map<int, const DAG::Node*> node_by_id;
    for (const auto& node : dag.nodes())
        node_by_id[node.id] = &node;

    // "Constrains" computed once, on this original (pre-removal) graph --
    // see this file's header comment for why that's equivalent to
    // recomputing it incrementally (Theorem 1).
    std::unordered_map<int, std::unordered_set<int>> reachable;
    for (const auto& st : task_subtasks)
        reachable[st.id] = reachable_from(st.id, node_by_id);

    std::set<std::pair<int, int>> invalid_edges; // (upstream, downstream)
    for (const auto& node : dag.nodes()) {
        if (node.predecessors.size() < 2)
            continue;
        for (int a : node.predecessors) {
            for (int b : node.predecessors) {
                if (a == b)
                    continue;
                if (reachable.at(a).count(b)) // a constrains b
                    invalid_edges.emplace(a, node.id);
            }
        }
    }

    std::vector<ConnectionInfo> reduced;
    reduced.reserve(task_connections.size());
    for (const auto& c : task_connections)
        if (!invalid_edges.count({c.upstream, c.downstream}))
            reduced.push_back(c);
    return reduced;
}

} // namespace ied

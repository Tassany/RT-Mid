// Standalone test (no framework, no CMake): verifies DAG supports arbitrary
// shapes (multiple sources, multiple sinks, parallel intermediate branches),
// as MCFlow's system model claims and RT-Mid's scheduling code assumes.
// Build: g++ -std=c++17 -Isrc tests/dag_arbitrary_shapes_test.cpp src/dag.cpp -o /tmp/dag_test

#include <cassert>
#include <iostream>
#include <map>
#include <vector>
#include "dag.hpp"

// Graph under test: 2 sources, 3 intermediate nodes (one with fan-in 2 AND
// fan-out 2), 2 sinks.
//
//   1(S1) --> 3(A) --> 6(T1)
//   1(S1) --> 4(B) --> 6(T1)
//   2(S2) --> 4(B) --> 7(T2)
//   2(S2) --> 5(C) --> 7(T2)
static DAG build_graph() {
    DAG dag;
    for (int id : {1, 2, 3, 4, 5, 6, 7})
        dag.add_node(id, nullptr);
    dag.add_edge(1, 3);
    dag.add_edge(1, 4);
    dag.add_edge(2, 4);
    dag.add_edge(2, 5);
    dag.add_edge(3, 6);
    dag.add_edge(4, 6);
    dag.add_edge(4, 7);
    dag.add_edge(5, 7);
    return dag;
}

static void expect(bool cond, const char* what) {
    if (!cond) {
        std::cerr << "FAIL: " << what << "\n";
        std::exit(1);
    }
    std::cerr << "ok:   " << what << "\n";
}

int main() {
    DAG dag = build_graph();

    expect(!dag.has_cycle(), "multi-source/multi-sink graph has no cycle");

    // fan_in/fan_out per node, and the MCFlow role each implies.
    std::map<int, std::pair<int,int>> expected_degrees = {
        {1, {0, 2}}, {2, {0, 2}},           // sources
        {3, {1, 1}}, {4, {2, 2}}, {5, {1, 1}}, // intermediate
        {6, {2, 0}}, {7, {2, 0}},           // sinks
    };
    for (const auto& [id, deg] : expected_degrees) {
        auto [fi, fo] = deg;
        expect(dag.fan_in_count(id)  == fi, "fan_in_count matches expected");
        expect(dag.fan_out_count(id) == fo, "fan_out_count matches expected");
    }

    // Two independent sources (1, 2) must both surface as roots: no crash,
    // no silent drop — topological_sort must place both before every node
    // that depends on them, and must include every node exactly once.
    std::vector<int> order = dag.topological_sort();
    expect(order.size() == 7, "topological_sort returns all 7 nodes");

    std::map<int, int> position;
    for (std::size_t i = 0; i < order.size(); ++i) position[order[i]] = static_cast<int>(i);

    static const std::vector<std::pair<int,int>> edges = {
        {1,3},{1,4},{2,4},{2,5},{3,6},{4,6},{4,7},{5,7}
    };
    for (auto [from, to] : edges) {
        expect(position.at(from) < position.at(to),
               "topological order respects every edge (both sources honored)");
    }

    // Longest path (by node count) in this graph is 3 (e.g. 1 -> 4 -> 6).
    expect(dag.pipeline_depth() == 3, "pipeline_depth matches the longest path");

    // A 65th predecessor on one node must be rejected elsewhere (Dispatcher's
    // 64-bit fan_in_mask, checked in TeamManager::initialize) — that limit is
    // the one deliberate constraint on "any DAG structure"; DAG itself places
    // no cap, confirmed here directly.
    {
        DAG wide;
        wide.add_node(0, nullptr);
        for (int i = 1; i <= 65; ++i) {
            wide.add_node(i, nullptr);
            wide.add_edge(i, 0);
        }
        expect(wide.fan_in_count(0) == 65,
               "DAG itself does not cap fan-in (the 64-bit mask limit lives in "
               "TeamManager/Dispatcher, not here)");
    }

    std::cerr << "\nAll checks passed.\n";
    return 0;
}

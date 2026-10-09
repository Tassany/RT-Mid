// Unit tests for the TDTA allocator (ied::/tdta::* and the "tdta"
// dispatcher branch) -- Algorithms 1 and 3, Wu et al. 2023. See
// specs/tdta-allocator/spec.md/plan.md/tasks.md for the algorithm each
// case checks against, and this file's own comments for the worked
// example (paper Fig. 2 -> Fig. 3) traced by hand there.
//
// Build: g++ -std=c++17 -Wall -Wextra -Isrc -Iinclude tests/tdta_test.cpp -o /tmp/tdta_test

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
#include "allocator.hpp"
#include "deployment_plan.hpp"
#include "ied.hpp"
#include "tdta.hpp"

static int g_failures = 0;

static void expect(bool cond, const std::string& what) {
    if (!cond) { std::cerr << "FAIL: " << what << "\n"; ++g_failures; return; }
    std::cerr << "ok:   " << what << "\n";
}

static bool nearly_equal(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) < eps;
}

static SubtaskInfo make_subtask(int id, uint64_t wcet_us = 100, uint64_t period_us = 100) {
    SubtaskInfo st;
    st.id = id;
    st.component_type = "intermediate";
    st.priority = 0;
    st.period_us = period_us;
    st.deadline_us = period_us;
    st.wcet_us = wcet_us;
    return st;
}

static ConnectionInfo make_connection(int upstream, int downstream) {
    ConnectionInfo c;
    c.upstream = upstream;
    c.downstream = downstream;
    return c;
}

static bool has_edge(const std::vector<ConnectionInfo>& edges, int upstream, int downstream) {
    for (const auto& c : edges)
        if (c.upstream == upstream && c.downstream == downstream)
            return true;
    return false;
}

// Paper's own worked example (Fig. 2 -> Fig. 3), reused from Phase 3
// onward: V1..V7, Ti=100, WCETs 3,5,2,5,3,2,2; edges already post-IED
// (e(1,4) removed, per Phase 2's own test).
static std::vector<SubtaskInfo> worked_example_subtasks() {
    const uint64_t wcets[8] = {0, 3, 5, 2, 5, 3, 2, 2}; // 1-indexed
    std::vector<SubtaskInfo> subtasks;
    for (int i = 1; i <= 7; ++i)
        subtasks.push_back(make_subtask(i, wcets[i], /*period_us=*/100));
    return subtasks;
}

static std::vector<ConnectionInfo> worked_example_reduced_edges() {
    return {
        make_connection(1, 2), make_connection(1, 3),
        make_connection(2, 4), make_connection(2, 5), make_connection(2, 6),
        make_connection(3, 6),
        make_connection(4, 7), make_connection(5, 7), make_connection(6, 7),
    };
}

// Original (pre-IED) edges, including e(1,4) -- allocate_task runs IED
// itself, so end-to-end tests hand it the original graph.
static std::vector<ConnectionInfo> worked_example_original_edges() {
    return {
        make_connection(1, 2), make_connection(1, 3), make_connection(1, 4),
        make_connection(2, 4), make_connection(2, 5), make_connection(2, 6),
        make_connection(3, 6),
        make_connection(4, 7), make_connection(5, 7), make_connection(6, 7),
    };
}

// ---------------------------------------------------------------------
// Phase 1: TaskInfo::priority / TASK_PRIORITY_UNSET
// ---------------------------------------------------------------------

static void test_task_priority_default_unset() {
    TaskInfo task;
    task.id = 1;
    expect(task.priority == TASK_PRIORITY_UNSET,
           "a default-constructed TaskInfo::priority is TASK_PRIORITY_UNSET");
}

// ---------------------------------------------------------------------
// Phase 2: ied::remove_invalid_edges
// ---------------------------------------------------------------------

// Paper's own worked example (Fig. 2): V1..V7, edges
// 1->2,1->3,1->4,2->4,2->5,2->6,3->6,4->7,5->7,6->7. IED must remove
// exactly e(1,4): V4's predecessors are {1,2}, and 1 constrains 2 (direct
// edge 1->2), so 1's edge into 4 is redundant with 1->2->4.
static void test_ied_worked_example_removes_exactly_one_edge() {
    std::vector<SubtaskInfo> subtasks;
    for (int i = 1; i <= 7; ++i) subtasks.push_back(make_subtask(i));
    std::vector<ConnectionInfo> edges = {
        make_connection(1, 2), make_connection(1, 3), make_connection(1, 4),
        make_connection(2, 4), make_connection(2, 5), make_connection(2, 6),
        make_connection(3, 6), make_connection(4, 7), make_connection(5, 7),
        make_connection(6, 7),
    };

    auto reduced = ied::remove_invalid_edges(subtasks, edges);

    expect(reduced.size() == 9, "worked example: exactly one edge removed (10 -> 9)");
    expect(!has_edge(reduced, 1, 4), "worked example: e(1,4) is removed");
    expect(has_edge(reduced, 1, 2) && has_edge(reduced, 1, 3) && has_edge(reduced, 2, 4) &&
           has_edge(reduced, 2, 5) && has_edge(reduced, 2, 6) && has_edge(reduced, 3, 6) &&
           has_edge(reduced, 4, 7) && has_edge(reduced, 5, 7) && has_edge(reduced, 6, 7),
           "worked example: every other edge survives untouched");
}

// Plain diamond 1->2,1->3,2->4,3->4: node 4's predecessors {2,3} -- neither
// constrains the other (no path 2->3 or 3->2) -- nothing should be removed.
static void test_ied_no_invalid_edges_is_a_no_op() {
    std::vector<SubtaskInfo> subtasks = {make_subtask(1), make_subtask(2), make_subtask(3), make_subtask(4)};
    std::vector<ConnectionInfo> edges = {
        make_connection(1, 2), make_connection(1, 3), make_connection(2, 4), make_connection(3, 4),
    };

    auto reduced = ied::remove_invalid_edges(subtasks, edges);

    expect(reduced.size() == 4, "diamond: no edge removed (a plain fork/join has no invalid edge)");
}

// 1->2,1->3,2->3,2->4,3->4: node 3's predecessors {1,2}, 1 constrains 2
// (direct edge) -> e(1,3) invalid. Node 4's predecessors {2,3}, 2
// constrains 3 (direct edge) -> e(2,4) invalid. Both removals are based
// on the ORIGINAL graph's reachability, not a partially-reduced one --
// this is the case that would catch a recompute-mid-algorithm bug.
static void test_ied_two_independent_removals() {
    std::vector<SubtaskInfo> subtasks = {make_subtask(1), make_subtask(2), make_subtask(3), make_subtask(4)};
    std::vector<ConnectionInfo> edges = {
        make_connection(1, 2), make_connection(1, 3), make_connection(2, 3),
        make_connection(2, 4), make_connection(3, 4),
    };

    auto reduced = ied::remove_invalid_edges(subtasks, edges);

    expect(reduced.size() == 3, "two independent invalid edges removed (5 -> 3)");
    expect(!has_edge(reduced, 1, 3), "e(1,3) removed (1 constrains 2, 2 is another predecessor of 3)");
    expect(!has_edge(reduced, 2, 4), "e(2,4) removed (2 constrains 3, 3 is another predecessor of 4)");
    expect(has_edge(reduced, 1, 2) && has_edge(reduced, 2, 3) && has_edge(reduced, 3, 4),
           "the resulting linear chain 1->2->3->4 survives intact");
}

// ---------------------------------------------------------------------
// Phase 3: tdta::detail::compute_levels
// ---------------------------------------------------------------------

static void test_compute_levels_worked_example() {
    auto levels = tdta::detail::compute_levels(worked_example_subtasks(), worked_example_reduced_edges());
    expect(levels.at(1) == 0, "worked example: V1 (source) is level 0");
    expect(levels.at(2) == 1 && levels.at(3) == 1, "worked example: V2, V3 are level 1");
    expect(levels.at(4) == 2 && levels.at(5) == 2 && levels.at(6) == 2,
           "worked example: V4, V5, V6 are level 2 (V6 = max(L(2),L(3))+1 = max(1,1)+1)");
    expect(levels.at(7) == 3, "worked example: V7 (sink) is level 3");
}

static void test_compute_levels_trivial_single_edge() {
    std::vector<SubtaskInfo> subtasks = {make_subtask(1), make_subtask(2)};
    std::vector<ConnectionInfo> edges = {make_connection(1, 2)};
    auto levels = tdta::detail::compute_levels(subtasks, edges);
    expect(levels.at(1) == 0 && levels.at(2) == 1, "trivial: source level 0, its only successor level 1");
}

// ---------------------------------------------------------------------
// Phase 4: tdta::detail::compute_earliest_start_times
// ---------------------------------------------------------------------

static void test_compute_earliest_start_times_worked_example() {
    auto delta = tdta::detail::compute_earliest_start_times(worked_example_subtasks(),
                                                              worked_example_reduced_edges());
    // Hand-traced: d1=0; d2=d3=d1+C1=3; d4=d5=d2+C2=3+5=8;
    // d6=max(d2+C2,d3+C3)=max(8,3+2)=8; d7=max(d4+C4,d5+C5,d6+C6)=max(13,11,10)=13.
    expect(nearly_equal(delta.at(1), 0), "worked example: delta(V1) = 0");
    expect(nearly_equal(delta.at(2), 3) && nearly_equal(delta.at(3), 3),
           "worked example: delta(V2) = delta(V3) = 3");
    expect(nearly_equal(delta.at(4), 8) && nearly_equal(delta.at(5), 8) && nearly_equal(delta.at(6), 8),
           "worked example: delta(V4) = delta(V5) = delta(V6) = 8");
    expect(nearly_equal(delta.at(7), 13), "worked example: delta(V7) = 13");
}

// ---------------------------------------------------------------------
// Phase 5: tdta::detail::find_str_structures
// ---------------------------------------------------------------------

static bool contains(const std::vector<int>& v, int x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}

static void test_find_str_structures_worked_example() {
    auto structures = tdta::detail::find_str_structures(worked_example_subtasks(),
                                                          worked_example_reduced_edges());

    expect(structures.size() == 2, "worked example: exactly two Str structures found");
    if (structures.size() == 2) {
        expect(structures[0].size() == 2 && contains(structures[0], 2) && contains(structures[0], 3),
               "worked example: first structure is xi_1 = {2,3} (parent V1, ascending-id order)");
        expect(structures[1].size() == 2 && contains(structures[1], 4) && contains(structures[1], 5),
               "worked example: second structure is xi_2 = {4,5} (parent V2)");
    }

    bool v1_in_any = false, v6_in_any = false, v7_in_any = false;
    for (const auto& s : structures) {
        if (contains(s, 1)) v1_in_any = true;
        if (contains(s, 6)) v6_in_any = true;
        if (contains(s, 7)) v7_in_any = true;
    }
    expect(!v1_in_any && !v6_in_any && !v7_in_any,
           "worked example: V1, V6, V7 belong to no Str structure");
}

// ---------------------------------------------------------------------
// Phase 6: tdta::detail::allocate_task
// ---------------------------------------------------------------------

// End-to-end, 2 cores: hand-traced in specs/tdta-allocator/tasks.md
// Phase 6. Groups processed in order: {1} (level0 leftover), {2,3}
// (xi_1, level1), {4,5} (xi_2, level2), {6} (level2 leftover), {7}
// (level3 leftover) -- the level-1 leftover group is empty (both level-1
// members belong to xi_1) and must be skipped, not called as a no-op.
static void test_allocate_task_worked_example() {
    DeploymentPlan plan;
    TaskInfo task;
    task.id = 1;
    task.subtasks = worked_example_subtasks();
    plan.tasks.push_back(task);
    plan.connections = worked_example_original_edges();
    plan.allocation.num_cores = 2;

    tdta::detail::allocate_task(plan, plan.tasks[0]);

    const int expected_core[8] = {-1, 0, 1, 0, 0, 1, 1, 0}; // 1-indexed
    for (int i = 1; i <= 7; ++i) {
        int core = -999;
        for (const auto& st : plan.tasks[0].subtasks)
            if (st.id == i) core = st.core;
        expect(core == expected_core[i],
               "allocate_task worked example: V" + std::to_string(i) + " -> core" +
               std::to_string(expected_core[i]) + " (got " + std::to_string(core) + ")");
    }
}

// allocate_task must never touch a subtask that already has core !=
// CORE_UNASSIGNED -- same convention dru::detail::worst_fit_place and
// eru::apply_eru_allocation already follow, and what
// allocator::apply_auto_allocation's own Doxygen promises ("assigns a
// core to every subtask with core == CORE_UNASSIGNED"). Ls->{L0,L1}->Lm,
// Ls pre-pinned to core2 (reducing its remaining capacity to 0.9, while
// cores 0,1,3 stay at 1.0): Ls must stay on core2 untouched; the leftover
// group at level 0 containing only Ls must be skipped entirely (not
// passed to ERU as a no-op call that could reassign it).
static void test_allocate_task_respects_pre_assigned_subtasks() {
    DeploymentPlan plan;
    TaskInfo task;
    task.id = 1;
    task.subtasks = {
        make_subtask(1, 100, /*period_us=*/1000), // Ls
        make_subtask(2, 300, 1000),                // L0
        make_subtask(3, 200, 1000),                // L1
        make_subtask(4, 100, 1000),                // Lm
    };
    task.subtasks[0].core = 2; // Ls pre-pinned to core2
    plan.tasks.push_back(task);
    plan.connections = {make_connection(1, 2), make_connection(1, 3),
                         make_connection(2, 4), make_connection(3, 4)};
    plan.allocation.num_cores = 4;

    tdta::detail::allocate_task(plan, plan.tasks[0]);

    auto core_of_local = [&](int id) {
        for (const auto& st : plan.tasks[0].subtasks)
            if (st.id == id) return st.core;
        return -999;
    };
    expect(core_of_local(1) == 2, "pre-assigned: Ls stays on its pinned core2, untouched by allocate_task");
    expect(core_of_local(2) == 0, "pre-assigned: L0 -> core0 (top-2 by remaining capacity among the 3 free cores)");
    expect(core_of_local(3) == 1, "pre-assigned: L1 -> core1");
    expect(core_of_local(4) == 3, "pre-assigned: Lm -> core3 (the one core nothing else touched yet)");
}

// ---------------------------------------------------------------------
// Phase 7: tdta::apply_tdta_allocation + dispatcher wiring
// ---------------------------------------------------------------------

static int core_of(DeploymentPlan& plan, int id) {
    for (auto& task : plan.tasks)
        for (auto& st : task.subtasks)
            if (st.id == id)
                return st.core;
    return -999;
}

// Two single-subtask tasks, 2 cores. TASK_LOW (priority 0, higher
// priority -- smaller value wins) must be fully placed before TASK_HIGHNUM
// (priority 1): its subtask lands on core0 (both cores tied at start,
// lowest index wins); TASK_HIGHNUM's subtask then lands on core1, because
// by the time it's placed, core0's real remaining capacity has already
// dropped below core1's. Wrong (descending) order would swap these.
static void test_apply_tdta_allocation_priority_order() {
    DeploymentPlan plan;
    TaskInfo low;
    low.id = 1;
    low.priority = 0;
    low.subtasks = {make_subtask(100, 900, 1000)}; // utilization 0.9
    TaskInfo highnum;
    highnum.id = 2;
    highnum.priority = 1;
    highnum.subtasks = {make_subtask(200, 900, 1000)}; // utilization 0.9
    plan.tasks = {low, highnum};
    plan.allocation.num_cores = 2;

    tdta::apply_tdta_allocation(plan);

    expect(core_of(plan, 100) == 0, "priority order: TASK_LOW (priority 0) placed first -> core0");
    expect(core_of(plan, 200) == 1,
           "priority order: TASK_HIGHNUM (priority 1) placed second -> core1 "
           "(core0's real remaining capacity already dropped by then)");
}

// Two tasks, one missing its priority -- must throw before any placement.
static void test_apply_tdta_allocation_missing_priority_throws() {
    DeploymentPlan plan;
    TaskInfo a;
    a.id = 1;
    a.priority = 0;
    a.subtasks = {make_subtask(100)};
    TaskInfo b;
    b.id = 2;
    // b.priority left at TASK_PRIORITY_UNSET
    b.subtasks = {make_subtask(200)};
    plan.tasks = {a, b};
    plan.allocation.num_cores = 2;

    bool threw = false;
    try {
        tdta::apply_tdta_allocation(plan);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    expect(threw, "missing priority (>1 task): throws std::runtime_error");
    expect(core_of(plan, 100) == CORE_UNASSIGNED && core_of(plan, 200) == CORE_UNASSIGNED,
           "missing priority: throw happens before any placement -- both subtasks untouched");
}

// A single-task plan with priority left unset must still allocate
// normally -- there's no ordering ambiguity with only one task.
static void test_apply_tdta_allocation_single_task_unset_priority_ok() {
    DeploymentPlan plan;
    TaskInfo only;
    only.id = 1;
    // only.priority left at TASK_PRIORITY_UNSET
    only.subtasks = {make_subtask(100)};
    plan.tasks = {only};
    plan.allocation.num_cores = 1;

    tdta::apply_tdta_allocation(plan);

    expect(core_of(plan, 100) == 0, "single task, unset priority: still allocates normally");
}

// Dispatcher wiring: plan.allocation.strategy = "tdta" reaches
// tdta::apply_tdta_allocation via allocator::apply_auto_allocation.
static void test_dispatcher_reaches_tdta() {
    DeploymentPlan plan;
    TaskInfo task;
    task.id = 1;
    task.subtasks = {make_subtask(100)};
    plan.tasks = {task};
    plan.allocation.strategy = "tdta";
    plan.allocation.num_cores = 1;

    allocator::apply_auto_allocation(plan);

    expect(core_of(plan, 100) == 0, "dispatcher: strategy=\"tdta\" reaches tdta::apply_tdta_allocation");
}

int main() {
    test_task_priority_default_unset();

    test_ied_worked_example_removes_exactly_one_edge();
    test_ied_no_invalid_edges_is_a_no_op();
    test_ied_two_independent_removals();

    test_compute_levels_worked_example();
    test_compute_levels_trivial_single_edge();

    test_compute_earliest_start_times_worked_example();

    test_find_str_structures_worked_example();

    test_allocate_task_worked_example();
    test_allocate_task_respects_pre_assigned_subtasks();

    test_apply_tdta_allocation_priority_order();
    test_apply_tdta_allocation_missing_priority_throws();
    test_apply_tdta_allocation_single_task_unset_priority_ok();
    test_dispatcher_reaches_tdta();

    if (g_failures > 0) {
        std::cerr << g_failures << " test(s) failed.\n";
        return 1;
    }
    std::cerr << "All checks passed.\n";
    return 0;
}

// Unit tests for dru::detail::* and dru::apply_wf_dru_allocation, plus
// allocator::apply_auto_allocation's dispatcher behavior (Worst-Fit with
// Decreasing Remaining Utilisation, Verucchi et al. 2023).
// See specs/wf-dru-allocator/spec.md for the algorithm this checks against
// and specs/wf-dru-allocator/tasks.md for what each case covers.
//
// Build: g++ -std=c++17 -Wall -Wextra -Isrc -Iinclude tests/dru_test.cpp src/dag.cpp src/parser_json.cpp src/dru.cpp src/allocator.cpp -o /tmp/dru_test

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include "allocator.hpp"
#include "dru.hpp"
#include "deployment_plan.hpp"
#include "parser_json.hpp"

static int g_failures = 0;

static void expect(bool cond, const std::string& what) {
    if (!cond) { std::cerr << "FAIL: " << what << "\n"; ++g_failures; return; }
    std::cerr << "ok:   " << what << "\n";
}

static bool nearly_equal(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) < eps;
}

// One subtask with period_us=1000 (unless overridden), so utilization ==
// wcet_us / 1000.0 for readable test numbers.
static SubtaskInfo make_subtask(int id, uint64_t wcet_us, int core = CORE_UNASSIGNED,
                                 uint64_t period_us = 1000) {
    SubtaskInfo st;
    st.id = id;
    st.component_type = "intermediate";
    st.core = core;
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

static DeploymentPlan make_plan(std::vector<SubtaskInfo> subtasks,
                                 std::vector<ConnectionInfo> connections,
                                 AllocationConfig alloc = AllocationConfig{}) {
    DeploymentPlan plan;
    TaskInfo task;
    task.id = 1;
    task.subtasks = std::move(subtasks);
    plan.tasks.push_back(std::move(task));
    plan.connections = std::move(connections);
    plan.allocation = alloc;
    return plan;
}

// ---------------------------------------------------------------------
// Phase 1: utilization + decreasing_remaining_utilization_order
// ---------------------------------------------------------------------

static void test_utilization() {
    SubtaskInfo st = make_subtask(1, 250, CORE_UNASSIGNED, 1000);
    expect(nearly_equal(allocator::detail::utilization(st), 0.25),
           "utilization: 250us/1000us == 0.25");
}

static void test_chain_remaining_utilization() {
    // A -> B -> C
    auto A = make_subtask(1, 100);
    auto B = make_subtask(2, 200);
    auto C = make_subtask(3, 300);
    DeploymentPlan plan = make_plan({A, B, C}, {make_connection(1, 2), make_connection(2, 3)});

    auto order = dru::detail::decreasing_remaining_utilization_order(plan);
    // remaining(A) = U(B) = 0.2; remaining(B) = U(C) = 0.3; remaining(C) = 0
    expect(order.size() == 3, "chain: all three subtasks ordered");
    expect(order[0] == 2 && order[1] == 1 && order[2] == 3,
           "chain: order is B(0.3), A(0.2), C(0) by descending remaining utilization");
}

static void test_fan_out_direct_successors_only() {
    // A -> B, A -> C, B -> D. A's remaining utilization must be U(B)+U(C)
    // ONLY -- D is a successor of B, not a DIRECT successor of A. D's
    // wcet is deliberately huge so a transitive-closure bug would show up
    // as A ranking first instead of B.
    auto A = make_subtask(1, 100);
    auto B = make_subtask(2, 200);
    auto C = make_subtask(3, 300);
    auto D = make_subtask(4, 900);
    DeploymentPlan plan = make_plan({A, B, C, D}, {make_connection(1, 2), make_connection(1, 3), make_connection(2, 4)});

    auto order = dru::detail::decreasing_remaining_utilization_order(plan);
    // remaining(A) = U(B)+U(C) = 0.5; remaining(B) = U(D) = 0.9;
    // remaining(C) = 0; remaining(D) = 0
    expect(order.size() == 4, "fan-out: all four subtasks ordered");
    expect(order[0] == 2, "fan-out: B ranks first (0.9, its direct successor D)");
    expect(order[1] == 1,
           "fan-out: A ranks second (0.5 = U(B)+U(C) only, NOT D's utilization too)");
    expect(order[2] == 3 && order[3] == 4,
           "fan-out: C then D, tied at 0 remaining utilization, stable original order");
}

static void test_no_successors_zero_remaining_utilization() {
    auto A = make_subtask(1, 100);
    DeploymentPlan plan = make_plan({A}, {});
    auto order = dru::detail::decreasing_remaining_utilization_order(plan);
    expect(order.size() == 1 && order[0] == 1,
           "sink node with no successors is still ordered (remaining utilization 0)");
}

static void test_already_assigned_excluded_but_counted() {
    // P -> Q (Q pre-assigned to core 0); R -> S (both unassigned).
    auto P = make_subtask(1, 100);
    auto Q = make_subtask(2, 500, /*core=*/0);
    auto R = make_subtask(3, 100);
    auto S = make_subtask(4, 100);
    DeploymentPlan plan = make_plan({P, Q, R, S}, {make_connection(1, 2), make_connection(3, 4)});

    auto order = dru::detail::decreasing_remaining_utilization_order(plan);
    expect(order.size() == 3, "already-assigned subtask (Q) excluded from the ordered list");
    expect(std::find(order.begin(), order.end(), 2) == order.end(),
           "Q (core already assigned) never appears in the ordering");
    // remaining(P) = U(Q) = 0.5 (counted despite Q being pre-assigned);
    // remaining(R) = U(S) = 0.1
    expect(order[0] == 1,
           "P ranks first: remaining utilization counts Q's utilization (0.5) even though "
           "Q is pre-assigned and excluded from the list itself");
    expect(order[1] == 3 && order[2] == 4, "R (0.1) then S (0, no successors)");
}

static void test_tie_break_stable_order() {
    auto A = make_subtask(1, 100);
    auto B = make_subtask(2, 100);
    auto C = make_subtask(3, 100);
    DeploymentPlan plan = make_plan({A, B, C}, {}); // no edges: all tied at remaining utilization 0
    auto order = dru::detail::decreasing_remaining_utilization_order(plan);
    expect(order.size() == 3 && order[0] == 1 && order[1] == 2 && order[2] == 3,
           "ties broken by stable original plan order");
}

// ---------------------------------------------------------------------
// Phase 2: worst_fit_place + apply_auto_allocation
// ---------------------------------------------------------------------

static void test_worst_fit_picks_most_remaining_capacity() {
    // core0 pre-loaded to remaining=0.4, core1 pre-loaded to remaining=0.9.
    // A subtask needing 0.3 fits on BOTH -- First-Fit would pick core0
    // (lowest index that fits); Worst-Fit must pick core1 (most remaining
    // capacity), proving the placement rule actually implemented is
    // Worst-Fit, not First-Fit.
    auto pinned0 = make_subtask(1, 600, /*core=*/0); // core0 remaining: 1.0-0.6=0.4
    auto pinned1 = make_subtask(2, 100, /*core=*/1); // core1 remaining: 1.0-0.1=0.9
    auto item    = make_subtask(3, 300);             // needs 0.3

    AllocationConfig cfg;
    cfg.num_cores = 2;
    DeploymentPlan plan = make_plan({pinned0, pinned1, item}, {}, cfg);

    dru::detail::worst_fit_place(plan, {3});
    expect(plan.tasks[0].subtasks[2].core == 1,
           "worst-fit picks core1 (0.9 remaining), not core0 (0.4 remaining, "
           "would be First-Fit's answer)");
}

static void test_pre_assigned_capacity_subtracted() {
    // Single core, pre-loaded to remaining=0.3. A subtask needing 0.5
    // must NOT fit -- if the pre-assigned subtask's utilization were not
    // subtracted from the core's starting capacity, this would wrongly
    // succeed (0.5 <= 1.0).
    auto pinned = make_subtask(1, 700, /*core=*/0); // core0 remaining: 1.0-0.7=0.3
    auto item   = make_subtask(2, 500);             // needs 0.5

    AllocationConfig cfg;
    cfg.num_cores = 1;
    DeploymentPlan plan = make_plan({pinned, item}, {}, cfg);

    bool threw = false;
    try {
        dru::detail::worst_fit_place(plan, {2});
    } catch (const std::runtime_error&) {
        threw = true;
    }
    expect(threw,
           "pre-assigned subtask's utilization is subtracted from its core's starting "
           "capacity -- placing a 0.5 item on a core with only 0.3 truly remaining throws");
}

static void test_infeasible_throws() {
    auto item = make_subtask(1, 1500); // needs 1.5, impossible on any single empty core
    AllocationConfig cfg;
    cfg.num_cores = 1;
    DeploymentPlan plan = make_plan({item}, {}, cfg);

    bool threw = false;
    try {
        dru::detail::worst_fit_place(plan, {1});
    } catch (const std::runtime_error&) {
        threw = true;
    }
    expect(threw, "a subtask needing more than any core's full capacity throws std::runtime_error");
}

static void test_rejects_unimplemented_strategy() {
    auto item = make_subtask(1, 100);
    AllocationConfig cfg;
    cfg.strategy = "first_fit"; // not implemented -- neither worst_fit nor eru
    DeploymentPlan plan = make_plan({item}, {}, cfg);

    bool threw = false;
    try {
        allocator::apply_auto_allocation(plan);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    expect(threw, "apply_auto_allocation rejects strategy != worst_fit/eru");
    expect(plan.tasks[0].subtasks[0].core == CORE_UNASSIGNED,
           "rejection happens before any placement -- subtask left untouched");
}

static void test_end_to_end_deployment_plan() {
    JsonParser parser;
    DeploymentPlan plan = parser.parse_for_codegen("plans/deployment_plan.json");

    bool all_assigned = true;
    std::unordered_map<int, double> util_by_core;
    for (auto& task : plan.tasks) {
        for (auto& st : task.subtasks) {
            if (st.core < 0) all_assigned = false;
            else util_by_core[st.core] += allocator::detail::utilization(st);
        }
    }
    expect(all_assigned, "plans/deployment_plan.json: every subtask ends up with core >= 0");

    bool no_core_overcommitted = true;
    for (const auto& [core, u] : util_by_core)
        if (u > 1.0 + 1e-9) no_core_overcommitted = false;
    expect(no_core_overcommitted,
           "plans/deployment_plan.json: no core's assigned utilization exceeds 1.0");
}

int main() {
    test_utilization();
    test_chain_remaining_utilization();
    test_fan_out_direct_successors_only();
    test_no_successors_zero_remaining_utilization();
    test_already_assigned_excluded_but_counted();
    test_tie_break_stable_order();

    test_worst_fit_picks_most_remaining_capacity();
    test_pre_assigned_capacity_subtracted();
    test_infeasible_throws();
    test_rejects_unimplemented_strategy();
    test_end_to_end_deployment_plan();

    if (g_failures > 0) {
        std::cerr << g_failures << " test(s) failed.\n";
        return 1;
    }
    std::cerr << "All checks passed.\n";
    return 0;
}

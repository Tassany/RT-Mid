// Unit tests for rta_fonseca2016::detail::* and
// rta_fonseca2016::compute_wcrt/check_schedulability (Fonseca et al.
// 2016 RTA -- see specs/rta-fonseca2016/spec.md for the algorithm and
// specs/rta-fonseca2016/plan.md for the hand-traced worked example these
// cases check against.
//
// Build: g++ -std=c++17 -Wall -Wextra -Isrc -Iinclude tests/rta_fonseca2016_test.cpp src/dag.cpp src/ied.cpp src/tdta.cpp src/rrc.cpp src/rta_fonseca2016.cpp src/allocator.cpp src/dru.cpp src/eru.cpp -o /tmp/rta_fonseca2016_test

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include "deployment_plan.hpp"
#include "rta_fonseca2016.hpp"

static int g_failures = 0;

static void expect(bool cond, const std::string& what) {
    if (!cond) { std::cerr << "FAIL: " << what << "\n"; ++g_failures; return; }
    std::cerr << "ok:   " << what << "\n";
}

static bool nearly_equal(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) < eps;
}

static SubtaskInfo make_subtask(int id, uint64_t wcet_us, int core, uint64_t period_us) {
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

// ---------------------------------------------------------------------
// Worked example (plan.md): tau_HIGH (priority 0, T=D=100, DAG) and
// tau_LOW (priority 1, T=D=50) sharing cores 0, 1, and 9.
// ---------------------------------------------------------------------

// Hs(C=1,c9) -> H1(C=2,c0), Hs -> H2(C=1,c0); H1 -> Hm(C=1,c9), H2 -> Hm.
static std::vector<SubtaskInfo> high_subtasks() {
    return {
        make_subtask(1, 1, 9, 100), // Hs
        make_subtask(2, 2, 0, 100), // H1
        make_subtask(3, 1, 0, 100), // H2
        make_subtask(4, 1, 9, 100), // Hm
    };
}
static std::vector<ConnectionInfo> high_edges() {
    return {make_connection(1, 2), make_connection(1, 3), make_connection(2, 4), make_connection(3, 4)};
}

// W1(C=2,c0,src) -> W2(C=3,c1), W1 -> W4(C=1,c0); W2 -> W3(C=1,c0,sink), W4 -> W3.
static std::vector<SubtaskInfo> low_subtasks() {
    return {
        make_subtask(11, 2, 0, 50), // W1
        make_subtask(12, 3, 1, 50), // W2
        make_subtask(13, 1, 0, 50), // W3
        make_subtask(14, 1, 0, 50), // W4
    };
}
static std::vector<ConnectionInfo> low_edges() {
    return {make_connection(11, 12), make_connection(11, 14), make_connection(12, 13), make_connection(14, 13)};
}

// ---------------------------------------------------------------------
// Phase 1: group_execution_regions
// ---------------------------------------------------------------------

static void test_group_execution_regions_no_merge() {
    // lambda1 = {W1,W2,W3}: cores 0,1,0 -- no adjacent same-core pair.
    auto regions = rta_fonseca2016::detail::group_execution_regions({11, 12, 13}, low_subtasks());
    expect(regions.size() == 3, "lambda1: 3 regions (no adjacent same-core pair to merge)");
    if (regions.size() == 3) {
        expect(regions[0].core == 0 && nearly_equal(regions[0].wcet, 2.0), "lambda1: region0 is W1 alone, core0, wcet 2");
        expect(regions[1].core == 1 && nearly_equal(regions[1].wcet, 3.0), "lambda1: region1 is W2 alone, core1, wcet 3");
        expect(regions[2].core == 0 && nearly_equal(regions[2].wcet, 1.0), "lambda1: region2 is W3 alone, core0, wcet 1");
    }
}

static void test_group_execution_regions_full_merge() {
    // lambda2 = {W1,W4,W3}: cores 0,0,0 -- all adjacent, one merged region.
    auto regions = rta_fonseca2016::detail::group_execution_regions({11, 14, 13}, low_subtasks());
    expect(regions.size() == 1, "lambda2: fully merges into 1 region (all three on core0)");
    if (regions.size() == 1) {
        expect(regions[0].core == 0, "lambda2: merged region is on core0");
        expect(nearly_equal(regions[0].wcet, 4.0), "lambda2: merged region's wcet = 2+1+1 = 4");
        expect(regions[0].subtask_ids == std::vector<int>({11, 14, 13}),
               "lambda2: merged region keeps all three original subtask ids, in path order");
    }
}

// ---------------------------------------------------------------------
// Phase 2: p_workload + self_interference_set
// ---------------------------------------------------------------------

static bool contains(const std::vector<int>& v, int x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}

static void test_p_workload() {
    // core0 carries W1(2) + W3(1) + W4(1) = 4, irrespective of any path.
    expect(nearly_equal(rta_fonseca2016::detail::p_workload(low_subtasks(), 0), 4.0),
           "p_workload(tau_LOW, core0) = W1+W3+W4 = 2+1+1 = 4");
    expect(nearly_equal(rta_fonseca2016::detail::p_workload(low_subtasks(), 1), 3.0),
           "p_workload(tau_LOW, core1) = W2 = 3");
}

static void test_self_interference_lambda1() {
    // lambda1 = {W1,W2,W3}: W4 shares core0 with the path, is NOT a path
    // member, and Theta (ancestors of W1 restricted to core0 = none,
    // W1 is the source; descendants of W3 restricted to core0 = none,
    // W3 is the sink) doesn't exclude it either -- even though W4 is
    // actually a direct successor of W1 AND direct predecessor of W3 in
    // the full DAG (a real, paper-documented pessimism: Theta only looks
    // at predecessors of the path's first-per-core subtask and
    // successors of its last-per-core subtask, not nodes sandwiched
    // between them).
    auto self1 = rta_fonseca2016::detail::self_interference_set({11, 12, 13}, low_subtasks(), low_edges());
    expect(self1.size() == 1 && contains(self1, 14),
           "lambda1: self = {W4} exactly (the documented Theta pessimism)");
}

static void test_self_interference_lambda2() {
    // lambda2 = {W1,W4,W3}: proc(lambda2) = {core0} only, and every
    // tau_LOW subtask on core0 is already IN the path -- nothing left to
    // self-interfere.
    auto self2 = rta_fonseca2016::detail::self_interference_set({11, 14, 13}, low_subtasks(), low_edges());
    expect(self2.empty(), "lambda2: self = empty (no other tau_LOW subtask on core0)");
}

// ---------------------------------------------------------------------
// Phase 3: virtual_tasks_for_core
// ---------------------------------------------------------------------

static void test_virtual_tasks_for_core() {
    TaskInfo high;
    high.id = 1;
    high.priority = 0;
    high.subtasks = high_subtasks();

    auto vts = rta_fonseca2016::detail::virtual_tasks_for_core(high, /*core=*/0, /*r_hp_task=*/5.0);
    expect(vts.size() == 2, "virtual_tasks_for_core: exactly 2 virtual tasks (H1, H2 on core0)");
    if (vts.size() == 2) {
        expect(nearly_equal(vts[0].c, 2.0) && nearly_equal(vts[0].t, 100.0) && nearly_equal(vts[0].j, 3.0),
               "virtual_tasks_for_core: H1 -> C=2, T=100, J=R-C=5-2=3");
        expect(nearly_equal(vts[1].c, 1.0) && nearly_equal(vts[1].t, 100.0) && nearly_equal(vts[1].j, 4.0),
               "virtual_tasks_for_core: H2 -> C=1, T=100, J=R-C=5-1=4");
    }
}

// ---------------------------------------------------------------------
// Phase 4: recursive path_analysis + response_time_of_path
// ---------------------------------------------------------------------

static DeploymentPlan worked_example_plan() {
    DeploymentPlan plan;
    TaskInfo high;
    high.id = 1;
    high.priority = 0;
    high.subtasks = high_subtasks();
    TaskInfo low;
    low.id = 2;
    low.priority = 1;
    low.subtasks = low_subtasks();
    plan.tasks = {high, low};
    plan.connections = high_edges();
    for (const auto& c : low_edges())
        plan.connections.push_back(c);
    return plan;
}

static void test_response_time_of_path_lambda1() {
    DeploymentPlan plan = worked_example_plan();
    double r = rta_fonseca2016::detail::response_time_of_path(plan, plan.tasks[1], {11, 12, 13},
                                                                low_subtasks(), low_edges());
    expect(nearly_equal(r, 10.0),
           "worked example: R(lambda1) = 10 (3-core recursion, Theorem 3, Theorem 2 sum)");
}

static void test_response_time_of_path_lambda2() {
    DeploymentPlan plan = worked_example_plan();
    double r = rta_fonseca2016::detail::response_time_of_path(plan, plan.tasks[1], {11, 14, 13},
                                                                low_subtasks(), low_edges());
    expect(nearly_equal(r, 7.0), "worked example: R(lambda2) = 7 (merged single region)");
}

// Dedicated coverage for the "different cores" branch (Algorithm 1 lines
// 18-28) -- neither worked-example path above exercises it directly,
// since lambda1's first/last regions share a core and lambda2 merges
// into one region. X(core5,C=2) -> Y(core6,C=3), single task (no hp, no
// self): R(X alone)=2, R(Y alone)=3, Theorem 2: R=[2-0]+[3-0]=5.
static void test_response_time_of_path_different_cores_branch() {
    DeploymentPlan plan;
    TaskInfo solo;
    solo.id = 1;
    solo.priority = 0;
    solo.subtasks = {make_subtask(21, 2, 5, 100), make_subtask(22, 3, 6, 100)};
    plan.tasks = {solo};
    plan.connections = {make_connection(21, 22)};

    double r = rta_fonseca2016::detail::response_time_of_path(plan, plan.tasks[0], {21, 22},
                                                                solo.subtasks, plan.connections);
    expect(nearly_equal(r, 5.0),
           "different-cores branch: 2-region path X(c5,C=2)->Y(c6,C=3), R=2+3=5, no hp/self");
}

// Found via tools/eval/fonseca_wcrt_eval_main.cpp on a real topology
// (TDTA's own allocation of the HIGH task produced an alternating
// core0,core1,core0,core1 path) -- not covered by the worked example
// above (lambda1 and lambda2 never alternate). X(c0,C=2)->Y(c1,C=3)
// ->Z(c0,C=1)->W(c1,C=4), single task, no hp/self: with zero external
// interference, R(path) must equal len(path) exactly (2+3+1+4=10) --
// this is what caught the original core-keyed memo's collision (it threw
// "core appears more than twice" here, even though each core appears
// exactly twice -- the range-keyed fix resolved it).
static void test_response_time_of_path_alternating_cores() {
    DeploymentPlan plan;
    TaskInfo solo;
    solo.id = 1;
    solo.priority = 0;
    solo.subtasks = {
        make_subtask(41, 2, 0, 1000), // X, core0
        make_subtask(42, 3, 1, 1000), // Y, core1
        make_subtask(43, 1, 0, 1000), // Z, core0
        make_subtask(44, 4, 1, 1000), // W, core1
    };
    plan.tasks = {solo};
    plan.connections = {make_connection(41, 42), make_connection(42, 43), make_connection(43, 44)};

    double r = rta_fonseca2016::detail::response_time_of_path(plan, plan.tasks[0], {41, 42, 43, 44},
                                                                solo.subtasks, plan.connections);
    expect(nearly_equal(r, 10.0),
           "alternating cores (c0,c1,c0,c1): R = len(path) = 2+3+1+4 = 10 exactly (zero external interference)");
}

static void test_response_time_of_path_missing_priority_throws() {
    DeploymentPlan plan = worked_example_plan();
    plan.tasks[0].priority = TASK_PRIORITY_UNSET;
    bool threw = false;
    try {
        rta_fonseca2016::detail::response_time_of_path(plan, plan.tasks[1], {11, 12, 13},
                                                         low_subtasks(), low_edges());
    } catch (const std::runtime_error&) {
        threw = true;
    }
    expect(threw, "missing priority (>1 task): throws std::runtime_error");
}

// Same engineered-magnitude lesson as rrc-analysis's own iteration-cap
// test: period_us=wcet_us=1e6 (not 1e15 -- that landed near double's
// 2^53 exact-integer limit and produced a spurious early convergence
// there), deadline_us=2e10, verified in Python first.
static void test_response_time_of_path_iteration_cap_throws() {
    DeploymentPlan plan;
    TaskInfo high;
    high.id = 1;
    high.priority = 0;
    high.subtasks = {make_subtask(31, 1000000ULL, 0, 1000000ULL)};
    TaskInfo low;
    low.id = 2;
    low.priority = 1;
    low.subtasks = {make_subtask(32, 1, 0, 2)};
    low.subtasks[0].deadline_us = 20000000000ULL;
    plan.tasks = {high, low};

    bool threw = false;
    try {
        rta_fonseca2016::detail::response_time_of_path(plan, plan.tasks[1], {32}, low.subtasks, {});
    } catch (const std::runtime_error&) {
        threw = true;
    }
    expect(threw, "non-convergent-but-under-deadline case: iteration cap throws std::runtime_error");
}

// ---------------------------------------------------------------------
// Phase 5: compute_wcrt + check_schedulability
// ---------------------------------------------------------------------

static void test_compute_wcrt_worked_example() {
    DeploymentPlan plan = worked_example_plan();
    expect(nearly_equal(rta_fonseca2016::compute_wcrt(plan, plan.tasks[0]), 5.0),
           "worked example: compute_wcrt(tau_HIGH) = 5");
    expect(nearly_equal(rta_fonseca2016::compute_wcrt(plan, plan.tasks[1]), 10.0),
           "worked example: compute_wcrt(tau_LOW) = max(10,7) = 10");
}

static void test_check_schedulability_worked_example() {
    DeploymentPlan plan = worked_example_plan();
    auto unschedulable = rta_fonseca2016::check_schedulability(plan);
    expect(unschedulable.empty(),
           "worked example: both tasks schedulable (tau_HIGH R=5<=100, tau_LOW R=10<=50)");
}

static void test_check_schedulability_unschedulable_case() {
    DeploymentPlan plan = worked_example_plan();
    for (auto& st : plan.tasks[1].subtasks)
        st.deadline_us = 8; // tau_LOW's R=10 > this shrunk deadline
    auto unschedulable = rta_fonseca2016::check_schedulability(plan);
    expect(unschedulable.size() == 1 && unschedulable[0] == 2,
           "engineered case: tau_LOW (id=2) reported unschedulable (R=10 > D=8)");
}

int main() {
    test_group_execution_regions_no_merge();
    test_group_execution_regions_full_merge();

    test_p_workload();
    test_self_interference_lambda1();
    test_self_interference_lambda2();

    test_virtual_tasks_for_core();

    test_response_time_of_path_lambda1();
    test_response_time_of_path_lambda2();
    test_response_time_of_path_different_cores_branch();
    test_response_time_of_path_alternating_cores();
    test_response_time_of_path_missing_priority_throws();
    test_response_time_of_path_iteration_cap_throws();

    test_compute_wcrt_worked_example();
    test_check_schedulability_worked_example();
    test_check_schedulability_unschedulable_case();

    if (g_failures > 0) {
        std::cerr << g_failures << " test(s) failed.\n";
        return 1;
    }
    std::cerr << "All checks passed.\n";
    return 0;
}

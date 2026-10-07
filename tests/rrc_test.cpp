// Unit tests for rrc::detail::* and rrc::compute_wcrt/check_schedulability
// (RRC WCRT analysis, Sect. IV-A Eq. 1-4, Wu et al. 2023). See
// specs/rrc-analysis/spec.md for the algorithm and specs/rrc-analysis/
// plan.md for the hand-traced worked example these cases check against.
//
// Build: g++ -std=c++17 -Wall -Wextra -Isrc -Iinclude tests/rrc_test.cpp src/dag.cpp src/ied.cpp src/tdta.cpp src/rrc.cpp src/allocator.cpp src/dru.cpp src/eru.cpp -o /tmp/rrc_test

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include "deployment_plan.hpp"
#include "rrc.hpp"

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

static bool path_equals(const std::vector<int>& path, std::vector<int> expected) {
    return path == expected;
}

static bool any_path_equals(const std::vector<std::vector<int>>& paths, std::vector<int> expected) {
    for (const auto& p : paths)
        if (path_equals(p, expected)) return true;
    return false;
}

// ---------------------------------------------------------------------
// Worked example (plan.md): tau_HIGH (priority 0, T=D=100) and
// tau_LOW (priority 1, T=D=50) sharing processors p0=0, p1=1.
// ---------------------------------------------------------------------

// V1(C=2,p0) -> V2(C=4,p0), V1 -> V3(C=3,p1), V2 -> V4(C=1,p1), V3 -> V4.
static std::vector<SubtaskInfo> high_subtasks() {
    return {
        make_subtask(1, 2, 0, 100), // V1, p0
        make_subtask(2, 4, 0, 100), // V2, p0
        make_subtask(3, 3, 1, 100), // V3, p1
        make_subtask(4, 1, 1, 100), // V4, p1
    };
}
static std::vector<ConnectionInfo> high_edges() {
    return {make_connection(1, 2), make_connection(1, 3), make_connection(2, 4), make_connection(3, 4)};
}

// W1(C=2,p0) -> W2(C=2,p1) -> W3(C=1,p0).
static std::vector<SubtaskInfo> low_subtasks() {
    return {
        make_subtask(11, 2, 0, 50), // W1, p0
        make_subtask(12, 2, 1, 50), // W2, p1
        make_subtask(13, 1, 0, 50), // W3, p0
    };
}
static std::vector<ConnectionInfo> low_edges() {
    return {make_connection(11, 12), make_connection(12, 13)};
}

// ---------------------------------------------------------------------
// Phase 2: enumerate_complete_paths + path_length
// ---------------------------------------------------------------------

static void test_enumerate_paths_low_single_chain() {
    auto paths = rrc::detail::enumerate_complete_paths(low_subtasks(), low_edges());
    expect(paths.size() == 1, "tau_LOW: exactly one complete path (a chain)");
    if (paths.size() == 1) {
        expect(path_equals(paths[0], {11, 12, 13}), "tau_LOW: the one path is W1->W2->W3");
        expect(nearly_equal(rrc::detail::path_length(paths[0], low_subtasks()), 5.0),
               "tau_LOW: L(lambda) = 2+2+1 = 5");
    }
}

static void test_enumerate_paths_high_two_branches() {
    auto paths = rrc::detail::enumerate_complete_paths(high_subtasks(), high_edges());
    expect(paths.size() == 2, "tau_HIGH: exactly two complete paths");
    expect(any_path_equals(paths, {1, 2, 4}), "tau_HIGH: path via V2 (V1->V2->V4) present");
    expect(any_path_equals(paths, {1, 3, 4}), "tau_HIGH: path via V3 (V1->V3->V4) present");
    for (const auto& p : paths) {
        double len = rrc::detail::path_length(p, high_subtasks());
        if (path_equals(p, {1, 2, 4}))
            expect(nearly_equal(len, 7.0), "tau_HIGH: L(via V2) = 2+4+1 = 7");
        if (path_equals(p, {1, 3, 4}))
            expect(nearly_equal(len, 6.0), "tau_HIGH: L(via V3) = 2+3+1 = 6");
    }
}

// ---------------------------------------------------------------------
// Phase 3: self_interference_set + path_self_interference
// ---------------------------------------------------------------------

// Diamond 1->2,1->3,2->4,3->4 (no invalid edges -- same shape already
// used in specs/tdta-allocator/tasks.md's IED tests). 2 and 3 are
// siblings with no precedence constraint between them.
static std::vector<SubtaskInfo> diamond_subtasks(int core2, int core3) {
    return {
        make_subtask(1, 1, 0, 1000),
        make_subtask(2, 1, core2, 1000),
        make_subtask(3, 1, core3, 1000),
        make_subtask(4, 1, 0, 1000),
    };
}
static std::vector<ConnectionInfo> diamond_edges() {
    return {make_connection(1, 2), make_connection(1, 3), make_connection(2, 4), make_connection(3, 4)};
}

static bool contains(const std::vector<int>& v, int x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}

static void test_self_interference_same_core_siblings() {
    auto subtasks = diamond_subtasks(/*core2=*/0, /*core3=*/0);
    auto self2 = rrc::detail::self_interference_set(2, subtasks, diamond_edges());
    auto self3 = rrc::detail::self_interference_set(3, subtasks, diamond_edges());
    expect(contains(self2, 3), "diamond, same core: self(2) contains 3");
    expect(contains(self3, 2), "diamond, same core: self(3) contains 2");
    expect(!contains(self2, 1) && !contains(self2, 4),
           "diamond, same core: self(2) excludes 1 and 4 (precedence constraint exists)");
}

static void test_self_interference_different_core_siblings() {
    auto subtasks = diamond_subtasks(/*core2=*/0, /*core3=*/1);
    auto self2 = rrc::detail::self_interference_set(2, subtasks, diamond_edges());
    auto self3 = rrc::detail::self_interference_set(3, subtasks, diamond_edges());
    expect(self2.empty(), "diamond, different cores: self(2) is empty");
    expect(self3.empty(), "diamond, different cores: self(3) is empty");
}

static void test_path_self_interference_chain_is_empty() {
    auto si = rrc::detail::path_self_interference({11, 12, 13}, low_subtasks(), low_edges());
    expect(si.empty(), "worked example: tau_LOW's chain has no self-interference at all");
}

// ---------------------------------------------------------------------
// Phase 4: omega_sum
// ---------------------------------------------------------------------

static std::unordered_map<int, int> worked_example_core_by_id() {
    return {{1, 0}, {2, 0}, {3, 1}, {4, 1}, {11, 0}, {12, 1}, {13, 0}};
}

static void test_omega_sum_worked_example() {
    double omega = rrc::detail::omega_sum(high_subtasks(), high_edges(), {11, 12, 13},
                                           worked_example_core_by_id());
    expect(nearly_equal(omega, 3.0),
           "worked example: omega_sum(xi_{1,1}={V2,V3}, lambda_{2,1}) = (2-1)*min(4,3) = 3");
}

static void test_omega_sum_empty_eta_contributes_zero() {
    std::unordered_map<int, int> core_by_id = {{1, 0}, {2, 0}, {3, 1}, {4, 1}, {100, 2}, {101, 3}};
    double omega = rrc::detail::omega_sum(high_subtasks(), high_edges(), {100, 101}, core_by_id);
    expect(nearly_equal(omega, 0.0),
           "eta=empty: a path on cores {2,3} never overlapping xi_{1,1}'s cores {0,1} contributes 0");
}

// ---------------------------------------------------------------------
// Phase 5: response_time_of_path + compute_wcrt
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

static void test_response_time_of_path_worked_example() {
    DeploymentPlan plan = worked_example_plan();
    auto reduced = low_edges();
    double r = rrc::detail::response_time_of_path(plan, plan.tasks[1], {11, 12, 13}, low_subtasks(), reduced);
    expect(nearly_equal(r, 12.0),
           "worked example: R(lambda_{2,1}) converges to exactly 12 (R0=5 -> R1=12 -> R2=12)");
}

static void test_compute_wcrt_worked_example() {
    DeploymentPlan plan = worked_example_plan();
    double r = rrc::compute_wcrt(plan, plan.tasks[1]);
    expect(nearly_equal(r, 12.0), "worked example: compute_wcrt(tau_LOW) = 12 (only one path)");
}

static void test_missing_priority_throws() {
    DeploymentPlan plan = worked_example_plan();
    plan.tasks[0].priority = TASK_PRIORITY_UNSET;
    bool threw = false;
    try {
        rrc::compute_wcrt(plan, plan.tasks[1]);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    expect(threw, "missing priority (>1 task): rrc throws std::runtime_error");
}

// Engineered to never converge and never cross the deadline within the
// iteration cap: a single hp subtask with period_us = wcet_us = 1e6
// makes R_n = 1 + n*1e6 exactly (strictly increasing, never a fixed
// point); deadline_us = 2e10 stays above R even at n=10000
// (R_10000 = 1e10+1), so the loop exhausts kMaxIterations and must
// throw rather than loop forever. Verified numerically (not just by
// hand) before use -- an earlier attempt at 1e15 accidentally landed
// near double's exact-integer limit (2^53 ~ 9e15) and produced a
// SPURIOUS floating-point "convergence" around iteration 10 instead of
// the genuine non-convergent growth intended; these much smaller
// magnitudes stay far below that edge.
static void test_iteration_cap_throws() {
    DeploymentPlan plan;
    TaskInfo high;
    high.id = 1;
    high.priority = 0;
    high.subtasks = {make_subtask(1, 1000000ULL, 0, 1000000ULL)};
    TaskInfo low;
    low.id = 2;
    low.priority = 1;
    low.subtasks = {make_subtask(2, 1, 0, 2)};
    low.subtasks[0].deadline_us = 20000000000ULL;
    plan.tasks = {high, low};

    std::vector<int> path = {2};
    bool threw = false;
    try {
        rrc::detail::response_time_of_path(plan, plan.tasks[1], path, low.subtasks, {});
    } catch (const std::runtime_error&) {
        threw = true;
    }
    expect(threw, "non-convergent-but-under-deadline case: iteration cap throws std::runtime_error");
}

// ---------------------------------------------------------------------
// Phase 6: check_schedulability
// ---------------------------------------------------------------------

static void test_check_schedulability_worked_example() {
    DeploymentPlan plan = worked_example_plan();
    auto unschedulable = rrc::check_schedulability(plan);
    expect(unschedulable.empty(),
           "worked example: both tasks schedulable (tau_HIGH R=7<=100, tau_LOW R=12<=50)");
}

static void test_check_schedulability_unschedulable_case() {
    DeploymentPlan plan = worked_example_plan();
    for (auto& st : plan.tasks[1].subtasks)
        st.deadline_us = 10; // tau_LOW's R=12 > this shrunk deadline
    auto unschedulable = rrc::check_schedulability(plan);
    expect(unschedulable.size() == 1 && unschedulable[0] == 2,
           "engineered case: tau_LOW (id=2) reported unschedulable (R=12 > D=10)");
}

int main() {
    test_enumerate_paths_low_single_chain();
    test_enumerate_paths_high_two_branches();

    test_self_interference_same_core_siblings();
    test_self_interference_different_core_siblings();
    test_path_self_interference_chain_is_empty();

    test_omega_sum_worked_example();
    test_omega_sum_empty_eta_contributes_zero();

    test_response_time_of_path_worked_example();
    test_compute_wcrt_worked_example();
    test_missing_priority_throws();
    test_iteration_cap_throws();

    test_check_schedulability_worked_example();
    test_check_schedulability_unschedulable_case();

    if (g_failures > 0) {
        std::cerr << g_failures << " test(s) failed.\n";
        return 1;
    }
    std::cerr << "All checks passed.\n";
    return 0;
}

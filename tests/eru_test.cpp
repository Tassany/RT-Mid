// Unit tests for eru::detail::equilibrium_remaining_utilization_place
// (Algorithm 2, Wu et al. 2023 -- see specs/eru-allocator/spec.md and
// specs/eru-allocator/tasks.md for what each case covers).
//
// Build: g++ -std=c++17 -Wall -Wextra -Isrc -Iinclude tests/eru_test.cpp src/dag.cpp src/parser_json.cpp src/eru.cpp src/allocator.cpp -o /tmp/eru_test

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
#include "allocator.hpp"
#include "eru.hpp"
#include "deployment_plan.hpp"

static int g_failures = 0;

static void expect(bool cond, const std::string& what) {
    if (!cond) { std::cerr << "FAIL: " << what << "\n"; ++g_failures; return; }
    std::cerr << "ok:   " << what << "\n";
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

static DeploymentPlan make_plan(std::vector<SubtaskInfo> subtasks, AllocationConfig alloc) {
    DeploymentPlan plan;
    TaskInfo task;
    task.id = 1;
    task.subtasks = std::move(subtasks);
    plan.tasks.push_back(std::move(task));
    plan.allocation = alloc;
    return plan;
}

static SubtaskInfo* find_subtask(DeploymentPlan& plan, int id) {
    for (auto& st : plan.tasks[0].subtasks)
        if (st.id == id) return &st;
    return nullptr;
}

// mu-selection: 4 cores with distinct remaining capacities (0.9, 0.5, 0.7,
// 0.3 via pre-assigned subtasks), group of 2 -- only the top-2 cores by
// remaining capacity (core0=0.9, core2=0.7) may ever receive a placement;
// core1/core3 must stay untouched by this call.
static void test_mu_selection_picks_top_cores_only() {
    auto pinned0 = make_subtask(1, 100, /*core=*/0); // core0 remaining 0.9
    auto pinned1 = make_subtask(2, 500, /*core=*/1); // core1 remaining 0.5
    auto pinned2 = make_subtask(3, 300, /*core=*/2); // core2 remaining 0.7
    auto pinned3 = make_subtask(4, 700, /*core=*/3); // core3 remaining 0.3
    auto item10  = make_subtask(10, 100);
    auto item11  = make_subtask(11, 100);

    AllocationConfig cfg;
    cfg.num_cores = 4;
    DeploymentPlan plan = make_plan({pinned0, pinned1, pinned2, pinned3, item10, item11}, cfg);

    eru::detail::equilibrium_remaining_utilization_place(plan, {10, 11});

    int c10 = find_subtask(plan, 10)->core;
    int c11 = find_subtask(plan, 11)->core;
    expect((c10 == 0 || c10 == 2) && (c11 == 0 || c11 == 2),
           "mu-selection: both placements land on {core0, core2} (top-2 by remaining capacity)");
    expect(c10 != c11,
           "mu-selection: the group's two subtasks land on two DIFFERENT cores (mu == |group|)");
}

// Virtual-vs-real distinction: core0 has much more REAL remaining capacity
// (0.9) than core1 (0.2), yet within one ERU call both start the virtual
// scratch at 1.0 -- after the large subtask lands on core0, core1's
// virtual slot (still 1.0) beats core0's (now 0.4), so the SECOND subtask
// goes to core1 despite core1's real capacity being far smaller than
// core0's real remaining capacity at that point (0.2 < 0.3). Plain
// Worst-Fit (always the single most-real-remaining-capacity core) would
// never produce this.
static void test_virtual_scratch_overrides_real_capacity_ranking() {
    auto pinned0 = make_subtask(1, 100, /*core=*/0); // core0 remaining 0.9
    auto pinned1 = make_subtask(2, 800, /*core=*/1); // core1 remaining 0.2
    auto big     = make_subtask(20, 600); // needs 0.6
    auto small   = make_subtask(21, 100); // needs 0.1

    AllocationConfig cfg;
    cfg.num_cores = 2;
    DeploymentPlan plan = make_plan({pinned0, pinned1, big, small}, cfg);

    eru::detail::equilibrium_remaining_utilization_place(plan, {20, 21});

    expect(find_subtask(plan, 20)->core == 0,
           "virtual scratch: larger subtask (0.6) placed first, on core0 (tied virtual 1.0, lowest index)");
    expect(find_subtask(plan, 21)->core == 1,
           "virtual scratch: smaller subtask (0.1) placed on core1 -- its virtual slot (1.0) still "
           "beats core0's (0.4 after the first placement), even though core1's REAL remaining "
           "capacity (0.2) is far smaller than core0's (0.3) at this point");
}

// Tie-break on core index: 3 fresh (unloaded) cores, group of 1 -- mu=1,
// all three tied at remaining capacity 1.0, must pick core0.
static void test_tie_break_core_index() {
    auto item = make_subtask(1, 100);
    AllocationConfig cfg;
    cfg.num_cores = 3;
    DeploymentPlan plan = make_plan({item}, cfg);

    eru::detail::equilibrium_remaining_utilization_place(plan, {1});

    expect(find_subtask(plan, 1)->core == 0,
           "tie-break: three equally-idle cores, lowest index (core0) picked into theta");
}

// Tie-break on WCET (stable, original group_ids order): 2 fresh cores,
// group of 2 subtasks with EQUAL WCET -- the one listed FIRST in
// group_ids must be placed first (and thus lands on core0, since both
// cores/virtual slots start tied).
static void test_tie_break_wcet_stable_order() {
    auto a = make_subtask(30, 100);
    auto b = make_subtask(31, 100);
    AllocationConfig cfg;
    cfg.num_cores = 2;
    DeploymentPlan plan = make_plan({a, b}, cfg);

    eru::detail::equilibrium_remaining_utilization_place(plan, {31, 30}); // note: 31 listed first

    expect(find_subtask(plan, 31)->core == 0,
           "WCET tie: 31 (listed first in group_ids) is placed first -> core0");
    expect(find_subtask(plan, 30)->core == 1,
           "WCET tie: 30 (listed second) is placed second -> core1");
}

// Infeasible group: single core, subtask needs more utilization (1.5)
// than any core can ever hold (1.0) -- must throw, not silently overcommit.
static void test_infeasible_throws() {
    auto item = make_subtask(1, 1500);
    AllocationConfig cfg;
    cfg.num_cores = 1;
    DeploymentPlan plan = make_plan({item}, cfg);

    bool threw = false;
    try {
        eru::detail::equilibrium_remaining_utilization_place(plan, {1});
    } catch (const std::runtime_error&) {
        threw = true;
    }
    expect(threw, "a subtask needing more than any core's full capacity throws std::runtime_error");
}

// Whole-plan entry point: every CORE_UNASSIGNED subtask across the plan
// forms one group; every one of them ends up placed, no core overcommitted.
static void test_apply_eru_allocation_whole_plan() {
    auto a = make_subtask(1, 300);
    auto b = make_subtask(2, 300);
    auto c = make_subtask(3, 300);
    AllocationConfig cfg;
    cfg.num_cores = 2;
    DeploymentPlan plan = make_plan({a, b, c}, cfg);

    eru::apply_eru_allocation(plan);

    bool all_assigned = true;
    double used0 = 0.0, used1 = 0.0;
    for (auto& st : plan.tasks[0].subtasks) {
        if (st.core == CORE_UNASSIGNED) all_assigned = false;
        else if (st.core == 0) used0 += allocator::detail::utilization(st);
        else if (st.core == 1) used1 += allocator::detail::utilization(st);
    }
    expect(all_assigned, "apply_eru_allocation: every subtask ends up with core >= 0");
    expect(used0 <= 1.0 + 1e-9 && used1 <= 1.0 + 1e-9,
           "apply_eru_allocation: no core's assigned utilization exceeds capacity");
}

// Dispatcher wiring: plan.allocation.strategy = "eru" reaches
// eru::apply_eru_allocation via allocator::apply_auto_allocation.
static void test_dispatcher_reaches_eru() {
    auto a = make_subtask(1, 300);
    AllocationConfig cfg;
    cfg.strategy = "eru";
    cfg.num_cores = 1;
    DeploymentPlan plan = make_plan({a}, cfg);

    allocator::apply_auto_allocation(plan);

    expect(find_subtask(plan, 1)->core == 0,
           "dispatcher: strategy=\"eru\" reaches eru::apply_eru_allocation (subtask placed)");
}

// Dispatcher still rejects anything that's neither "worst_fit" nor "eru".
static void test_dispatcher_rejects_unknown_strategy() {
    auto a = make_subtask(1, 100);
    AllocationConfig cfg;
    cfg.strategy = "first_fit";
    DeploymentPlan plan = make_plan({a}, cfg);

    bool threw = false;
    try {
        allocator::apply_auto_allocation(plan);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    expect(threw, "dispatcher: strategy=\"first_fit\" still throws (unimplemented)");
}

int main() {
    test_mu_selection_picks_top_cores_only();
    test_virtual_scratch_overrides_real_capacity_ranking();
    test_tie_break_core_index();
    test_tie_break_wcet_stable_order();
    test_infeasible_throws();
    test_apply_eru_allocation_whole_plan();
    test_dispatcher_reaches_eru();
    test_dispatcher_rejects_unknown_strategy();

    if (g_failures > 0) {
        std::cerr << g_failures << " test(s) failed.\n";
        return 1;
    }
    std::cerr << "All checks passed.\n";
    return 0;
}

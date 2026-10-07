#include "interference_topology.hpp"

namespace interference_topology {

/** @copydoc build_plan */
DeploymentPlan build_plan(uint64_t low_period_us, const std::string& mode) {
    DeploymentPlan plan;

    TaskInfo high;
    high.id = 1;
    high.priority = 0; // pi(tau): smaller = higher priority
    for (int i = 0; i < 8; ++i) {
        SubtaskInfo st;
        st.id = i + 1;
        st.component_type = (i == 0) ? "source" : (i == 7) ? "sink" : "intermediate";
        // SubtaskInfo::priority: the REAL SCHED_FIFO priority TeamManager
        // applies at runtime -- a separate field from TaskInfo::priority
        // above (allocation-time only).
        st.priority = 30;
        st.period_us = HIGH_PERIOD_US;
        st.deadline_us = HIGH_PERIOD_US;
        st.wcet_us = HIGH_WCET_US[i];
        st.core = (mode == "single_core") ? 0 : CORE_UNASSIGNED;
        st.config = json{{"workload_us", HIGH_WCET_US[i]}};
        high.subtasks.push_back(st);
    }
    plan.tasks.push_back(high);

    TaskInfo low;
    low.id = 2;
    low.priority = 1; // lower priority than HIGH
    for (int i = 0; i < 8; ++i) {
        SubtaskInfo st;
        st.id = 11 + i;
        st.component_type = (i == 0) ? "source" : (i == 7) ? "sink" : "intermediate";
        st.priority = 10; // real SCHED_FIFO priority; lower than HIGH's 30 above
        st.period_us = low_period_us;
        st.deadline_us = low_period_us;
        st.wcet_us = LOW_WCET_US[i];
        st.core = (mode == "single_core") ? 0 : CORE_UNASSIGNED;
        st.config = json{{"workload_us", LOW_WCET_US[i]}};
        low.subtasks.push_back(st);
    }
    plan.tasks.push_back(low);

    auto connect = [](int u, int d) {
        ConnectionInfo c;
        c.upstream = u;
        c.downstream = d;
        return c;
    };
    // HIGH: Hi(1) -> A(2),B(3); A -> A1(4),A2(5); B -> B1(6),B2(7);
    // {A1,A2,B1,B2} -> Hf(8).
    plan.connections.push_back(connect(1, 2));
    plan.connections.push_back(connect(1, 3));
    plan.connections.push_back(connect(2, 4));
    plan.connections.push_back(connect(2, 5));
    plan.connections.push_back(connect(3, 6));
    plan.connections.push_back(connect(3, 7));
    plan.connections.push_back(connect(4, 8));
    plan.connections.push_back(connect(5, 8));
    plan.connections.push_back(connect(6, 8));
    plan.connections.push_back(connect(7, 8));
    // LOW: Li(11) -> P(12),Q(13); P -> P1(14),P2(15); Q -> Q1(16),Q2(17);
    // {P1,P2,Q1,Q2} -> Lf(18). Same shape as HIGH, ids offset by +10.
    plan.connections.push_back(connect(11, 12));
    plan.connections.push_back(connect(11, 13));
    plan.connections.push_back(connect(12, 14));
    plan.connections.push_back(connect(12, 15));
    plan.connections.push_back(connect(13, 16));
    plan.connections.push_back(connect(13, 17));
    plan.connections.push_back(connect(14, 18));
    plan.connections.push_back(connect(15, 18));
    plan.connections.push_back(connect(16, 18));
    plan.connections.push_back(connect(17, 18));

    if (mode == "wf_dru") {
        plan.allocation.strategy = "worst_fit";
        plan.allocation.sort_by = "remaining_utilization_desc";
        plan.allocation.weight = "utilization";
        plan.allocation.num_cores = NUM_CORES;
        allocator::apply_auto_allocation(plan);
    } else if (mode == "eru") {
        plan.allocation.strategy = "eru";
        plan.allocation.num_cores = NUM_CORES;
        allocator::apply_auto_allocation(plan);
    } else if (mode == "tdta") {
        plan.allocation.strategy = "tdta";
        plan.allocation.num_cores = NUM_CORES;
        allocator::apply_auto_allocation(plan);
    }
    return plan;
}

} // namespace interference_topology

#include "allocator.hpp"
#include "dru.hpp"
#include "eru.hpp"
#include <stdexcept>

namespace allocator {

namespace detail {

/** @copydoc utilization */
double utilization(const SubtaskInfo& st) {
    return static_cast<double>(st.wcet_us) / static_cast<double>(st.period_us);
}

/** @copydoc build_plan_dag */
DAG build_plan_dag(const DeploymentPlan& plan) {
    DAG dag;
    for (const auto& task : plan.tasks)
        for (const auto& st : task.subtasks)
            dag.add_node(st.id);
    for (const auto& c : plan.connections)
        dag.add_edge(c.upstream, c.downstream);
    return dag;
}

} // namespace detail

/** @copydoc apply_auto_allocation */
void apply_auto_allocation(DeploymentPlan& plan) {
    const auto& cfg = plan.allocation;
    if (cfg.strategy == "worst_fit") {
        dru::apply_wf_dru_allocation(plan);
        return;
    }
    if (cfg.strategy == "eru") {
        eru::apply_eru_allocation(plan);
        return;
    }
    throw std::runtime_error(
        "allocator::apply_auto_allocation: unimplemented strategy \"" + cfg.strategy +
        "\" (only \"worst_fit\" and \"eru\" are implemented so far)");
}

} // namespace allocator

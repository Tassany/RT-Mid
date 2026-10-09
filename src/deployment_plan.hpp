#pragma once
#ifndef DEPLOYMENT_PLAN_HPP
#define DEPLOYMENT_PLAN_HPP

#include <string>
#include <vector>
#include <cstdint>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

/** @brief A deployment target machine: a name and network address. */
struct HostInfo {
    std::string name;
    std::string address;
};

// 'core' sentinel: no core declared in the plan, to be filled by the allocator.
// The allocator also uses -1 to mark "could not place", so any core still < 0
// after allocation is an error either way.
/**
 * @brief Sentinel core value meaning "no core declared in the plan".
 *
 * Filled in later by the allocator. The allocator also uses -1 to mark
 * "could not place", so any core still negative after allocation is an
 * error either way.
 */
constexpr int CORE_UNASSIGNED = -1;

/**
 * @brief Sentinel meaning "no task-level priority declared in the plan".
 *
 * TaskInfo::priority (pi(tau_i) in specs/tdta-allocator/) is
 * allocation-time-only metadata consumed by the "tdta" strategy to order
 * DAG tasks; it is unrelated to SubtaskInfo::priority, which TeamManager
 * uses at runtime for Dispatcher (core, priority) grouping.
 */
constexpr int TASK_PRIORITY_UNSET = -1;


/**
 * @brief One subtask (pipeline node) as declared in a deployment plan.
 *
 * @var SubtaskInfo::task_id Id of the parent task; set by the parser, 0 for
 *      manually-built entries.
 * @var SubtaskInfo::id Globally unique subtask id across all tasks.
 * @var SubtaskInfo::component_type Registry key naming this subtask's
 *      concrete component (see ComponentRegistry).
 * @var SubtaskInfo::host Name of the HostInfo this subtask runs on.
 * @var SubtaskInfo::core Assigned CPU core, or CORE_UNASSIGNED if not yet
 *      allocated.
 * @var SubtaskInfo::priority Scheduling priority of this subtask.
 * @var SubtaskInfo::period_us Period of this subtask, in microseconds.
 * @var SubtaskInfo::deadline_us Relative deadline of this subtask, in
 *      microseconds.
 * @var SubtaskInfo::wcet_us Worst-case execution time, in microseconds; 0
 *      means not set.
 * @var SubtaskInfo::benchmark Name of the wcet_bench entry point to run as
 *      the subtask body; empty means demo semantics (see
 *      bench_registry.hpp).
 * @var SubtaskInfo::config Component-specific configuration, parsed by
 *      each component's own from_json().
 * @var SubtaskInfo::cpp_class Codegen-only: concrete C++ class name, e.g.
 *      "SourceDemo". Never read by the scheduling path.
 * @var SubtaskInfo::header Codegen-only: header declaring cpp_class, e.g.
 *      "demo_components.hpp".
 * @var SubtaskInfo::input_type Codegen-only: C++ type name for input_;
 *      empty if the component has none (source).
 * @var SubtaskInfo::output_type Codegen-only: C++ type name for output_;
 *      empty if the component has none (sink).
 */
struct SubtaskInfo {
    int         task_id = 0;     // parent task; set by parser, 0 for manually-built entries
    int         id      = 0;    // subtask (globally unique across all tasks)
    std::string component_type;
    std::string host;
    int         core = CORE_UNASSIGNED;
    int         priority;
    uint64_t    period_us;
    uint64_t    deadline_us;
    uint64_t    wcet_us = 0;    // worst-case execution time; 0 = not set
    std::string benchmark;      // wcet_bench entry point to run as the subtask
                                // body; empty = demo semantics (see bench_registry.hpp)
    json        config;

    // Codegen-only metadata (tools/codegen/codegen_main.cpp). Never read by
    // the scheduling path (Dispatcher/TeamManager) — component_type above is
    // still what ComponentRegistry uses at runtime. These are plain strings,
    // not derived from any C++-side registry, so codegen never needs to
    // #include a component header itself; only the code it generates does.
    std::string cpp_class;   // concrete C++ class name, e.g. "SourceDemo"
    std::string header;      // header declaring cpp_class, e.g. "demo_components.hpp"
    std::string input_type;  // C++ type name for input_; empty if the component has none (source)
    std::string output_type; // C++ type name for output_; empty if the component has none (sink)
};

/**
 * @brief A directed edge from one subtask's output to another's input.
 *
 * @var ConnectionInfo::upstream Id of the producing subtask.
 * @var ConnectionInfo::downstream Id of the consuming subtask.
 * @var ConnectionInfo::adapter Codegen-only (MCFlow Section IV-B): free
 *      function name that converts the upstream's output into the
 *      downstream's input; empty means identity (types must match
 *      exactly). Must be set together with adapter_header, and only when
 *      the two types actually differ.
 * @var ConnectionInfo::adapter_header Codegen-only: header declaring
 *      `adapter`.
 */
struct ConnectionInfo {
    int upstream;
    int downstream;

    // Codegen-only (MCFlow Section IV-B: "an adapter is a C or C++ function
    // that takes the output of an upstream component and converts it into
    // the input of a downstream component"). Empty = identity: the
    // upstream's output_type and this edge's downstream input_type must
    // match exactly. Both must be set together, and only when the two
    // types actually differ — codegen_main.cpp enforces this.
    std::string adapter;        // free function name, e.g. "int_to_double"
    std::string adapter_header; // header declaring `adapter`
};

/**
 * @brief A task grouping one or more subtasks under a shared task id.
 * @var TaskInfo::id Task identifier.
 * @var TaskInfo::priority pi(tau_i); TASK_PRIORITY_UNSET if not declared.
 *      Smaller value = higher priority. Allocation-time-only (the "tdta"
 *      strategy's task-ordering rule) — not read by TeamManager/Dispatcher.
 * @var TaskInfo::subtasks Subtasks belonging to this task.
 */
struct TaskInfo {
    int id;
    int priority = TASK_PRIORITY_UNSET;
    std::vector<SubtaskInfo> subtasks;
};

// Optional "allocation" block of the plan. Drives the automatic core
// assignment applied to every subtask that omits "core".
/**
 * @brief Optional "allocation" block; auto-assigns cores to subtasks
 * that omit "core" via allocator::apply_auto_allocation, which dispatches
 * on strategy: "worst_fit" is Worst-Fit with Decreasing Remaining
 * Utilisation (Verucchi et al. 2023; src/dru.hpp); "eru" is Equilibrium
 * Remaining Utilization (Wu et al. 2023, Algorithm 2; src/eru.hpp); "tdta"
 * is the full Topology-based DAG Task Allocation strategy (Wu et al.
 * 2023, Algorithm 3; src/tdta.hpp), which requires TaskInfo::priority set
 * whenever the plan has more than one task. Neither "eru" nor "tdta"
 * consult sort_by/weight at all — those are worst_fit-specific.
 *
 * Only strategy=worst_fit's own sort_by/weight defaults below are
 * implemented for that strategy; other inline-commented sort_by/weight
 * values are the WF+DRU paper's own Sect. 7 baselines and make
 * allocator::apply_auto_allocation throw when strategy=worst_fit.
 *
 * @var AllocationConfig::strategy Packing strategy (values inline).
 * @var AllocationConfig::sort_by Ordering before packing; worst_fit only (values inline).
 * @var AllocationConfig::weight Subtask sizing metric; worst_fit only (values inline).
 * @var AllocationConfig::num_cores 0 = all online CPUs.
 * @var AllocationConfig::capacity 0 = derive from weight mode.
 * @var AllocationConfig::validate Field itself unimplemented, not
 *      consulted — but the analysis it would invoke,
 *      rta_fonseca2016::compute_wcrt/check_schedulability (Fonseca et
 *      al. 2016), exists and is directly callable; specs/rta-fonseca2016/
 *      scoped wiring this field to it as a separate, future concern.
 * @var AllocationConfig::guided Unimplemented, not consulted (same
 *      caveat as @ref AllocationConfig::validate).
 */
struct AllocationConfig {
    std::string strategy  = "worst_fit";                     // first_fit | best_fit | worst_fit | eru | tdta
    std::string sort_by   = "remaining_utilization_desc";    // priority_desc | priority_asc |
                                             // period_asc | period_desc | utilization_asc |
                                             // utilization_desc | remaining_utilization_desc |
                                             // none
    std::string weight    = "utilization";   // count | utilization
    int         num_cores = 0;               // 0 = all online CPUs
    double      capacity  = 0.0;             // 0 = derive from weight mode
    std::string validate  = "none";          // none | rta | rta_v — extra acceptance test run
                                             // after the utilization-sum packing succeeds (see
                                             // src/rta_fonseca2016.hpp)
    std::string guided    = "none";          // none | rta | rta_v — when set, replaces the
                                             // utilization-sum packing itself with one that
                                             // tests each candidate core against this RTA
                                             // technique as it goes (see
                                             // allocator::detail::pack_rta_guided)
};

/**
 * @brief Full parsed deployment plan: hosts, tasks, connections, allocation.
 * @var DeploymentPlan::hosts Available deployment target machines.
 * @var DeploymentPlan::tasks Tasks and their subtasks.
 * @var DeploymentPlan::connections Directed edges between subtasks.
 * @var DeploymentPlan::allocation Automatic core-allocation settings.
 */
struct DeploymentPlan {
    std::vector<HostInfo>       hosts;
    std::vector<TaskInfo>       tasks;
    std::vector<ConnectionInfo> connections;
    AllocationConfig            allocation;
};

#endif // DEPLOYMENT_PLAN_HPP
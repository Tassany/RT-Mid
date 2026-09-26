#pragma once
#ifndef DEPLOYMENT_PLAN_HPP
#define DEPLOYMENT_PLAN_HPP

#include <string>
#include <vector>
#include <cstdint>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

struct HostInfo {
    std::string name;
    std::string address;
};

// 'core' sentinel: no core declared in the plan, to be filled by the allocator.
// The allocator also uses -1 to mark "could not place", so any core still < 0
// after allocation is an error either way.
constexpr int CORE_UNASSIGNED = -1;


struct SubtaskInfo {
    int         task_id = 0;     // parent task; set by parser, 0 for manually-built entries
    int         id      = 0;    // subtask (globally unique across all tasks)
    std::string component_type;
    std::string host;
    int         core = CORE_UNASSIGNED;
    int         priority;
    uint64_t    period_ns;
    uint64_t    deadline_ns;
    uint64_t    wcet_ns = 0;    // worst-case execution time; 0 = not set
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

struct TaskInfo {
    int id;
    std::vector<SubtaskInfo> subtasks;
};

// Optional "allocation" block of the plan. Drives the automatic core
// assignment applied to every subtask that omits "core".
struct AllocationConfig {
    std::string strategy  = "worst_fit";     // first_fit | best_fit | worst_fit
    std::string sort_by   = "priority_desc"; // priority_desc | priority_asc | period_asc |
                                             // period_desc | utilization_asc |
                                             // utilization_desc | remaining_utilization_desc |
                                             // none
    std::string weight    = "count";         // count | utilization
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

struct DeploymentPlan {
    std::vector<HostInfo>       hosts;
    std::vector<TaskInfo>       tasks;
    std::vector<ConnectionInfo> connections;
    AllocationConfig            allocation;
};

#endif // DEPLOYMENT_PLAN_HPP
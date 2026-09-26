#pragma once
#ifndef JSON_PARSER_HPP
#define JSON_PARSER_HPP

#include "deployment_plan.hpp"
#include <vector>
#include <nlohmann/json.hpp>
#include <fstream>
#include <string>
#include <sstream>
#include <stdexcept>


class JsonParser {
public:
    // Full validation: structure (below) plus the strong component_type-vs-
    // topology check, which constructs a real component per subtask via
    // ComponentRegistry — the caller's binary must have those component
    // types actually registered (i.e. #include their headers), or this
    // throws "unknown component_type". This is what every normal caller
    // (tests, and eventually the real application) should use.
    DeploymentPlan parse(const std::string& filename);

    // Structure-only validation (cycles) plus auto-allocation, deliberately
    // skipping the ComponentRegistry-based check above. For tools/codegen
    // only: codegen_main.cpp is a pure text generator that never #includes
    // a component header (by design — see its own file comment), so it has
    // no real components to construct and cannot run the strong check.
    // Component_type/topology agreement for a plan processed this way is
    // only verified later, whenever something that *does* have the real
    // components registered calls parse() (or validate_components()) on it.
    DeploymentPlan parse_for_codegen(const std::string& filename);

private:
    std::vector<HostInfo>       parse_hosts(const nlohmann::json& j);
    std::vector<TaskInfo>       parse_tasks(const nlohmann::json& j);
    std::vector<SubtaskInfo>    parse_subtasks(const nlohmann::json& j);
    std::vector<ConnectionInfo> parse_connections(const nlohmann::json& j);
    AllocationConfig            parse_allocation(const nlohmann::json& j);

    DeploymentPlan parse_raw(const std::string& filename);
    void           apply_allocation_if_needed(DeploymentPlan& plan);

    // Throws std::runtime_error if plan.connections form a cycle.
    void validate_structure(const DeploymentPlan& plan) const;

    // Throws std::runtime_error if any subtask's declared component_type
    // disagrees with its actual fan-in/fan-out (e.g. a node with 2
    // predecessors declared "source"), checked against the *real* built
    // component's kind() via ComponentRegistry — requires the caller's
    // binary to have that component_type actually registered.
    void validate_components(const DeploymentPlan& plan) const;
};

#endif // JSON_PARSER_HPP
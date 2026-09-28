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


/**
 * @brief Parses and validates a JSON deployment plan into a DeploymentPlan.
 *
 * Reads hosts, tasks/subtasks, connections and allocation settings from a
 * plan file, checks the resulting dependency graph, and (depending on the
 * entry point used) cross-checks each subtask's declared component_type
 * against the real component's role.
 */
class JsonParser {
public:
    /**
     * @brief Fully parses and validates a deployment plan file.
     *
     * Performs structure validation (below) plus the strong
     * component_type-vs-topology check, which constructs a real component
     * per subtask via ComponentRegistry. This is what every normal caller
     * (tests, and eventually the real application) should use.
     *
     * @param filename Path to the JSON deployment plan file.
     * @return Parsed, validated, and auto-allocated DeploymentPlan.
     * @throws std::runtime_error if the file cannot be opened, the
     *         connections form a cycle, a subtask names an unregistered
     *         component_type, or the caller's binary does not have that
     *         component type registered (i.e. its header was not included).
     */
    DeploymentPlan parse(const std::string& filename);

    /**
     * @brief Parses a deployment plan file for the codegen tool only.
     *
     * Performs structure-only validation (cycles) plus auto-allocation,
     * deliberately skipping the ComponentRegistry-based check in parse().
     * codegen_main.cpp is a pure text generator that never includes a
     * component header, so it has no real components to construct and
     * cannot run the strong check; component_type/topology agreement for a
     * plan processed this way is verified later, whenever something with
     * the real components registered calls parse() on it.
     *
     * @param filename Path to the JSON deployment plan file.
     * @return Parsed, structurally-validated, and auto-allocated
     *         DeploymentPlan.
     * @throws std::runtime_error if the file cannot be opened or the
     *         connections form a cycle.
     */
    DeploymentPlan parse_for_codegen(const std::string& filename);

private:
    /**
     * @brief Parses the "hosts" array of a plan.
     * @param j Root JSON object of the deployment plan.
     * @return Parsed list of HostInfo entries.
     */
    std::vector<HostInfo>       parse_hosts(const nlohmann::json& j);
    /**
     * @brief Parses the "tasks" array of a plan, including nested subtasks.
     * @param j Root JSON object of the deployment plan.
     * @return Parsed list of TaskInfo entries with task_id stamped on each
     *         subtask.
     */
    std::vector<TaskInfo>       parse_tasks(const nlohmann::json& j);
    /**
     * @brief Parses the "subtasks" array within a single task object.
     * @param j JSON object for one task, containing a "subtasks" array.
     * @return Parsed list of SubtaskInfo entries with defaults applied.
     */
    std::vector<SubtaskInfo>    parse_subtasks(const nlohmann::json& j);
    /**
     * @brief Parses the "connections" array of a plan.
     * @param j Root JSON object of the deployment plan.
     * @return Parsed list of ConnectionInfo entries.
     */
    std::vector<ConnectionInfo> parse_connections(const nlohmann::json& j);
    /**
     * @brief Parses the optional "allocation" object of a plan.
     * @param j Root JSON object of the deployment plan.
     * @return AllocationConfig with fields overridden from JSON, or the
     *         default AllocationConfig if the plan has no "allocation" key.
     */
    AllocationConfig            parse_allocation(const nlohmann::json& j);

    /**
     * @brief Reads and parses a plan file with no validation applied.
     * @param filename Path to the JSON deployment plan file.
     * @return DeploymentPlan populated from the file's raw contents.
     * @throws std::runtime_error if the file cannot be opened.
     */
    DeploymentPlan parse_raw(const std::string& filename);
    /**
     * @brief Runs auto-allocation if any subtask has no assigned core.
     * @param plan Plan to check and, if needed, mutate in place via
     *        allocator::apply_auto_allocation.
     * @return void
     */
    void           apply_allocation_if_needed(DeploymentPlan& plan);

    /**
     * @brief Validates that the plan's connections form an acyclic graph.
     * @param plan Plan whose connections are checked.
     * @return void
     * @throws std::runtime_error if plan.connections form a cycle.
     */
    void validate_structure(const DeploymentPlan& plan) const;

    /**
     * @brief Cross-checks each subtask's declared component_type.
     *
     * Builds the real component named by each subtask's component_type via
     * ComponentRegistry and compares its actual kind() against the role
     * implied by the subtask's fan-in/fan-out in the connection graph.
     *
     * @param plan Plan whose subtasks and connections are checked.
     * @return void
     * @throws std::runtime_error if any subtask's declared component_type
     *         disagrees with its actual fan-in/fan-out (e.g. a node with 2
     *         predecessors declared "source"), or names an unregistered
     *         component_type.
     */
    void validate_components(const DeploymentPlan& plan) const;
};

#endif // JSON_PARSER_HPP
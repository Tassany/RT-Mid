#include "parser_json.hpp"
#include "allocator.hpp"
#include "dag.hpp"
#include "component_registry.hpp"


using json = nlohmann::json;

namespace {

// MCFlow's classification (paper System Model, pg. 2): "an initial subtask
// does not depend on any other subtask" / "an intermediate subtask has at
// least one other subtask on which it depends and at least one other
// subtask that depends on it" / "a terminal subtask has no other subtasks
// that depend on it". A subtask with fan_in==0 AND fan_out==0 (isolated,
// no connections either way) satisfies both the initial and the terminal
// wording at once — the paper doesn't resolve that degenerate case, so this
// is a deliberate tie-break, not an oversight: it resolves to "source".
/**
 * @brief Derives the expected component role from graph connectivity.
 *
 * Ties are broken deliberately: a subtask with fan_in==0 AND fan_out==0
 * (isolated) satisfies both the "initial" and "terminal" wording of the
 * paper's classification at once, and resolves to "source".
 *
 * @param fan_in Number of direct predecessors of the subtask.
 * @param fan_out Number of direct successors of the subtask.
 * @return "source" if fan_in==0, "sink" if fan_out==0 (and fan_in!=0),
 *         otherwise "intermediate".
 */
std::string expected_component_type(int fan_in, int fan_out) {
    if (fan_in == 0)  return "source";
    if (fan_out == 0) return "sink";
    return "intermediate";
}

/**
 * @brief Converts a ComponentKind enum value to its plan-file string name.
 * @param kind Component role reported by a real component's kind().
 * @return "source", "intermediate", or "sink"; "unknown" is unreachable
 *         and exists only to silence -Wreturn-type on some compilers.
 */
std::string role_string(ComponentKind kind) {
    switch (kind) {
        case ComponentKind::SOURCE:       return "source";
        case ComponentKind::INTERMEDIATE: return "intermediate";
        case ComponentKind::SINK:         return "sink";
    }
    return "unknown"; // unreachable; silences -Wreturn-type on some compilers
}

}

/** @copydoc JsonParser::parse_raw */
DeploymentPlan JsonParser::parse_raw(const std::string& filename) {
    std::ifstream file(filename.c_str());
    if (!file.is_open()) {
        throw std::runtime_error("Not possible to open: " + filename);
    }

    json j = json::parse(file);

    DeploymentPlan plan;
    plan.hosts       = parse_hosts(j);
    plan.tasks       = parse_tasks(j);
    plan.connections = parse_connections(j);
    plan.allocation  = parse_allocation(j);
    return plan;
}

/** @copydoc JsonParser::apply_allocation_if_needed */
void JsonParser::apply_allocation_if_needed(DeploymentPlan& plan) {
    bool needs_allocation = false;
    for (const auto& task : plan.tasks)
        for (const auto& st : task.subtasks)
            if (st.core == CORE_UNASSIGNED) needs_allocation = true;

    if (needs_allocation)
        allocator::apply_auto_allocation(plan);
}

/** @copydoc JsonParser::parse */
DeploymentPlan JsonParser::parse(const std::string& filename) {
    DeploymentPlan plan = parse_raw(filename);
    validate_structure(plan);
    validate_components(plan);
    apply_allocation_if_needed(plan);
    return plan;
}

/** @copydoc JsonParser::parse_for_codegen */
DeploymentPlan JsonParser::parse_for_codegen(const std::string& filename) {
    DeploymentPlan plan = parse_raw(filename);
    validate_structure(plan);
    apply_allocation_if_needed(plan);
    return plan;
}

/** @copydoc JsonParser::parse_hosts */
std::vector<HostInfo> JsonParser::parse_hosts(const json& j) {
    std::vector<HostInfo> hosts;
    for (const auto& h : j["hosts"]) {
        HostInfo host;
        host.name    = h["name"];
        host.address = h["address"];
        hosts.push_back(host);
    }
    return hosts;
}

/** @copydoc JsonParser::parse_tasks */
std::vector<TaskInfo> JsonParser::parse_tasks(const json& j) {
    std::vector<TaskInfo> tasks;
    for (const auto& t : j["tasks"]) {
        TaskInfo task;
        task.id       = t["id"];
        task.subtasks = parse_subtasks(t);
        for (auto& s : task.subtasks)
            s.task_id = task.id;
        tasks.push_back(task);
    }
    return tasks;
}

/** @copydoc JsonParser::parse_subtasks */
std::vector<SubtaskInfo> JsonParser::parse_subtasks(const json& j) {
    std::vector<SubtaskInfo> subtasks;
    for (const auto& s : j["subtasks"]) {
        SubtaskInfo subtask;
        subtask.id      = s.value("id", 0);
        subtask.component_type = s["component_type"];
        subtask.host      = s.value("host", std::string{""});
        subtask.core      = s.value("core", CORE_UNASSIGNED);
        subtask.priority  = s["priority"];
        subtask.period_us   = s.value("period_us", uint64_t(0));
        subtask.deadline_us = s.value("deadline_us", uint64_t(0));
        subtask.wcet_us     = s.value("wcet_us", uint64_t(0));
        subtask.benchmark   = s.value("benchmark", std::string{""});
        subtask.config      = s.value("config", json::object());
        subtask.cpp_class   = s.value("cpp_class",   std::string{""});
        subtask.header      = s.value("header",      std::string{""});
        subtask.input_type  = s.value("input_type",  std::string{""});
        subtask.output_type = s.value("output_type", std::string{""});
        subtasks.push_back(subtask);
    }
    return subtasks;
}

/** @copydoc JsonParser::parse_allocation */
AllocationConfig JsonParser::parse_allocation(const json& j) {
    AllocationConfig cfg;  // defaults live in deployment_plan.hpp
    if (!j.contains("allocation")) return cfg;

    const auto& a = j["allocation"];
    cfg.strategy  = a.value("strategy",  cfg.strategy);
    cfg.sort_by   = a.value("sort_by",   cfg.sort_by);
    cfg.weight    = a.value("weight",    cfg.weight);
    cfg.num_cores = a.value("num_cores", cfg.num_cores);
    cfg.capacity  = a.value("capacity",  cfg.capacity);
    cfg.validate  = a.value("validate",  cfg.validate);
    cfg.guided    = a.value("guided",    cfg.guided);
    return cfg;
}

/** @copydoc JsonParser::parse_connections */
std::vector<ConnectionInfo> JsonParser::parse_connections(const json& j) {
    std::vector<ConnectionInfo> connections;
    for(const auto& c : j["connections"]){
        ConnectionInfo connection;
        connection.upstream      = c["upstream"];
        connection.downstream    = c["downstream"];
        connection.adapter       = c.value("adapter",        std::string{""});
        connection.adapter_header = c.value("adapter_header", std::string{""});
        connections.push_back(connection);
    }
    return connections;
}

/** @copydoc JsonParser::validate_structure */
void JsonParser::validate_structure(const DeploymentPlan& plan) const {
    DAG dag;
    for (const auto& task : plan.tasks)
        for (const auto& st : task.subtasks)
            dag.add_node(st.id);
    for (const auto& c : plan.connections)
        dag.add_edge(c.upstream, c.downstream);

    try {
        dag.topological_sort();
    } catch (const std::runtime_error&) {
        throw std::runtime_error(
            "JsonParser::parse: plan connections form a cycle");
    }
}

/** @copydoc JsonParser::validate_components */
void JsonParser::validate_components(const DeploymentPlan& plan) const {
    DAG dag;
    for (const auto& task : plan.tasks)
        for (const auto& st : task.subtasks)
            dag.add_node(st.id);
    for (const auto& c : plan.connections)
        dag.add_edge(c.upstream, c.downstream);

    // Strong guarantee: for every subtask, build the real component its
    // plan entry names (component_type + config) and check its *actual*
    // kind() — from the C++ class the developer wrote, via component.hpp —
    // against what the graph topology requires. Two independent sources of
    // truth checked against each other, not a hand-typed label compared to
    // itself. Each instance is discarded right after the check: this pass
    // only validates the plan, it doesn't wire the real execution DAG.
    for (const auto& task : plan.tasks) {
        for (const auto& st : task.subtasks) {
            const std::string expected_role = expected_component_type(
                dag.fan_in_count(st.id), dag.fan_out_count(st.id));

            if (!ComponentRegistry::instance().has(st.component_type))
                throw std::runtime_error(
                    "JsonParser::parse: subtask " + std::to_string(st.id) +
                    " declares unknown component_type \"" +
                    st.component_type + "\"");

            ComponentInstance inst =
                ComponentRegistry::instance().create(st.component_type, st.config);
            const std::string actual_role = role_string(inst.component->kind());

            if (actual_role != expected_role)
                throw std::runtime_error(
                    "JsonParser::parse: subtask " + std::to_string(st.id) +
                    " is built as \"" + actual_role + "\" (component_type \"" +
                    st.component_type + "\") but its connections make it \"" +
                    expected_role + "\"");
        }
    }
}
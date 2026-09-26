// Unit test for Step 1 of the codegen plan: the new codegen-only metadata
// fields on SubtaskInfo (cpp_class, header, input_type, output_type) and
// ConnectionInfo (adapter, adapter_header) round-trip correctly from JSON.
// Registers two tiny throwaway components locally (not src/demo_components.hpp
// — that extraction is Step 2) purely so validate_dag()'s existing strong
// check (component_type -> real kind() vs. topology) has something to build.
//
// Must be run from the repo root (fixture path below is relative).
// Build: g++ -std=c++17 -Isrc -Iinclude tests/deployment_plan_fields_test.cpp src/dag.cpp src/parser_json.cpp -o /tmp/deployment_plan_fields_test

#include <cstdlib>
#include <iostream>
#include "parser_json.hpp"
#include "component_registry.hpp"

struct EmptyConfig {};
inline void from_json(const nlohmann::json&, EmptyConfig&) {}

class FieldsSource : public SourceComponent<int, EmptyConfig> {
public:
    using SourceComponent::SourceComponent;
    void execute() override { output_ = 1; }
};
class FieldsSink : public SinkComponent<double, EmptyConfig> {
public:
    using SinkComponent::SinkComponent;
    void execute() override {}
};

RT_MID_REGISTER_COMPONENT("fields_source", FieldsSource, EmptyConfig)
RT_MID_REGISTER_COMPONENT("fields_sink",   FieldsSink,   EmptyConfig)

static void expect(bool cond, const char* what) {
    if (!cond) { std::cerr << "FAIL: " << what << "\n"; std::exit(1); }
    std::cerr << "ok:   " << what << "\n";
}

int main() {
    JsonParser parser;
    DeploymentPlan plan = parser.parse("tests/fixtures/fields_roundtrip_plan.json");

    expect(plan.tasks.size() == 1 && plan.tasks[0].subtasks.size() == 2,
           "fixture plan parsed with two subtasks");

    const SubtaskInfo& src = plan.tasks[0].subtasks[0];
    const SubtaskInfo& snk = plan.tasks[0].subtasks[1];

    expect(src.cpp_class == "FieldsSource", "source subtask cpp_class round-tripped");
    expect(src.header == "fields_demo.hpp", "source subtask header round-tripped");
    expect(src.output_type == "int", "source subtask output_type round-tripped");
    expect(src.input_type.empty(), "source subtask input_type defaults empty (not set in the plan)");

    expect(snk.cpp_class == "FieldsSink", "sink subtask cpp_class round-tripped");
    expect(snk.input_type == "double", "sink subtask input_type round-tripped");
    expect(snk.output_type.empty(), "sink subtask output_type defaults empty (not set in the plan)");

    expect(plan.connections.size() == 1, "one connection parsed");
    const ConnectionInfo& conn = plan.connections[0];
    expect(conn.upstream == 1 && conn.downstream == 2, "connection endpoints correct");
    expect(conn.adapter == "int_to_double", "connection adapter name round-tripped");
    expect(conn.adapter_header == "fields_adapters.hpp", "connection adapter_header round-tripped");

    std::cerr << "\nAll checks passed.\n";
    return 0;
}

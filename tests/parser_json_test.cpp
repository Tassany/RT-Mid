// Integration test for JsonParser: parses the real plans/deployment_plan.json
// (not a synthetic in-memory plan) end to end — parsing, validate_dag()'s
// strong component_type-vs-topology check (via ComponentRegistry, see
// parser_json.cpp), and the auto-allocation path (every subtask in the plan
// omits "core", so this only passes if allocator.hpp's stub actually ran).
// Components (source_demo/intermediate_demo/sink_demo) live in
// src/demo_components.hpp, shared with other tests and named directly in
// the plan's cpp_class/header fields for the codegen tool.
//
// Must be run from the repo root (the path below is relative), same as
// `make test` already does.
//
// Build: g++ -std=c++17 -Isrc -Iinclude tests/parser_json_test.cpp src/dag.cpp src/parser_json.cpp -o /tmp/parser_json_test

#include <cstdlib>
#include <iostream>
#include "parser_json.hpp"
#include "demo_components.hpp"

static void expect(bool cond, const char* what) {
    if (!cond) { std::cerr << "FAIL: " << what << "\n"; std::exit(1); }
    std::cerr << "ok:   " << what << "\n";
}

int main() {
    JsonParser parser;
    DeploymentPlan plan;
    try {
        plan = parser.parse("plans/deployment_plan.json");
    } catch (const std::exception& e) {
        std::cerr << "FAIL: parse() threw: " << e.what() << "\n";
        return 1;
    }
    std::cerr << "ok:   parse() succeeded without throwing\n";

    expect(plan.hosts.size() == 1, "one host parsed");
    expect(plan.tasks.size() == 1, "one task parsed");
    expect(plan.tasks[0].subtasks.size() == 6, "six subtasks parsed (Ts, T0-T3, Tm)");
    expect(plan.connections.size() == 8, "eight connections parsed (4 fan-out + 4 fan-in)");

    // The plan declares no "core" for any subtask — reaching a real value
    // here only happens if allocator::apply_auto_allocation() (the stub)
    // actually ran via JsonParser::parse()'s needs_allocation check.
    for (const auto& st : plan.tasks[0].subtasks) {
        expect(st.core != CORE_UNASSIGNED,
               "subtask core was auto-allocated (plan declared none)");
        expect(st.core == 0, "stub allocator's known placeholder behavior: everything on core 0");
    }

    // component_type-vs-topology agreement (Ts=source, T0-T3=intermediate,
    // Tm=sink) was already enforced inside parse() (validate_dag(), building
    // a real component per subtask via ComponentRegistry and checking its
    // kind()) — reaching this line without an exception already proves it
    // held for all 6 subtasks; nothing further to assert here.

    std::cerr << "\nAll checks passed.\n";
    return 0;
}

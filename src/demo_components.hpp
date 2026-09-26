#pragma once

/**
 * @file demo_components.hpp
 *
 * Example components registered under "source_demo"/"intermediate_demo"/
 * "sink_demo", matching MCFlow's own three-way classification (source,
 * intermediate, sink — not "worker", which was this file's placeholder name
 * before it was corrected). Used by tests/parser_json_test.cpp,
 * tests/component_registry_test.cpp-style tests, and named directly in
 * plans/deployment_plan.json's cpp_class/header fields for the codegen tool.
 */

#include <array>
#include "component_registry.hpp"

struct EmptyConfig {};
inline void from_json(const nlohmann::json&, EmptyConfig&) {}

struct IntermediateConfig { double gain = 1.0; };
inline void from_json(const nlohmann::json& j, IntermediateConfig& c) {
    c.gain = j.value("gain", 1.0);
}

// Names and roles match plans/deployment_plan.json's component_type fields,
// which mirror the paper's own Fig. 1 / Table 1 "High" task:
// source_demo = Ts, intermediate_demo x4 = T0..T3, sink_demo = Tm.
class SourceDemo : public SourceComponent<double, EmptyConfig> {
public:
    using SourceComponent::SourceComponent;
    void execute() override { output_ = 1.0; }
};
class IntermediateDemo : public Component<double, double, IntermediateConfig> {
public:
    using Component::Component;
    void execute() override { output_ = input_ * config_->gain; }
};
// Tm has fan-in 4 (T0..T3). A multi-supplier consumer's input_type must be
// std::array<T,K> — there is no generic way to merge K suppliers into one
// scalar value (see src/adapter.hpp's header comment / MCFlow Fig. 7 "Data
// Merge"); the consumer's own execute() decides how to combine them. Fixed
// at K=4 here since that's this demo topology's actual fan-in.
class SinkDemo : public SinkComponent<std::array<double, 4>, EmptyConfig> {
public:
    using SinkComponent::SinkComponent;
    void execute() override {}
};

RT_MID_REGISTER_COMPONENT("source_demo",       SourceDemo,       EmptyConfig)
RT_MID_REGISTER_COMPONENT("intermediate_demo", IntermediateDemo, IntermediateConfig)
RT_MID_REGISTER_COMPONENT("sink_demo",         SinkDemo,         EmptyConfig)

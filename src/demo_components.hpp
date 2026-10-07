#pragma once

/**
 * @file demo_components.hpp
 *
 * Example components registered under "source"/"intermediate"/"sink",
 * matching MCFlow's own three-way classification (source, intermediate,
 * sink — not "worker", which was this file's placeholder name before it
 * was corrected). Used by tests/parser_json_test.cpp,
 * tests/component_registry_test.cpp-style tests, and named directly in
 * plans/deployment_plan.json's cpp_class/header fields for the codegen tool.
 */

#include <array>
#include "component_registry.hpp"

/** @brief Marker config for demo components that take no parameters. */
struct EmptyConfig {};
/**
 * @brief Parses an EmptyConfig from JSON (no-op: there are no fields).
 * @param Unnamed JSON value; ignored.
 * @param Unnamed output EmptyConfig; left unmodified.
 * @return void
 */
inline void from_json(const nlohmann::json&, EmptyConfig&) {}

/** @brief Config for IntermediateDemo: a scalar multiplicative gain. */
struct IntermediateConfig { double gain = 1.0; };
/**
 * @brief Parses an IntermediateConfig from JSON.
 * @param j JSON object optionally containing a "gain" field.
 * @param c Output config; c.gain is set to j["gain"], defaulting to 1.0.
 * @return void
 */
inline void from_json(const nlohmann::json& j, IntermediateConfig& c) {
    c.gain = j.value("gain", 1.0);
}

// Names and roles match plans/deployment_plan.json's component_type fields,
// which mirror the paper's own Fig. 1 / Table 1 "High" task:
// source = Ts, intermediate x4 = T0..T3, sink = Tm.
/**
 * @brief Demo source component (Ts) producing a constant scalar output.
 */
class SourceDemo : public SourceComponent<double, EmptyConfig> {
public:
    using SourceComponent::SourceComponent;
    /**
     * @brief Sets output_ to the constant 1.0.
     * @return void
     */
    void execute() override { output_ = 1.0; }
};
/**
 * @brief Demo intermediate component (T0..T3) scaling its input by a gain.
 */
class IntermediateDemo : public Component<double, double, IntermediateConfig> {
public:
    using Component::Component;
    /**
     * @brief Sets output_ to input_ multiplied by config_->gain.
     * @return void
     */
    void execute() override { output_ = input_ * config_->gain; }
};
// Tm has fan-in 4 (T0..T3). A multi-supplier consumer's input_type must be
// std::array<T,K> — there is no generic way to merge K suppliers into one
// scalar value (see src/adapter.hpp's header comment / MCFlow Fig. 7 "Data
// Merge"); the consumer's own execute() decides how to combine them. Fixed
// at K=4 here since that's this demo topology's actual fan-in.
/**
 * @brief Demo sink component (Tm) that merges 4 upstream scalar inputs.
 *
 * Fan-in is fixed at K=4 to match this demo topology's actual fan-in
 * (T0..T3 feeding Tm); execute() is currently a no-op placeholder.
 */
class SinkDemo : public SinkComponent<std::array<double, 4>, EmptyConfig> {
public:
    using SinkComponent::SinkComponent;
    /**
     * @brief Placeholder: does nothing with the merged inputs.
     * @return void
     */
    void execute() override {}
};

RT_MID_REGISTER_COMPONENT("source",       SourceDemo,       EmptyConfig)
RT_MID_REGISTER_COMPONENT("intermediate", IntermediateDemo, IntermediateConfig)
RT_MID_REGISTER_COMPONENT("sink",         SinkDemo,         EmptyConfig)

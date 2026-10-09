#pragma once

// Demo components for tests/test_flux.cpp's topology only — NOT reused from
// src/demo_components.hpp. Reason: demo_components.hpp's SinkDemo is
// SinkComponent<std::array<double,4>, EmptyConfig>, hardcoded to the
// original plan's fan-in of 4 (T0..T3 -> Tm). This test's N4 has fan-in 2
// (N2, N3), so codegen derives multi_reader<double, N, 2> for it, and
// wire_component<SinkDemo>() would try to pass a std::array<double,2> into
// SinkDemo::init_input(const std::array<double,4>&) — a compile error.
// FluxSource/FluxIntermediate don't have a fan-in/fan-out baked into their
// type, so they could have reused SourceDemo/IntermediateDemo; defined here
// too instead, to keep this test fully independent of demo_components.hpp.
//
// Lives in its own header (not inline in test_flux.cpp) because
// tools/codegen/codegen_main.cpp emits `#include "<header>"` per subtask
// into the generated pipeline .cpp — it cannot #include a .cpp that also
// defines main().

#include <array>
#include "component_registry.hpp"

struct FluxEmptyConfig {};
inline void from_json(const nlohmann::json&, FluxEmptyConfig&) {}

struct FluxIntermediateConfig { double gain = 1.0; };
inline void from_json(const nlohmann::json& j, FluxIntermediateConfig& c) {
    c.gain = j.value("gain", 1.0);
}

class FluxSource : public SourceComponent<double, FluxEmptyConfig> {
public:
    using SourceComponent::SourceComponent;
    void execute() override { output_ = 1.0; }
};

class FluxIntermediate : public Component<double, double, FluxIntermediateConfig> {
public:
    using Component::Component;
    void execute() override { output_ = input_ * config_->gain; }
};

// input_type is std::array<double,2>: this test's N4 has fan-in 2, not 4.
// SinkComponent has no output_ (component.hpp) — sinks are terminal, with
// nowhere to write an output to. MCFlow's own model (Fig. 7, "Data Merge")
// gives the sink all K fields and lets its own execute() decide how to
// combine them, so summing here (into a plain member of our own, result_)
// matches that model rather than working around it.
class FluxSink : public SinkComponent<std::array<double, 2>, FluxEmptyConfig> {
public:
    using SinkComponent::SinkComponent;
    double result_ = 0.0;
    void execute() override { result_ = input_[0] + input_[1]; }
};

RT_MID_REGISTER_COMPONENT("source",       FluxSource,       FluxEmptyConfig)
RT_MID_REGISTER_COMPONENT("intermediate", FluxIntermediate, FluxIntermediateConfig)
RT_MID_REGISTER_COMPONENT("sink",         FluxSink,         FluxEmptyConfig)

#pragma once

// Components for tests/performance_test.cpp: MCFlow paper (Huang et al.
// 2012), Section VI-C "Real-time Performance". Unlike flux_components.hpp
// (trivial multiply/no-op, used to check WIRING correctness), these
// actually BURN the workload_us given in the plan's config, via a busy-spin
// on Dispatcher::monotonic_ns() — Table I's numbers are "computation time",
// and a sleep()-based stand-in would yield the CPU instead of occupying it,
// which is not what the table is measuring.

#include <array>
#include <cstdint>
#include "component_registry.hpp"
#include "dispatcher.hpp" // Dispatcher::monotonic_ns() — CLOCK_MONOTONIC, same clock as everywhere else in this project

struct WorkloadConfig { double workload_us = 0.0; };
inline void from_json(const nlohmann::json& j, WorkloadConfig& c) {
    c.workload_us = j.value("workload_us", 0.0);
}

// Busy-spins for approximately workload_us microseconds of real CPU time.
// A no-op (workload_us <= 0) for T0's Medium-priority cell in Table I,
// which is printed as (CPU 1, 0 us) — see the file comment in
// deployment_plan_perf.json for why that's kept as printed, not "fixed".
inline void busy_spin_us(double workload_us) {
    if (workload_us <= 0.0) return;
    const uint64_t start  = Dispatcher::monotonic_ns();
    const uint64_t target = start + static_cast<uint64_t>(workload_us * 1000.0);
    while (Dispatcher::monotonic_ns() < target) { /* spin: this IS the workload */ }
}

// Ts: initial subtask of every task in Figure 8's topology.
class WorkloadSource : public SourceComponent<double, WorkloadConfig> {
public:
    using SourceComponent::SourceComponent;
    void execute() override { busy_spin_us(config_->workload_us); output_ = 1.0; }
};

// T0..T3: the four parallel subtasks Ts fans out to and Tm fans in from.
// Pass-through — this experiment is about timing, not data transformation.
class WorkloadIntermediate : public Component<double, double, WorkloadConfig> {
public:
    using Component::Component;
    void execute() override { busy_spin_us(config_->workload_us); output_ = input_; }
};

// Tm: terminal subtask, fan-in 4 (T0..T3) — same std::array<double,4> shape
// as src/demo_components.hpp's SinkDemo, since it's the same topology.
class WorkloadSink : public SinkComponent<std::array<double, 4>, WorkloadConfig> {
public:
    using SinkComponent::SinkComponent;
    void execute() override { busy_spin_us(config_->workload_us); }
};

RT_MID_REGISTER_COMPONENT("workload_source",       WorkloadSource,       WorkloadConfig)
RT_MID_REGISTER_COMPONENT("workload_intermediate", WorkloadIntermediate, WorkloadConfig)
RT_MID_REGISTER_COMPONENT("workload_sink",         WorkloadSink,         WorkloadConfig)

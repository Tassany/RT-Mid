#pragma once

/**
 * @file mcflow_bench_components.hpp
 *
 * Components for replicating MCFlow's own Section VI-C "Real-time
 * Performance" experiment (Table I/II/III): each one busy-spins for a
 * configurable number of microseconds (config's "workload_us") before
 * passing data on — a real, CPU-bound workload, not the near-instant math
 * in demo_components.hpp. The spin uses CLOCK_MONOTONIC directly (same
 * convention as Dispatcher::monotonic_ns()), not sleep — sleeping would
 * yield the CPU instead of holding it, which is the whole point of
 * simulating a WCET-bearing subtask.
 */

#include <array>
#include <time.h>
#include "component_registry.hpp"

struct WorkloadConfig { uint64_t workload_us = 0; };
inline void from_json(const nlohmann::json& j, WorkloadConfig& c) {
    c.workload_us = j.value("workload_us", uint64_t(0));
}

inline void busy_spin_us(uint64_t us) {
    if (us == 0) return;
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    const uint64_t target_ns = us * 1000ULL;
    struct timespec now;
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        const uint64_t elapsed_ns =
            static_cast<uint64_t>(now.tv_sec - start.tv_sec) * 1'000'000'000ULL +
            static_cast<uint64_t>(now.tv_nsec - start.tv_nsec);
        if (elapsed_ns >= target_ns) return;
    }
}

class BenchSource : public SourceComponent<double, WorkloadConfig> {
public:
    using SourceComponent::SourceComponent;
    void execute() override { busy_spin_us(config_->workload_us); output_ = 1.0; }
};

class BenchIntermediate : public Component<double, double, WorkloadConfig> {
public:
    using Component::Component;
    void execute() override { busy_spin_us(config_->workload_us); output_ = input_; }
};

// Sink for a 4-fan-in task (this experiment's topology: Ts -> T0..T3 -> Tm).
class BenchSink4 : public SinkComponent<std::array<double, 4>, WorkloadConfig> {
public:
    using SinkComponent::SinkComponent;
    void execute() override { busy_spin_us(config_->workload_us); }
};

RT_MID_REGISTER_COMPONENT("bench_source",       BenchSource,       WorkloadConfig)
RT_MID_REGISTER_COMPONENT("bench_intermediate", BenchIntermediate, WorkloadConfig)
RT_MID_REGISTER_COMPONENT("bench_sink4",        BenchSink4,        WorkloadConfig)

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

/** @brief Config for bench components: busy-spin duration in microseconds. */
struct WorkloadConfig { uint64_t workload_us = 0; };
/**
 * @brief Parses a WorkloadConfig from JSON.
 * @param j JSON object optionally containing a "workload_us" field.
 * @param c Output config; c.workload_us is set from j, defaulting to 0.
 * @return void
 */
inline void from_json(const nlohmann::json& j, WorkloadConfig& c) {
    c.workload_us = j.value("workload_us", uint64_t(0));
}

/**
 * @brief Busy-spins on CLOCK_MONOTONIC for a fixed duration.
 *
 * Holds the CPU (rather than sleeping, which would yield it) so the
 * simulated workload actually occupies the core for the requested time,
 * matching a real WCET-bearing subtask.
 *
 * @param us Duration to spin for, in microseconds; 0 returns immediately.
 * @return void
 */
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

/** @brief Benchmark source component: spins, then outputs a constant. */
class BenchSource : public SourceComponent<double, WorkloadConfig> {
public:
    using SourceComponent::SourceComponent;
    /**
     * @brief Busy-spins for config_->workload_us, then sets output_ to 1.0.
     * @return void
     */
    void execute() override { busy_spin_us(config_->workload_us); output_ = 1.0; }
};

/** @brief Benchmark intermediate component: spins, then forwards its input. */
class BenchIntermediate : public Component<double, double, WorkloadConfig> {
public:
    using Component::Component;
    /**
     * @brief Busy-spins for config_->workload_us, then copies input_ to
     *        output_.
     * @return void
     */
    void execute() override { busy_spin_us(config_->workload_us); output_ = input_; }
};

// Sink for a 4-fan-in task (this experiment's topology: Ts -> T0..T3 -> Tm).
/** @brief Benchmark sink for a 4-fan-in task (Ts -> T0..T3 -> Tm). */
class BenchSink4 : public SinkComponent<std::array<double, 4>, WorkloadConfig> {
public:
    using SinkComponent::SinkComponent;
    /**
     * @brief Busy-spins for config_->workload_us, discarding the input.
     * @return void
     */
    void execute() override { busy_spin_us(config_->workload_us); }
};

RT_MID_REGISTER_COMPONENT("source",       BenchSource,       WorkloadConfig)
RT_MID_REGISTER_COMPONENT("intermediate", BenchIntermediate, WorkloadConfig)
RT_MID_REGISTER_COMPONENT("sink",         BenchSink4,        WorkloadConfig)

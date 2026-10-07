/**
 * @file allocator_comparison_eval_main.cpp
 *
 * Measures end-to-end response time for one fixed pipeline topology
 * (Ts -> {T0,T1,T2,T3} -> Tm, the demo/Table-I shape already used by
 * tests/performance_test.cpp) under four core placements: every subtask
 * forced onto core 0 ("single_core", the old allocator stub's literal
 * behavior), allocator::apply_auto_allocation with strategy=worst_fit
 * (WF+DRU, "wf_dru"), strategy=eru (Equilibrium Remaining Utilization,
 * "eru" — specs/eru-allocator/), or strategy=tdta (Topology-based DAG
 * Task Allocation, "tdta" — specs/tdta-allocator/), all three real
 * strategies spread across 4 cores. This topology is a single DAG task,
 * so tdta needs no TaskInfo::priority (no ordering ambiguity with only
 * one task). See specs/allocator-comparison-eval/spec.md.
 *
 * No codegen, no JSON plan file: tools/codegen was deleted from this tree
 * (confirmed at spec time), so — like tests/test_flux.cpp and
 * tests/performance_test.cpp already do — this wires the real components
 * directly via rtmid::wire_component and builds its DeploymentPlan
 * in-memory.
 *
 * Same release/finish timing convention as latency_eval_main.cpp:
 * release_ns is the intended periodic release instant (CLOCK_MONOTONIC),
 * finish_ns is when Tm (the sink) completes.
 *
 * Usage: allocator_comparison_eval <single_core|wf_dru|eru|tdta> <freq_hz> [jobs]
 * Output: one CSV line per completed job to stdout
 * (job_index,release_ns,finish_ns,response_us); a summary (avg response
 * time, miss ratio) to stderr.
 */

#include <array>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <time.h>
#include <vector>

#include "allocator.hpp"
#include "adapter.hpp"
#include "dag.hpp"
#include "deployment_plan.hpp"
#include "mcflow_bench_components.hpp"
#include "team_manager.hpp"

namespace {

// Table I's own "high" workload values (already proven representative in
// this codebase, not invented for this tool) — Ts, T0, T1, T2, T3, Tm.
constexpr uint64_t WORKLOAD_US[6] = {900, 1800, 1800, 1800, 1800, 900};
constexpr int ALLOCATED_NUM_CORES = 4; // matches the topology's own fan-out width; shared by wf_dru and eru

constexpr std::size_t N = 32; // ring buffer slot count; see performance_test.cpp's
                               // comment on why the bare formula's minimum isn't used

struct TaskBuffers {
    RingBuffer<double, N> ts_t0, ts_t1, ts_t2, ts_t3;
    MultiSupplierRingBuffer<double, N, 4> join;
};

DeploymentPlan build_plan(uint64_t period_us, const std::string& mode) {
    DeploymentPlan plan;
    TaskInfo task;
    task.id = 1;
    const char* names[6] = {"Ts", "T0", "T1", "T2", "T3", "Tm"};
    for (int i = 0; i < 6; ++i) {
        SubtaskInfo st;
        st.id = i + 1;
        st.component_type = (i == 0) ? "source" : (i == 5) ? "sink" : "intermediate";
        st.priority = 10;
        st.period_us = period_us;
        st.deadline_us = period_us;
        st.wcet_us = WORKLOAD_US[i];
        st.core = (mode == "single_core") ? 0 : CORE_UNASSIGNED;
        st.config = json{{"workload_us", WORKLOAD_US[i]}};
        (void)names[i];
        task.subtasks.push_back(st);
    }
    plan.tasks.push_back(task);
    auto connect = [](int upstream, int downstream) {
        ConnectionInfo c;
        c.upstream = upstream;
        c.downstream = downstream;
        return c;
    };
    for (int i = 2; i <= 5; ++i) plan.connections.push_back(connect(1, i)); // Ts -> T0..T3
    for (int i = 2; i <= 5; ++i) plan.connections.push_back(connect(i, 6)); // T0..T3 -> Tm

    if (mode == "wf_dru") {
        plan.allocation.strategy = "worst_fit";
        plan.allocation.sort_by = "remaining_utilization_desc";
        plan.allocation.weight = "utilization";
        plan.allocation.num_cores = ALLOCATED_NUM_CORES;
        allocator::apply_auto_allocation(plan);
    } else if (mode == "eru") {
        plan.allocation.strategy = "eru"; // sort_by/weight not consulted by eru
        plan.allocation.num_cores = ALLOCATED_NUM_CORES;
        allocator::apply_auto_allocation(plan);
    } else if (mode == "tdta") {
        plan.allocation.strategy = "tdta"; // sort_by/weight not consulted by tdta either
        plan.allocation.num_cores = ALLOCATED_NUM_CORES;
        allocator::apply_auto_allocation(plan);
    }
    return plan;
}

const SubtaskInfo& info_for(const DeploymentPlan& plan, int id) {
    for (const auto& st : plan.tasks[0].subtasks)
        if (st.id == id) return st;
    throw std::runtime_error("allocator_comparison_eval: subtask id " + std::to_string(id) + " not found");
}

// Wires the fixed Ts -> {T0..T3} -> Tm shape against bufs — same pattern
// as tests/performance_test.cpp's wire_task(), specialized to one task.
std::vector<rtmid::WiredNode> wire(const DeploymentPlan& plan, TaskBuffers& bufs) {
    std::vector<rtmid::WiredNode> nodes;
    nodes.push_back(rtmid::wire_component<BenchSource>(1, info_for(plan, 1).config,
        rtmid::no_upstream{},
        rtmid::spsc_writer<double, N>(bufs.ts_t0),
        rtmid::spsc_writer<double, N>(bufs.ts_t1),
        rtmid::spsc_writer<double, N>(bufs.ts_t2),
        rtmid::spsc_writer<double, N>(bufs.ts_t3)));

    nodes.push_back(rtmid::wire_component<BenchIntermediate>(2, info_for(plan, 2).config,
        rtmid::spsc_reader<double, N>(bufs.ts_t0),
        rtmid::multi_writer<double, N, 4>(bufs.join, /*supplier_id=*/0)));
    nodes.push_back(rtmid::wire_component<BenchIntermediate>(3, info_for(plan, 3).config,
        rtmid::spsc_reader<double, N>(bufs.ts_t1),
        rtmid::multi_writer<double, N, 4>(bufs.join, /*supplier_id=*/1)));
    nodes.push_back(rtmid::wire_component<BenchIntermediate>(4, info_for(plan, 4).config,
        rtmid::spsc_reader<double, N>(bufs.ts_t2),
        rtmid::multi_writer<double, N, 4>(bufs.join, /*supplier_id=*/2)));
    nodes.push_back(rtmid::wire_component<BenchIntermediate>(5, info_for(plan, 5).config,
        rtmid::spsc_reader<double, N>(bufs.ts_t3),
        rtmid::multi_writer<double, N, 4>(bufs.join, /*supplier_id=*/3)));

    nodes.push_back(rtmid::wire_component<BenchSink4>(6, info_for(plan, 6).config,
        rtmid::multi_reader<double, N, 4>(bufs.join)));
    return nodes;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: allocator_comparison_eval <single_core|wf_dru|eru|tdta> <freq_hz> [jobs]\n";
        return 1;
    }
    const std::string mode = argv[1];
    if (mode != "single_core" && mode != "wf_dru" && mode != "eru" && mode != "tdta") {
        std::cerr << "unknown mode: " << mode << " (expected single_core, wf_dru, eru, or tdta)\n";
        return 1;
    }
    const double freq_hz = std::atof(argv[2]);
    const int jobs = argc >= 4 ? std::atoi(argv[3]) : 100;
    const uint64_t period_us = static_cast<uint64_t>(std::llround(1'000'000.0 / freq_hz));
    const uint64_t deadline_ns = period_us * 1000ULL;

    DeploymentPlan plan;
    try {
        plan = build_plan(period_us, mode);
    } catch (const std::exception& e) {
        std::cerr << "allocation failed: " << e.what() << "\n";
        return 1;
    }

    DAG dag;
    for (const auto& st : plan.tasks[0].subtasks) dag.add_node(st.id);
    for (const auto& c : plan.connections) dag.add_edge(c.upstream, c.downstream);

    TaskBuffers bufs;
    auto nodes = wire(plan, bufs);

    std::mutex mutex;
    std::vector<uint64_t> release_ns, finish_ns;

    for (auto& n : nodes) {
        if (n.subtask->id != 6) continue; // Tm, the sink
        auto original = n.subtask->execute;
        n.subtask->execute = [original, &mutex, &finish_ns]() {
            original();
            uint64_t now = Dispatcher::monotonic_ns();
            std::lock_guard<std::mutex> lk(mutex);
            finish_ns.push_back(now);
        };
    }

    std::vector<TeamManager::SubtaskEntry> entries;
    for (auto& n : nodes) entries.push_back({info_for(plan, n.subtask->id), n.subtask.get()});

    TeamManager tm;
    tm.initialize(entries, dag);
    tm.start();

    uint64_t next_ns = Dispatcher::monotonic_ns();
    int consecutive_late = 0;
    for (int k = 0; k < jobs; ++k) {
        {
            std::lock_guard<std::mutex> lk(mutex);
            release_ns.push_back(next_ns);
        }
        tm.notify(1);
        next_ns += period_us * 1000ULL;

        const uint64_t now = Dispatcher::monotonic_ns();
        if (next_ns < now) {
            next_ns = now;
            if (++consecutive_late > 5) {
                std::cerr << "  [bail] fell behind 5 periods in a row -- stopping early at job "
                          << k << "/" << jobs << "\n";
                break;
            }
        } else {
            consecutive_late = 0;
        }

        struct timespec ts;
        ts.tv_sec = next_ns / 1'000'000'000ULL;
        ts.tv_nsec = next_ns % 1'000'000'000ULL;
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // drain the last job(s)
    tm.stop();

    const std::size_t n = std::min(release_ns.size(), finish_ns.size());
    uint64_t sum_response_ns = 0;
    std::size_t misses = 0;
    for (std::size_t k = 0; k < n; ++k) {
        const uint64_t response_ns = finish_ns[k] - release_ns[k];
        const double response_us = static_cast<double>(response_ns) / 1000.0;
        std::printf("%zu,%" PRIu64 ",%" PRIu64 ",%.1f\n", k, release_ns[k], finish_ns[k], response_us);
        if (response_ns > deadline_ns) ++misses;
        sum_response_ns += response_ns;
    }
    const double avg_response_us = n > 0 ? static_cast<double>(sum_response_ns) / n / 1000.0 : 0.0;
    const double miss_ratio = n > 0 ? static_cast<double>(misses) / n : 0.0;
    std::cerr << "mode=" << mode << " freq_hz=" << freq_hz << " jobs=" << n
              << " avg_response_us=" << avg_response_us << " miss_ratio=" << miss_ratio << "\n";
    return 0;
}

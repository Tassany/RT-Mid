/**
 * @file interference_eval_main.cpp
 *
 * Measures the response time of a LOW-priority task under interference
 * from a concurrent, higher-priority task (HIGH) sharing the same 4-core
 * pool, under four core placements: everything forced onto core 0
 * ("single_core"), allocator::apply_auto_allocation with
 * strategy=worst_fit ("wf_dru"), strategy=eru ("eru" —
 * specs/eru-allocator/), or strategy=tdta ("tdta" —
 * specs/tdta-allocator/).
 *
 * Unlike allocator_comparison_eval_main.cpp (one task, one flat 4-fan-out
 * DAG — deliberately too shallow to show the allocators apart, per the
 * author's own observation comparing their results), this tool exists to
 * actually separate the three real strategies: TDTA's claimed advantage
 * (Theorem 3) is specifically about reducing the interference a
 * higher-priority task's placement imposes on a LOWER-priority one, AND
 * about reducing a task's own self-interference (Definition 3) among its
 * own branches — neither of which a single-task, unforked benchmark can
 * show. Two DAG tasks, same shape (see interference_topology.hpp for the
 * full rationale on why these shapes/WCETs were picked, and for the
 * wider-5-way-fork and three-task HIGH/MID/LOW variants that were tried
 * and dropped):
 *
 * HIGH (priority 0, the interferer): 8 subtasks, two nested 2-way forks
 *   (Hi -> A,B; A -> A1,A2; B -> B1,B2; {A1,A2,B1,B2} -> Hf) — two Str
 *   structures (Definition 1: {A,B} at level 1, {A1,A2} and {B1,B2} at
 *   level 2). Deliberately tight (18500us total WCET, 9000us critical
 *   path against a 10000us period) so the allocators' placement choices
 *   actually matter (the paper's own Fig. 4 shows the gap between
 *   strategies is largest near this "knee", not deep in a slack-rich safe
 *   zone). Fixed at 100 Hz (interference_topology.hpp's HIGH_PERIOD_US).
 *
 * LOW (priority 1, the victim): the SAME shape as HIGH (Li -> P,Q; ... ->
 *   Lf), WCETs halved — its own two Str structures give TDTA a second,
 *   HIGH-independent mechanism to win on: keeping LOW's own sibling
 *   branches off the same core (self-interference) via the identical
 *   Algorithm 3 it runs for every task, not just the interference HIGH's
 *   placement imposes on it. Its frequency is what this tool sweeps, and
 *   its response time is what's reported.
 *
 * Two DIFFERENT priority fields are in play, and both must be set for
 * this tool to mean anything: TaskInfo::priority (0 for HIGH, 1 for LOW)
 * only drives TDTA's allocation-TIME task ordering (Sect. III's pi(tau),
 * smaller = higher there) and never reaches the runtime at all.
 * SubtaskInfo::priority (30 for HIGH, 10 for LOW, matching
 * plans/mcflow_priority_50hz.json's High/Low convention) is the REAL
 * SCHED_FIFO priority TeamManager applies per (core, priority) Dispatcher
 * thread — get this one wrong (e.g. leave both tasks at the same value)
 * and they silently share one Dispatcher thread whenever co-located,
 * cooperating in FIFO release order instead of HIGH ever preempting LOW,
 * which erases the entire priority-isolation effect this tool exists to
 * measure while still LOOKING like it's running correctly (see the
 * project's own progress notes for how this was found: real PREEMPT_RT
 * runs kept showing unexpectedly wide response-time variance instead of
 * clean priority separation, traced back to exactly this).
 *
 * Caveat carried from every other eval tool in this codebase: without a
 * PREEMPT_RT kernel and root (see the "RT priority not applied" warning),
 * SCHED_FIFO priority preemption is not actually enforced, so what's
 * measured is plain CPU contention on shared cores (CFS time-slicing
 * between busy-spinning threads), not the paper's own idealized
 * priority-isolation guarantee. The allocation DIFFERENCES between
 * strategies (which cores end up shared between HIGH and LOW) are still
 * real and still drive the numbers; the numbers themselves are noisier
 * and less clean than a real PREEMPT_RT host would produce — same
 * limitation tests/performance_test.cpp's own file comment documents.
 *
 * Driver pattern (separate driver thread per task's source, absolute-time
 * releases, pinned off the cores under test, bail-out valve on runaway
 * backlog) copied from tests/performance_test.cpp, which already solved
 * this for three concurrent tasks — see that file's comments for why each
 * piece is there.
 *
 * Usage: interference_eval <single_core|wf_dru|eru|tdta> <low_freq_hz> [jobs]
 * Output: one CSV line per completed LOW job to stdout
 * (job_index,release_ns,finish_ns,response_us); a summary for BOTH tasks
 * (avg response time, miss ratio) to stderr.
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
#include <pthread.h>
#include <sched.h>
#include <string>
#include <thread>
#include <time.h>
#include <vector>

#include "allocator.hpp"
#include "adapter.hpp"
#include "dag.hpp"
#include "deployment_plan.hpp"
#include "interference_topology.hpp"
#include "mcflow_bench_components.hpp"
#include "team_manager.hpp"

using interference_topology::build_plan;
using interference_topology::HIGH_PERIOD_US;

namespace {

uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

constexpr std::size_t N = 32; // see performance_test.cpp's comment on why the bare formula's minimum isn't used

// Field names are generic (not HIGH/LOW-specific) because wire_forked_task
// below is shared between both tasks' identically-shaped topology.
struct ForkedTaskBuffers {
    RingBuffer<double, N> root_a, root_b, a_a1, a_a2, b_b1, b_b2;
    MultiSupplierRingBuffer<double, N, 4> join;
};
using HighBuffers = ForkedTaskBuffers;
using LowBuffers = ForkedTaskBuffers;

const SubtaskInfo& info_for(const DeploymentPlan& plan, int id) {
    for (const auto& task : plan.tasks)
        for (const auto& st : task.subtasks)
            if (st.id == id) return st;
    throw std::runtime_error("interference_eval: subtask id " + std::to_string(id) + " not found");
}

// Shared shape for both HIGH (ids 1-8) and LOW (ids 11-18): Root -> A,B;
// A -> A1,A2; B -> B1,B2; {A1,A2,B1,B2} -> Leaf. id_base is 1 for HIGH, 11
// for LOW, so e.g. Root=id_base, A=id_base+1, ..., Leaf=id_base+7.
std::vector<rtmid::WiredNode> wire_forked_task(const DeploymentPlan& plan, ForkedTaskBuffers& b, int id_base) {
    std::vector<rtmid::WiredNode> nodes;
    const int root = id_base, a = id_base + 1, bb = id_base + 2, a1 = id_base + 3,
              a2 = id_base + 4, b1 = id_base + 5, b2 = id_base + 6, leaf = id_base + 7;
    nodes.push_back(rtmid::wire_component<BenchSource>(root, info_for(plan, root).config,
        rtmid::no_upstream{},
        rtmid::spsc_writer<double, N>(b.root_a),
        rtmid::spsc_writer<double, N>(b.root_b)));
    nodes.push_back(rtmid::wire_component<BenchIntermediate>(a, info_for(plan, a).config,
        rtmid::spsc_reader<double, N>(b.root_a),
        rtmid::spsc_writer<double, N>(b.a_a1),
        rtmid::spsc_writer<double, N>(b.a_a2)));
    nodes.push_back(rtmid::wire_component<BenchIntermediate>(bb, info_for(plan, bb).config,
        rtmid::spsc_reader<double, N>(b.root_b),
        rtmid::spsc_writer<double, N>(b.b_b1),
        rtmid::spsc_writer<double, N>(b.b_b2)));
    nodes.push_back(rtmid::wire_component<BenchIntermediate>(a1, info_for(plan, a1).config,
        rtmid::spsc_reader<double, N>(b.a_a1),
        rtmid::multi_writer<double, N, 4>(b.join, /*supplier_id=*/0)));
    nodes.push_back(rtmid::wire_component<BenchIntermediate>(a2, info_for(plan, a2).config,
        rtmid::spsc_reader<double, N>(b.a_a2),
        rtmid::multi_writer<double, N, 4>(b.join, /*supplier_id=*/1)));
    nodes.push_back(rtmid::wire_component<BenchIntermediate>(b1, info_for(plan, b1).config,
        rtmid::spsc_reader<double, N>(b.b_b1),
        rtmid::multi_writer<double, N, 4>(b.join, /*supplier_id=*/2)));
    nodes.push_back(rtmid::wire_component<BenchIntermediate>(b2, info_for(plan, b2).config,
        rtmid::spsc_reader<double, N>(b.b_b2),
        rtmid::multi_writer<double, N, 4>(b.join, /*supplier_id=*/3)));
    nodes.push_back(rtmid::wire_component<BenchSink4>(leaf, info_for(plan, leaf).config,
        rtmid::multi_reader<double, N, 4>(b.join)));
    return nodes;
}

std::vector<rtmid::WiredNode> wire_high(const DeploymentPlan& plan, HighBuffers& b) {
    return wire_forked_task(plan, b, /*id_base=*/1);
}

std::vector<rtmid::WiredNode> wire_low(const DeploymentPlan& plan, LowBuffers& b) {
    return wire_forked_task(plan, b, /*id_base=*/11);
}

struct TaskTracker {
    std::mutex mutex;
    std::vector<uint64_t> release_ns;
    std::vector<uint64_t> finish_ns;
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: interference_eval <single_core|wf_dru|eru|tdta> <low_freq_hz> [jobs] [--trace]\n";
        return 1;
    }
    const std::string mode = argv[1];
    if (mode != "single_core" && mode != "wf_dru" && mode != "eru" && mode != "tdta") {
        std::cerr << "unknown mode: " << mode << " (expected single_core, wf_dru, eru, or tdta)\n";
        return 1;
    }
    const double low_freq_hz = std::atof(argv[2]);
    const int jobs = argc >= 4 && std::string(argv[3]) != "--trace" ? std::atoi(argv[3]) : 100;
    const uint64_t low_period_us = static_cast<uint64_t>(std::llround(1'000'000.0 / low_freq_hz));
    bool trace_enabled = false;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--trace") trace_enabled = true;

    DeploymentPlan plan;
    try {
        plan = build_plan(low_period_us, mode);
    } catch (const std::exception& e) {
        std::cerr << "allocation failed: " << e.what() << "\n";
        return 1;
    }

    DAG dag;
    for (const auto& t : plan.tasks)
        for (const auto& st : t.subtasks)
            dag.add_node(st.id);
    for (const auto& c : plan.connections)
        dag.add_edge(c.upstream, c.downstream);

    HighBuffers high_bufs;
    LowBuffers low_bufs;
    auto high_nodes = wire_high(plan, high_bufs);
    auto low_nodes = wire_low(plan, low_bufs);

    std::vector<TeamManager::SubtaskEntry> entries;
    for (auto* nodes : {&high_nodes, &low_nodes})
        for (auto& n : *nodes)
            entries.push_back({info_for(plan, n.subtask->id), n.subtask.get()});

    TaskTracker high_t, low_t;
    struct TaskWiring { int source_id, sink_id; uint64_t period_ns; TaskTracker* tracker; };
    TaskWiring wiring[2] = {
        {1, 8, HIGH_PERIOD_US * 1000ULL, &high_t},
        {11, 18, low_period_us * 1000ULL, &low_t},
    };

    // Wrap each task's sink to record finish_ns -- same technique as
    // tests/performance_test.cpp. With --trace, EVERY subtask (not just
    // sinks) is also wrapped to log its own start/finish timestamps and
    // core, as "TRACE,<subtask_id>,<core>,<start_ns>,<finish_ns>" lines
    // to stderr -- this is what lets a post-hoc script reconstruct, per
    // job, how long a subtask (e.g. LOW's Lf) actually waited for its
    // core to free up versus merely waiting on its own DAG predecessors,
    // and attribute that wait to whichever other subtask (e.g. HIGH's A
    // or A2) was occupying the same core right before it -- see the
    // investigation in this tool's own git history / specs for why this
    // specific question (does WF+DRU's placement shift at some
    // frequency increase that contention?) motivated adding this.
    std::mutex trace_mutex;
    for (auto* nodes : {&high_nodes, &low_nodes}) {
        for (auto& n : *nodes) {
            const int sid = n.subtask->id;
            const int core = info_for(plan, sid).core;
            TaskTracker* tracker = nullptr;
            for (auto& w : wiring)
                if (sid == w.sink_id) tracker = w.tracker;
            auto original = n.subtask->execute;
            n.subtask->execute = [sid, core, tracker, original, trace_enabled, &trace_mutex]() {
                uint64_t start_ns = trace_enabled ? now_ns() : 0;
                original();
                if (!trace_enabled && !tracker) return; // skip clock_gettime when nobody needs finish_ns
                uint64_t finish_ns = now_ns();
                if (trace_enabled) {
                    std::lock_guard<std::mutex> lk(trace_mutex);
                    std::fprintf(stderr, "TRACE,%d,%d,%" PRIu64 ",%" PRIu64 "\n",
                                 sid, core, start_ns, finish_ns);
                }
                if (tracker) {
                    std::lock_guard<std::mutex> lk(tracker->mutex);
                    tracker->finish_ns.push_back(finish_ns);
                }
            };
        }
    }

    TeamManager tm;
    tm.initialize(entries, dag);
    tm.start();

    // One driver thread per task's source, pinned off cores 0-3 (the pool
    // under test), absolute-time releases, bail-out valve on runaway
    // backlog -- same pattern and rationale as tests/performance_test.cpp.
    std::vector<std::thread> drivers;
    int driver_idx = 0;
    for (auto& w : wiring) {
        TaskWiring* wp = &w;
        const int driver_core = 10 + driver_idx++;
        drivers.emplace_back([&tm, wp, driver_core, jobs]() {
            cpu_set_t mask;
            CPU_ZERO(&mask);
            CPU_SET(driver_core, &mask);
            pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask);

            struct timespec now_ts;
            clock_gettime(CLOCK_MONOTONIC, &now_ts);
            uint64_t next_ns = static_cast<uint64_t>(now_ts.tv_sec) * 1'000'000'000ULL +
                                static_cast<uint64_t>(now_ts.tv_nsec);
            int consecutive_late = 0;
            for (int k = 0; k < jobs; ++k) {
                {
                    std::lock_guard<std::mutex> lk(wp->tracker->mutex);
                    wp->tracker->release_ns.push_back(next_ns);
                }
                tm.notify(wp->source_id);
                next_ns += wp->period_ns;

                struct timespec chk;
                clock_gettime(CLOCK_MONOTONIC, &chk);
                uint64_t now = static_cast<uint64_t>(chk.tv_sec) * 1'000'000'000ULL +
                               static_cast<uint64_t>(chk.tv_nsec);
                if (next_ns < now) {
                    next_ns = now;
                    if (++consecutive_late > 5) {
                        std::cerr << "  [bail] source " << wp->source_id
                                  << " fell behind 5 periods in a row -- stopping early at job "
                                  << k << "/" << jobs << "\n";
                        break;
                    }
                } else {
                    consecutive_late = 0;
                }

                struct timespec ts;
                ts.tv_sec = static_cast<time_t>(next_ns / 1'000'000'000ULL);
                ts.tv_nsec = static_cast<long>(next_ns % 1'000'000'000ULL);
                clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
            }
        });
    }
    for (auto& th : drivers) th.join();

    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // drain the last job(s)
    tm.stop();

    // LOW's per-job CSV to stdout -- the metric this tool exists to report.
    {
        std::lock_guard<std::mutex> lk(low_t.mutex);
        std::size_t n = std::min(low_t.release_ns.size(), low_t.finish_ns.size());
        for (std::size_t k = 0; k < n; ++k) {
            double response_us = static_cast<double>(low_t.finish_ns[k] - low_t.release_ns[k]) / 1000.0;
            std::printf("%zu,%" PRIu64 ",%" PRIu64 ",%.1f\n", k, low_t.release_ns[k], low_t.finish_ns[k], response_us);
        }
    }

    // Summary for BOTH tasks to stderr.
    auto summarize = [](const char* label, TaskTracker& t, uint64_t deadline_ns) {
        std::lock_guard<std::mutex> lk(t.mutex);
        std::size_t n = std::min(t.release_ns.size(), t.finish_ns.size());
        if (n == 0) { std::fprintf(stderr, "%s: no jobs completed\n", label); return; }
        double sum_us = 0.0;
        int misses = 0;
        for (std::size_t k = 0; k < n; ++k) {
            uint64_t resp_ns = t.finish_ns[k] - t.release_ns[k];
            sum_us += static_cast<double>(resp_ns) / 1000.0;
            if (resp_ns > deadline_ns) ++misses;
        }
        std::fprintf(stderr, "%s: jobs=%zu avg_response_us=%.1f miss_ratio=%.2f\n",
                     label, n, sum_us / static_cast<double>(n), static_cast<double>(misses) / static_cast<double>(n));
    };
    summarize("HIGH", high_t, HIGH_PERIOD_US * 1000ULL);
    summarize("LOW ", low_t, low_period_us * 1000ULL);

    return 0;
}

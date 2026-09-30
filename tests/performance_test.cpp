// Replicates MCFlow (Huang et al. 2012), Section VI-C "Real-time
// Performance": three concurrent tasks (High/Medium/Low priority), each
// shaped like Figure 8 (Ts -> {T0,T1,T2,T3} -> Tm), CPU/workload from
// Table I, sweeping the Low task's rate over 50/60/70/80/90 Hz and
// reproducing Table II (deadline miss ratio) and Table III (avg response
// time). No codegen (deleted) — hand-wired like tests/test_flux.cpp, using
// tests/perf_components.hpp's busy-spin workload components and
// tests/plans/deployment_plan_perf.json's Table I values.
//
// Table I anomaly, kept as printed: T0's Medium-priority workload is
// (CPU 1, 0 us) in the paper — 0, unlike T0's High/Low cells (1800 us
// each). Not "corrected" here.
//
// Deadline = period for every task ("We assume the deadline of each task
// is equal to its period."). High/Medium are fixed at 200Hz/100Hz; only
// Low's rate varies across the sweep, exactly as the paper describes.
//
// What's asserted, and why NOT exact Table II/III numbers: those numbers
// are specific to the paper's own two 6-core i7-980 hosts under real
// SCHED_FIFO; this runs on whatever machine invokes it, and (without root
// — see the "RT priority not applied" warning already printed by
// tests/test_flux.cpp) without real SCHED_FIFO preemption, so exact
// numbers will differ and the priority-isolation guarantee below is only
// as strong as the scheduler actually enforcing it. What IS the paper's
// actual structural claim, independent of hardware, is PRIORITY
// ISOLATION: High and Medium must never miss a deadline regardless of how
// overloaded Low gets, and Low's own miss ratio must not improve as its
// rate increases. Table II's own numbers (High/Med always 0, Low rising
// 0 -> 1.0) are exactly that claim, evaluated on their hardware; this
// test evaluates the same claim on this one.
//
// Build: g++ -std=c++17 -Isrc -Iinclude -Itests tests/performance_test.cpp src/dag.cpp src/parser_json.cpp src/team_manager.cpp -o /tmp/performance_test

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <sched.h>
#include <string>
#include <thread>
#include <time.h>
#include <vector>
#include "parser_json.hpp"
#include "perf_components.hpp"
#include "dag.hpp"
#include "adapter.hpp"
#include "team_manager.hpp"

static void expect(bool cond, const char* what) {
    if (!cond) { std::cerr << "FAIL: " << what << "\n"; std::exit(1); }
    std::cerr << "ok:   " << what << "\n";
}

static const SubtaskInfo& info_for(const DeploymentPlan& plan, int id) {
    for (const auto& t : plan.tasks)
        for (const auto& s : t.subtasks)
            if (s.id == id) return s;
    throw std::runtime_error("performance_test: subtask id " + std::to_string(id) + " not found");
}

namespace {

constexpr int JOBS_PER_TASK = 100; // per frequency point — under real SCHED_FIFO (sudo) the previous 15-job
// run completed the full 5-point sweep without ever hitting the bail-out valve below, so there's headroom
// for a bigger, statistically steadier sample; the valve still protects against a runaway backlog either way.

// One task's release/finish timestamps (same shape as
// tools/eval/latency_eval_main.cpp's TaskTracker).
struct TaskTracker {
    std::mutex mutex;
    std::vector<uint64_t> release_ns;
    std::vector<uint64_t> finish_ns;
};

struct TaskResult {
    double miss_ratio = 0.0;
    double avg_response_us = 0.0;
};

// Ring buffer slot count. ring_buffer_n() gives N = next_pow2(ceil(deadline
// /period) + depth) = next_pow2(4) = 4 here — deadline == period always
// (paper's own assumption), so the ratio is always exactly 1 and the
// formula returns its bare minimum, with zero slack for jitter. That
// minimum is what produced the runaway-backlog finding: Medium's Ts/T0/T1/
// Tm share one core/dispatcher thread, and under real contention from
// High (also on that core), a producer only 4 jobs ahead of a same-thread
// consumer it's competing with for CPU time is enough to start a queue
// that never recovers. N below is a deliberate, generous override of the
// formula's literal output — not derived from it — specifically to test
// whether more backpressure slack prevents that ratchet. If it does,
// that's real evidence the formula (fine in the paper's own model, no
// real contention assumed) needs a jitter margin for a same-thread
// producer/consumer under actual fixed-priority preemption; if it
// doesn't, the bottleneck is elsewhere and this rules the formula out.
constexpr int DEPTH = 3; // Ts -> {T0..T3} -> Tm
constexpr std::size_t N = 32; // was 4 (ring_buffer_n(1, 1, DEPTH)); see comment above

// One task's buffers, declared as plain members — never returned/moved
// (see tests/test_flux.cpp's comment on why buffer lifetime/declaration
// order matters for the reader/writer closures that reference them).
struct TaskBuffers {
    RingBuffer<double, N> ts_t0, ts_t1, ts_t2, ts_t3;
    MultiSupplierRingBuffer<double, N, 4> join;
};

// Wires one task's 6 subtasks (id_base..id_base+5 = Ts,T0,T1,T2,T3,Tm)
// against its own TaskBuffers. Returns owning WiredNodes — safe to move,
// since a WiredNode only holds REFERENCES into bufs (via spsc_reader/
// writer/multi_reader/writer), never owns the buffer memory itself; bufs
// must outlive every node built from it and must never move after this
// call (TaskBuffers is a plain local in run_experiment(), never returned
// by value, so that holds).
std::vector<rtmid::WiredNode> wire_task(const DeploymentPlan& plan, int id_base, TaskBuffers& bufs) {
    std::vector<rtmid::WiredNode> nodes;
    const int ts = id_base, t0 = id_base + 1, t1 = id_base + 2,
              t2 = id_base + 3, t3 = id_base + 4, tm = id_base + 5;

    nodes.push_back(rtmid::wire_component<WorkloadSource>(ts, info_for(plan, ts).config,
        rtmid::no_upstream{},
        rtmid::spsc_writer<double, N>(bufs.ts_t0),
        rtmid::spsc_writer<double, N>(bufs.ts_t1),
        rtmid::spsc_writer<double, N>(bufs.ts_t2),
        rtmid::spsc_writer<double, N>(bufs.ts_t3)));

    nodes.push_back(rtmid::wire_component<WorkloadIntermediate>(t0, info_for(plan, t0).config,
        rtmid::spsc_reader<double, N>(bufs.ts_t0),
        rtmid::multi_writer<double, N, 4>(bufs.join, /*supplier_id=*/0)));
    nodes.push_back(rtmid::wire_component<WorkloadIntermediate>(t1, info_for(plan, t1).config,
        rtmid::spsc_reader<double, N>(bufs.ts_t1),
        rtmid::multi_writer<double, N, 4>(bufs.join, /*supplier_id=*/1)));
    nodes.push_back(rtmid::wire_component<WorkloadIntermediate>(t2, info_for(plan, t2).config,
        rtmid::spsc_reader<double, N>(bufs.ts_t2),
        rtmid::multi_writer<double, N, 4>(bufs.join, /*supplier_id=*/2)));
    nodes.push_back(rtmid::wire_component<WorkloadIntermediate>(t3, info_for(plan, t3).config,
        rtmid::spsc_reader<double, N>(bufs.ts_t3),
        rtmid::multi_writer<double, N, 4>(bufs.join, /*supplier_id=*/3)));

    nodes.push_back(rtmid::wire_component<WorkloadSink>(tm, info_for(plan, tm).config,
        rtmid::multi_reader<double, N, 4>(bufs.join)));

    return nodes;
}

// Runs the whole 3-task pipeline for one Low frequency point, for
// JOBS_PER_TASK jobs per task, and returns each task's {miss_ratio,
// avg_response_us}. Rebuilds everything from scratch (fresh buffers,
// fresh TeamManager) so no state leaks between sweep points.
std::array<TaskResult, 3> run_experiment(DeploymentPlan plan, double low_freq_hz) {
    // Low's period/deadline for this sweep point; deadline == period
    // (paper's own assumption). High/Medium stay fixed at 200Hz/100Hz —
    // the plan file's own values, untouched here.
    const uint64_t low_period_us = static_cast<uint64_t>(std::llround(1'000'000.0 / low_freq_hz));
    for (auto& st : plan.tasks[2].subtasks) {
        st.period_us   = low_period_us;
        st.deadline_us = low_period_us;
    }

    DAG dag;
    for (const auto& t : plan.tasks)
        for (const auto& st : t.subtasks)
            dag.add_node(st.id);
    for (const auto& c : plan.connections)
        dag.add_edge(c.upstream, c.downstream);

    TaskBuffers high_bufs, med_bufs, low_bufs;
    auto high_nodes = wire_task(plan, 1,  high_bufs);
    auto med_nodes  = wire_task(plan, 7,  med_bufs);
    auto low_nodes  = wire_task(plan, 13, low_bufs);

    std::vector<TeamManager::SubtaskEntry> entries;
    for (auto* nodes : { &high_nodes, &med_nodes, &low_nodes })
        for (auto& n : *nodes)
            entries.push_back({info_for(plan, n.subtask->id), n.subtask.get()});

    // One tracker per task; Ts/Tm ids are known directly (id_base and
    // id_base+5) since we assigned them, no need to rediscover them from
    // fan_in/fan_out the way tools/eval/latency_eval_main.cpp does.
    TaskTracker high_t, med_t, low_t;
    struct TaskWiring { int source_id, sink_id; uint64_t period_ns, deadline_ns; TaskTracker* tracker; };
    TaskWiring wiring[3] = {
        {1,  6,  5000ULL * 1000,          5000ULL * 1000,          &high_t},
        {7,  12, 10000ULL * 1000,         10000ULL * 1000,         &med_t},
        {13, 18, low_period_us * 1000ULL, low_period_us * 1000ULL, &low_t},
    };

    // Wrap each task's sink subtask to record finish_ns — same technique
    // as tests/test_flux.cpp's execution-order capture.
    for (auto* nodes : { &high_nodes, &med_nodes, &low_nodes }) {
        for (auto& n : *nodes) {
            for (auto& w : wiring) {
                if (n.subtask->id != w.sink_id) continue;
                TaskTracker* tracker = w.tracker;
                auto original = n.subtask->execute;
                n.subtask->execute = [tracker, original]() {
                    original();
                    uint64_t now = Dispatcher::monotonic_ns();
                    std::lock_guard<std::mutex> lk(tracker->mutex);
                    tracker->finish_ns.push_back(now);
                };
            }
        }
    }

    TeamManager tm;
    tm.initialize(entries, dag);
    tm.start();

    // One driver thread per task's source, same accumulate-absolute-time
    // CLOCK_MONOTONIC pattern as src/main.cpp / latency_eval_main.cpp —
    // simplified realignment on falling behind (snap to now, no missed-
    // release counting/logging), since this is a fixed-length test run,
    // not the production driver.
    //
    // Pinned to cores 10-12 (well outside 0-3 AND outside their HT
    // siblings 4/5 — see the topology check from the hyperthread
    // investigation) — this stands in for the paper's own client host,
    // which physically cannot interfere with the server's cores because
    // it's different hardware entirely (MCFlow paper, Section VI-C:
    // "all the client subtasks are on the same machine and all server
    // subtasks are on the other"). Our driver threads previously ran
    // unpinned SCHED_OTHER, free to land on cores 0-3 and contend with
    // the very Dispatcher threads under test — a discrepancy from the
    // paper's actual two-host setup that this closes.
    std::vector<std::thread> drivers;
    int driver_idx = 0;
    for (auto& w : wiring) {
        TaskWiring* wp = &w;
        const int driver_core = 10 + driver_idx++;
        drivers.emplace_back([&tm, wp, driver_core]() {
            cpu_set_t mask;
            CPU_ZERO(&mask);
            CPU_SET(driver_core, &mask);
            pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask);

            uint64_t next_ns = Dispatcher::monotonic_ns();
            // Bail-out valve: without real SCHED_FIFO (no root — see the
            // "RT priority not applied" warning), CFS shares CPU fairly
            // among every busy-spinning subtask pinned to a core instead
            // of by priority, so a subtask can take far longer in wall
            // time than its workload_us. If that pushes releases behind
            // for too many periods in a row, stop issuing new ones for
            // this task instead of piling an unbounded backlog into the
            // Dispatcher's queue — that pileup is what made an earlier
            // run of this test spin for minutes without finishing.
            int consecutive_late = 0;
            for (int k = 0; k < JOBS_PER_TASK; ++k) {
                {
                    std::lock_guard<std::mutex> lk(wp->tracker->mutex);
                    wp->tracker->release_ns.push_back(next_ns);
                }
                tm.notify(wp->source_id);
                next_ns += wp->period_ns;

                const uint64_t now = Dispatcher::monotonic_ns();
                if (next_ns < now) {
                    next_ns = now; // fell behind: realign, don't burst
                    if (++consecutive_late > 5) {
                        std::cerr << "  [bail] source " << wp->source_id
                                  << " fell behind 5 periods in a row — stopping early at job "
                                  << k << "/" << JOBS_PER_TASK << "\n";
                        break;
                    }
                } else {
                    consecutive_late = 0;
                }

                struct timespec ts;
                ts.tv_sec  = next_ns / 1'000'000'000ULL;
                ts.tv_nsec = next_ns % 1'000'000'000ULL;
                clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
            }
        });
    }
    for (auto& th : drivers) th.join();

    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // drain the last job(s)
    tm.stop();

    std::array<TaskResult, 3> results;
    TaskTracker* trackers[3] = { &high_t, &med_t, &low_t };
    for (int i = 0; i < 3; ++i) {
        TaskTracker* t = trackers[i];
        std::lock_guard<std::mutex> lk(t->mutex);
        const std::size_t n = std::min(t->release_ns.size(), t->finish_ns.size());
        uint64_t sum_response_ns = 0;
        std::size_t misses = 0;
        for (std::size_t k = 0; k < n; ++k) {
            const uint64_t response_ns = t->finish_ns[k] - t->release_ns[k];
            if (response_ns > wiring[i].deadline_ns) ++misses;
            sum_response_ns += response_ns;
        }
        results[i].miss_ratio      = n > 0 ? static_cast<double>(misses) / n : 0.0;
        results[i].avg_response_us = n > 0 ? static_cast<double>(sum_response_ns) / n / 1000.0 : 0.0;
    }

    return results;
}

} // namespace

int main() {
    JsonParser parser;
    DeploymentPlan plan;
    try {
        plan = parser.parse("tests/plans/deployment_plan_perf.json");
    } catch (const std::exception& e) {
        std::cerr << "FAIL: parse() threw: " << e.what() << "\n";
        return 1;
    }
    std::cerr << "ok:   parse() succeeded without throwing\n";
    expect(plan.tasks.size() == 3, "three tasks parsed (High, Medium, Low)");
    expect(plan.tasks[0].subtasks.size() == 6 &&
           plan.tasks[1].subtasks.size() == 6 &&
           plan.tasks[2].subtasks.size() == 6, "six subtasks per task (Ts, T0-T3, Tm)");

    // Throwaway warm-up run, discarded, before the real (measured) sweep
    // below — the very first real-time threads ever created in this
    // process pay a one-time cost (first pthread_create/setaffinity/
    // setschedparam, cold caches) that would otherwise land entirely on
    // whichever frequency point happens to run first, distorting it.
    std::cerr << "--- warm-up (discarded) ---\n";
    run_experiment(plan, 50);

    const double low_freqs[] = {50, 60, 70, 80, 90};

    // Repeats the full 5-point sweep REPEATS times and averages each
    // point's miss_ratio/avg_response_us across repetitions, to smooth
    // out run-to-run scheduling noise instead of reporting a single run.
    // Each repetition's own raw numbers are printed too, so a
    // per-repetition pattern (e.g. 50Hz specifically improving after the
    // first) stays visible instead of being averaged away.
    constexpr int REPEATS = 5;
    double miss_sum[5][3]     = {};
    double response_sum[5][3] = {};

    for (int rep = 0; rep < REPEATS; ++rep) {
        std::cerr << "\n=== Repetition " << (rep + 1) << "/" << REPEATS << " ===\n";
        std::array<TaskResult, 3> rep_results[5];
        for (int i = 0; i < 5; ++i) {
            std::cerr << "\n--- Low @ " << low_freqs[i] << " Hz ---\n";
            rep_results[i] = run_experiment(plan, low_freqs[i]);
            for (int t = 0; t < 3; ++t) {
                miss_sum[i][t]     += rep_results[i][t].miss_ratio;
                response_sum[i][t] += rep_results[i][t].avg_response_us;
            }
        }
        std::printf("\nRepetition %d/%d — Miss Ratios (High/Med/Low per Hz):\n", rep + 1, REPEATS);
        for (int i = 0; i < 5; ++i)
            std::printf("%4gHz %6.2f %6.2f %6.2f\n", low_freqs[i],
                        rep_results[i][0].miss_ratio, rep_results[i][1].miss_ratio, rep_results[i][2].miss_ratio);
    }

    std::array<TaskResult, 3> sweep[5];
    for (int i = 0; i < 5; ++i)
        for (int t = 0; t < 3; ++t) {
            sweep[i][t].miss_ratio      = miss_sum[i][t]     / REPEATS;
            sweep[i][t].avg_response_us = response_sum[i][t] / REPEATS;
        }

    // --- Report: Table II / III shape, averaged over REPEATS runs -------
    std::printf("\nTable II (reproduced, avg of %d runs): Tasks Deadline Miss Ratios\n", REPEATS);
    std::printf("%6s %8s %8s %8s\n", "", "High", "Med", "Low");
    for (int i = 0; i < 5; ++i)
        std::printf("%4gHz %8.2f %8.2f %8.2f\n", low_freqs[i],
                    sweep[i][0].miss_ratio, sweep[i][1].miss_ratio, sweep[i][2].miss_ratio);

    std::printf("\nTable III (reproduced, avg of %d runs): Average Response Times in us\n", REPEATS);
    std::printf("%6s %8s %8s %8s\n", "", "High", "Med", "Low");
    for (int i = 0; i < 5; ++i)
        std::printf("%4gHz %8.0f %8.0f %8.0f\n", low_freqs[i],
                    sweep[i][0].avg_response_us, sweep[i][1].avg_response_us, sweep[i][2].avg_response_us);

    // --- Assert the paper's actual claim: priority isolation ------------
    // NOT exact reproduction of Table II/III's numbers (those are specific
    // to the paper's own hardware under real SCHED_FIFO — see the file
    // comment). What's hardware-independent is the STRUCTURAL property
    // Table II demonstrates: High and Medium never miss a deadline no
    // matter how overloaded Low gets, and Low's own miss ratio does not
    // improve as its rate increases.
    std::cerr << "\n";
    for (int i = 0; i < 5; ++i) {
        expect(sweep[i][0].miss_ratio == 0.0,
               "High never misses a deadline, regardless of Low's rate");
        expect(sweep[i][1].miss_ratio == 0.0,
               "Medium never misses a deadline, regardless of Low's rate");
    }
    for (int i = 1; i < 5; ++i) {
        expect(sweep[i][2].miss_ratio >= sweep[i - 1][2].miss_ratio,
               "Low's miss ratio does not improve as its own rate increases");
    }

    std::cerr << "\nAll checks passed.\n";
    return 0;
}

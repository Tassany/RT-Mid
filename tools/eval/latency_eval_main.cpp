/**
 * @file latency_eval_main.cpp
 *
 * Measures end-to-end response time and deadline miss ratio per task, using
 * the exact definitions from the paper's own evaluation (Section 5,
 * "Metrics"): for the k-th job of a task, release time r_k is when its
 * source subtask becomes eligible for execution — here, the intended
 * periodic release instant (CLOCK_MONOTONIC, same clock and same
 * accumulation style as src/main.cpp's driver), not the moment the
 * Dispatcher actually got around to running it, so dispatch latency counts
 * against the deadline like it should. Finish time f_k is when the task's
 * sink subtask (no successors) completes. Response time rho_k = f_k - r_k;
 * a job misses its deadline when rho_k > D_i (deadline_us from the plan,
 * converted to nanoseconds);
 * the miss ratio is misses / N_i jobs — same formulas as Tables 2-4 of the
 * paper.
 *
 * Assumes one source and one sink per task — the paper's own simplifying
 * assumption ("each task has a single initial subtask and a single
 * terminal subtask").
 *
 * KNOWN LIMITATION, read before trusting numbers under real load: this
 * correlates release k with finish k by ARRIVAL ORDER, not by a per-job
 * sequence number. Today's codegen-generated pipeline (see adapter.hpp)
 * uses one shared round_seq_ per pipeline rather than a true per-job
 * sequence counter, so if a task's period is short enough (or per-subtask
 * work heavy enough) that a new release happens before the previous job's
 * data has fully drained through the ring buffers, this tool's numbers
 * become meaningless without any hard error — only the release/finish
 * count-drift warning below might hint at it, and even that only catches
 * gross cases. Safe for the current demo plan (near-instant processing,
 * multi-millisecond periods); not safe yet for a real WCET-heavy,
 * high-frequency sweep like the paper's Table 3/4. Fixing this means
 * threading a real per-job sequence number through wire_component/codegen,
 * which is separate, deliberately out-of-scope work.
 *
 * Usage: latency_eval [plan_path] [jobs_per_task]
 * Output: one CSV line per job to stdout — meant to feed the project's own
 * Python evaluation scripts — plus a summary per task to stderr.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <time.h>
#include <vector>
#include "parser_json.hpp"
#include "pipeline_generated.hpp"

namespace {

/**
 * @brief Per-task release/finish timestamp log for latency measurement.
 *
 * @var TaskTracker::task_id Task identifier.
 * @var TaskTracker::source_id Id of this task's single source subtask.
 * @var TaskTracker::sink_id Id of this task's single sink subtask.
 * @var TaskTracker::period_ns Source subtask's period, in nanoseconds
 *      (converted from the plan's period_us — compared directly against
 *      monotonic_ns()).
 * @var TaskTracker::deadline_ns Sink subtask's relative deadline, in
 *      nanoseconds (converted from the plan's deadline_us, for the same
 *      reason).
 * @var TaskTracker::mutex Guards release_ns/finish_ns against concurrent
 *      access from driver and dispatcher threads.
 * @var TaskTracker::release_ns release_ns[k]: intended release instant of
 *      job k.
 * @var TaskTracker::finish_ns finish_ns[k]: sink completion instant of
 *      job k.
 */
struct TaskTracker {
    int task_id = 0;
    int source_id = -1;
    int sink_id = -1;
    uint64_t period_ns = 0;
    uint64_t deadline_ns = 0;

    std::mutex mutex;
    std::vector<uint64_t> release_ns; // release_ns[k]: intended release instant of job k
    std::vector<uint64_t> finish_ns;  // finish_ns[k]:  sink completion instant of job k
};

/**
 * @brief Reads the current monotonic time via Dispatcher's clock.
 * @return Current CLOCK_MONOTONIC time, in nanoseconds.
 */
uint64_t monotonic_ns() { return Dispatcher::monotonic_ns(); }

} // namespace

/**
 * @brief Entry point: measures response time and deadline miss ratio.
 *
 * Parses the plan, builds the pipeline, wraps each tracked task's sink
 * execute() to record finish timestamps, drives each task's source for a
 * fixed number of periodic releases while recording intended release
 * timestamps, then prints one CSV line per job (release/finish/response
 * time/deadline/met) to stdout and a per-task summary to stderr.
 *
 * @param argc Argument count.
 * @param argv Argument vector; argv[1] is the plan path (default
 *        "plans/deployment_plan.json"), argv[2] is jobs per task (default
 *        100).
 * @return 0 on success; 1 if the plan fails to parse or the pipeline
 *         fails to start.
 */
int main(int argc, char** argv) {
    const std::string plan_path = argc > 1 ? argv[1] : "plans/deployment_plan.json";
    const int jobs_per_task = argc > 2 ? std::atoi(argv[2]) : 100;

    DeploymentPlan plan;
    try {
        plan = JsonParser{}.parse(plan_path);
    } catch (const std::exception& e) {
        std::cerr << "latency_eval: failed to parse " << plan_path << ": " << e.what() << "\n";
        return 1;
    }

    GeneratedPipeline gp = build_pipeline(plan);

    // One tracker per task: its single source (fan_in==0) and single sink
    // (fan_out==0), per the paper's own single-source/single-sink assumption.
    std::map<int, std::unique_ptr<TaskTracker>> trackers;
    for (const auto& t : plan.tasks) {
        auto tracker = std::make_unique<TaskTracker>();
        tracker->task_id = t.id;
        for (const auto& st : t.subtasks) {
            if (gp.dag.fan_in_count(st.id) == 0)  { tracker->source_id = st.id; tracker->period_ns = st.period_us * 1000; }
            if (gp.dag.fan_out_count(st.id) == 0) { tracker->sink_id   = st.id; tracker->deadline_ns = st.deadline_us * 1000; }
        }
        if (tracker->source_id < 0 || tracker->sink_id < 0) {
            std::cerr << "latency_eval: task " << t.id << " has no single source/sink; skipping\n";
            continue;
        }
        trackers[t.id] = std::move(tracker);
    }

    // Wrap each tracked task's sink subtask execute() to record its finish
    // timestamp — same closure-wrapping technique as
    // tests/codegen_pipeline_test.cpp's execution-order capture.
    for (auto& node : gp.nodes) {
        auto it = std::find_if(trackers.begin(), trackers.end(),
            [&](const auto& kv) { return kv.second->sink_id == node.subtask->id; });
        if (it == trackers.end()) continue;

        TaskTracker* tracker = it->second.get();
        auto original = node.subtask->execute;
        node.subtask->execute = [tracker, original]() {
            original();
            uint64_t now = monotonic_ns();
            std::lock_guard<std::mutex> lk(tracker->mutex);
            tracker->finish_ns.push_back(now);
        };
    }

    TeamManager tm;
    try {
        tm.initialize(gp.entries, gp.dag);
        tm.start();
    } catch (const std::exception& e) {
        std::cerr << "latency_eval: failed to start pipeline: " << e.what() << "\n";
        return 1;
    }

    std::cerr << "latency_eval: running " << plan_path << ", " << jobs_per_task << " jobs per task\n";

    // Drive each task's source for exactly jobs_per_task releases,
    // CLOCK_MONOTONIC-timed the same way as src/main.cpp.
    std::vector<std::thread> drivers;
    for (auto& [task_id, tracker] : trackers) {
        if (tracker->period_ns == 0) {
            std::cerr << "latency_eval: task " << task_id << "'s source is aperiodic; skipping\n";
            continue;
        }
        TaskTracker* t = tracker.get();
        drivers.emplace_back([&tm, t, jobs_per_task]() {
            uint64_t next_ns = monotonic_ns();
            uint64_t skipped = 0;
            for (int k = 0; k < jobs_per_task; ++k) {
                {
                    std::lock_guard<std::mutex> lk(t->mutex);
                    t->release_ns.push_back(next_ns);
                }
                try {
                    tm.notify(t->source_id);
                } catch (const std::exception&) {
                    break;
                }
                next_ns += t->period_ns;

                // Release skipping: under real SCHED_FIFO priority (a lower-
                // priority task's cores can be starved by higher-priority
                // real workload), clock_nanosleep(TIMER_ABSTIME) with a
                // past target returns immediately — without this check, a
                // task that falls behind would fire every missed release
                // back-to-back, bursting jobs_per_task notify() calls
                // almost instantly and exploding a Dispatcher's backlog
                // (this is what caused an actual multi-minute stall while
                // building this tool). Skip ahead to the next period
                // boundary at-or-after now instead of replaying the past.
                const uint64_t now = monotonic_ns();
                if (next_ns < now) {
                    const uint64_t missed = (now - next_ns) / t->period_ns + 1;
                    next_ns += missed * t->period_ns;
                    skipped += missed;
                    std::cerr << "latency_eval: warning: task " << t->task_id
                              << " fell behind, skipped " << missed
                              << " release(s) (total skipped so far: " << skipped << ")\n";
                }

                struct timespec ts;
                ts.tv_sec  = next_ns / 1'000'000'000ULL;
                ts.tv_nsec = next_ns % 1'000'000'000ULL;
                clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
            }
        });
    }
    for (auto& th : drivers) th.join();

    // Give the last job(s) time to drain before stopping.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    tm.stop();

    // --- Report ---
    std::printf("task_id,job_index,release_ns,finish_ns,response_ns,deadline_ns,deadline_met\n");
    for (auto& [task_id, tracker] : trackers) {
        std::lock_guard<std::mutex> lk(tracker->mutex);

        if (tracker->release_ns.size() != tracker->finish_ns.size())
            std::cerr << "latency_eval: warning: task " << task_id << " released "
                      << tracker->release_ns.size() << " jobs but only " << tracker->finish_ns.size()
                      << " finished — release/finish correlation may be unreliable (see file header)\n";

        const std::size_t n = std::min(tracker->release_ns.size(), tracker->finish_ns.size());
        uint64_t sum_response_ns = 0;
        std::size_t misses = 0;

        for (std::size_t k = 0; k < n; ++k) {
            const uint64_t response_ns = tracker->finish_ns[k] - tracker->release_ns[k];
            const bool met = response_ns <= tracker->deadline_ns;
            if (!met) ++misses;
            sum_response_ns += response_ns;
            std::printf("%d,%zu,%llu,%llu,%llu,%llu,%d\n", task_id, k,
                        (unsigned long long)tracker->release_ns[k],
                        (unsigned long long)tracker->finish_ns[k],
                        (unsigned long long)response_ns,
                        (unsigned long long)tracker->deadline_ns,
                        met ? 1 : 0);
        }

        if (n > 0) {
            const double avg_us = static_cast<double>(sum_response_ns) / static_cast<double>(n) / 1000.0;
            const double miss_ratio = static_cast<double>(misses) / static_cast<double>(n);
            std::cerr << "task " << task_id << ": avg response time = " << avg_us
                      << " us, deadline miss ratio = " << miss_ratio
                      << " (" << misses << "/" << n << ")\n";
        }
    }

    return 0;
}

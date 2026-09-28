/**
 * @file main.cpp
 *
 * The actual entry point: parses a deployment plan, builds the real
 * pipeline (via codegen's generated build_pipeline()), starts the real
 * TeamManager, and keeps the process alive — unlike every test so far,
 * which drives a fixed number of rounds and exits.
 *
 * Periodic sources (fan_in == 0, period_us > 0) have no upstream to
 * re-notify them, so nothing in the scheduler itself re-releases them on
 * their own period — one thread per periodic source calls
 * TeamManager::notify() at that rate here. An aperiodic source
 * (period_us == 0) is not driven automatically; this minimal entry point
 * has no other trigger source (network, a timer with a different signal,
 * etc.) for one, so it is simply never ticked — a known, named limitation,
 * not a silent gap.
 *
 * Usage: rt_mid [plan_path]   (defaults to plans/deployment_plan.json)
 * Stop with Ctrl+C (SIGINT) or SIGTERM — shuts down cleanly via
 * TeamManager::stop(), which runs the MCFlow-style termination protocol
 * (see team_manager.cpp) before the process exits.
 */

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>
#include <time.h>
#include <vector>
#include "parser_json.hpp"
#include "pipeline_generated.hpp"

namespace {
std::atomic<bool> g_running{true};
/**
 * @brief SIGINT/SIGTERM handler: requests a clean shutdown.
 * @param Unnamed signal number; ignored.
 * @return void
 */
void handle_signal(int) { g_running.store(false, std::memory_order_relaxed); }
}

/**
 * @brief Entry point: parses a plan, runs the pipeline until signaled.
 *
 * Parses the deployment plan, builds the generated pipeline, starts
 * TeamManager, spawns one driver thread per periodic source to call
 * TeamManager::notify() at that source's period, and blocks until SIGINT
 * or SIGTERM is received, then shuts down via TeamManager::stop().
 *
 * @param argc Argument count.
 * @param argv Argument vector; argv[1], if present, is the plan file path
 *        (defaults to "plans/deployment_plan.json").
 * @return 0 on clean shutdown; 1 if the plan fails to parse or the
 *         pipeline fails to start.
 */
int main(int argc, char** argv) {
    const std::string plan_path = argc > 1 ? argv[1] : "plans/deployment_plan.json";

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    DeploymentPlan plan;
    try {
        plan = JsonParser{}.parse(plan_path);
    } catch (const std::exception& e) {
        std::cerr << "rt_mid: failed to parse " << plan_path << ": " << e.what() << "\n";
        return 1;
    }

    GeneratedPipeline gp = build_pipeline(plan);

    TeamManager tm;
    try {
        tm.initialize(gp.entries, gp.dag);
        tm.start();
    } catch (const std::exception& e) {
        std::cerr << "rt_mid: failed to start pipeline: " << e.what() << "\n";
        return 1;
    }

    std::cerr << "rt_mid: running " << plan_path << " (Ctrl+C to stop)\n";

    // One driver thread per periodic source, timed on CLOCK_MONOTONIC —
    // same clock and same absolute-time-accumulation style as
    // Dispatcher::monotonic_ns()/its timerfd arming, rather than a second,
    // different time source (std::chrono::steady_clock) for the same job.
    // Accumulating the next release time (instead of sleeping `period_ns`
    // relative to "now" each iteration) avoids drift from notify()'s own
    // execution time.
    //
    // Release skipping: if the system falls far enough behind that
    // next_ns is already in the past by the time we get back here (real
    // SCHED_FIFO priority starvation on an overloaded core is exactly when
    // this happens — a lower-priority source can be shut out of the CPU
    // for a while), clock_nanosleep(TIMER_ABSTIME) with a past target
    // returns immediately. Without this check, that turns into an
    // unbounded burst of notify() calls trying to replay every missed
    // release back-to-back, which can grow a Dispatcher's backlog without
    // bound. Real-time systems handle this by skipping the missed releases
    // and realigning to the next period boundary at-or-after now, which is
    // what this does — logging how many were skipped rather than silently
    // dropping them.
    std::vector<std::thread> drivers;
    for (const auto& entry : gp.entries) {
        if (gp.dag.fan_in_count(entry.info.id) != 0) continue; // not a source
        if (entry.info.period_us == 0) continue;               // aperiodic: not driven here

        const int id = entry.info.id;
        // entry.info.period_us is the plan's microsecond value; this loop
        // accumulates absolute CLOCK_MONOTONIC time, which is nanoseconds.
        const uint64_t period_ns = entry.info.period_us * 1000;
        drivers.emplace_back([&tm, id, period_ns]() {
            uint64_t next_ns = Dispatcher::monotonic_ns();
            uint64_t skipped = 0;
            while (g_running.load(std::memory_order_relaxed)) {
                try {
                    tm.notify(id);
                } catch (const std::exception&) {
                    break; // TeamManager is stopping under us
                }
                next_ns += period_ns;

                const uint64_t now = Dispatcher::monotonic_ns();
                if (next_ns < now) {
                    const uint64_t missed = (now - next_ns) / period_ns + 1;
                    next_ns += missed * period_ns;
                    skipped += missed;
                    std::cerr << "rt_mid: warning: source " << id << " fell behind, skipped "
                              << missed << " release(s) (total skipped so far: " << skipped << ")\n";
                }

                struct timespec ts;
                ts.tv_sec  = next_ns / 1'000'000'000ULL;
                ts.tv_nsec = next_ns % 1'000'000'000ULL;
                clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
            }
        });
    }

    while (g_running.load(std::memory_order_relaxed))
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    for (auto& t : drivers) t.join();

    tm.stop();
    std::cerr << "rt_mid: stopped\n";
    return 0;
}

/**
 * @file fonseca_wcrt_eval_main.cpp
 *
 * Computes rta_fonseca2016::compute_wcrt (Fonseca, Nelissen, Nélis &
 * Pinho, RTCSA 2016 — specs/rta-fonseca2016/) for
 * tools/eval/interference_topology.hpp's own HIGH/LOW plan, under each
 * core placement tools/eval/interference_eval_main.cpp already measures
 * (single_core/wf_dru/eru/tdta). Deliberately Fonseca-only — see
 * tools/eval/wcrt_eval_main.cpp for the sibling tool that computes
 * specs/rrc-analysis/'s own bound on the identical plan instead; the two
 * are kept as separate tools/outputs rather than one combined report.
 *
 * No execution at all: rta_fonseca2016::compute_wcrt is a pure
 * calculation over an already-allocated DeploymentPlan — no TeamManager,
 * no ring buffers, no driver threads, no PREEMPT_RT kernel needed.
 * Deterministic given (topology, WCETs, mode, low_freq_hz).
 *
 * Usage: fonseca_wcrt_eval <single_core|wf_dru|eru|tdta> <low_freq_hz>
 * Output: one CSV line to stdout:
 * mode,low_freq_hz,R_HIGH,D_HIGH,schedulable_HIGH,R_LOW,D_LOW,schedulable_LOW
 * A human-readable mirror goes to stderr.
 */

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "allocator.hpp"
#include "deployment_plan.hpp"
#include "interference_topology.hpp"
#include "rta_fonseca2016.hpp"

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: fonseca_wcrt_eval <single_core|wf_dru|eru|tdta> <low_freq_hz>\n";
        return 1;
    }
    const std::string mode = argv[1];
    if (mode != "single_core" && mode != "wf_dru" && mode != "eru" && mode != "tdta") {
        std::cerr << "unknown mode: " << mode << " (expected single_core, wf_dru, eru, or tdta)\n";
        return 1;
    }
    const double low_freq_hz = std::atof(argv[2]);
    const uint64_t low_period_us = static_cast<uint64_t>(std::llround(1'000'000.0 / low_freq_hz));

    DeploymentPlan plan;
    try {
        plan = interference_topology::build_plan(low_period_us, mode);
    } catch (const std::exception& e) {
        std::cerr << "allocation failed: " << e.what() << "\n";
        return 1;
    }

    try {
        const TaskInfo& high = plan.tasks[0];
        const TaskInfo& low = plan.tasks[1];

        double r_high = rta_fonseca2016::compute_wcrt(plan, high);
        double r_low = rta_fonseca2016::compute_wcrt(plan, low);
        double d_high = static_cast<double>(high.subtasks.front().deadline_us);
        double d_low = static_cast<double>(low.subtasks.front().deadline_us);
        bool sched_high = r_high <= d_high;
        bool sched_low = r_low <= d_low;

        std::printf("%s,%.1f,%.1f,%.1f,%s,%.1f,%.1f,%s\n", mode.c_str(), low_freq_hz, r_high, d_high,
                    sched_high ? "yes" : "no", r_low, d_low, sched_low ? "yes" : "no");

        std::fprintf(stderr,
                      "mode=%-11s low_freq_hz=%6.1f | HIGH: R=%10.1f D=%10.1f schedulable=%-3s | "
                      "LOW: R=%10.1f D=%10.1f schedulable=%-3s\n",
                      mode.c_str(), low_freq_hz, r_high, d_high, sched_high ? "yes" : "no", r_low, d_low,
                      sched_low ? "yes" : "no");
    } catch (const std::exception& e) {
        std::cerr << "rta_fonseca2016 analysis failed: " << e.what() << "\n";
        return 1;
    }
    return 0;
}

#!/usr/bin/env python3
"""Plots the LOW-priority task's average response time, under interference
from a concurrent HIGH-priority task sharing the same 4-core pool, across
four core placements: single_core, wf_dru, eru, tdta. See
tools/eval/interference_eval_main.cpp's own header comment for why this
exists (allocator_comparison_eval / plot_allocator_comparison's flat
single-task topology is too shallow to separate wf_dru/eru/tdta) and for
the same "no PREEMPT_RT in this sandbox" caveat every eval tool in this
codebase carries.

Builds tools/eval/interference_eval_main.cpp once, then for each
(LOW frequency, mode) point runs it --repeats times (default 3, same
"average of N repetitions" convention tests/performance_test.cpp already
uses for its own Table II/III), parses LOW's per-job CSV (stdout) each
time, and plots the mean of those per-repetition averages with error bars
(+/- 1 standard deviation across repetitions). HIGH's own summary
(stderr) is echoed but not plotted -- it exists only as the interference
source, not the metric of interest here.

The error bars matter here specifically: a single run's average at one
frequency can land far from its neighbors purely from one-off OS jitter,
or from a genuine "harmonic" resonance between HIGH's fixed period and
that specific LOW frequency (a low-denominator rational ratio between the
two periods can make a worst-case phase alignment recur every hyperperiod
instead of drifting/averaging out -- a known real-time-scheduling effect,
not necessarily a bug). Repeating each point is how you tell those apart:
a tall error bar means "this point is noisy, don't trust one run of it";
a short error bar on a point that's still far from its neighbors means
the effect is reproducible.

Usage: python3 tools/eval/plot_interference_eval.py [out.png]
       [--freqs 50,60,70,80,90] [--jobs 100] [--repeats 3]
       [--modes single_core,wf_dru,eru,tdta]
"""
import argparse
import csv
import io
import math
import os
import statistics
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CXX = "g++"
CXXFLAGS = ["-std=c++17", "-O2", "-Wall", "-Wextra", f"-I{ROOT}/src", f"-I{ROOT}/include"]
SRC = os.path.join(ROOT, "tools", "eval", "interference_eval_main.cpp")
# allocator.cpp's dispatcher references every strategy unconditionally, so
# linking it needs every strategy's own .cpp too (see Makefile's
# ALLOCATOR_SRCS comment -- kept in sync with it by hand, since this
# script builds independently of the Makefile).
EXTRA_SRCS = [os.path.join(ROOT, "tools", "eval", "interference_topology.cpp"),
              os.path.join(ROOT, "src", "dag.cpp"), os.path.join(ROOT, "src", "team_manager.cpp"),
              os.path.join(ROOT, "src", "allocator.cpp"), os.path.join(ROOT, "src", "dru.cpp"),
              os.path.join(ROOT, "src", "eru.cpp"), os.path.join(ROOT, "src", "ied.cpp"),
              os.path.join(ROOT, "src", "tdta.cpp")]

ALL_MODES = ["single_core", "wf_dru", "eru", "tdta"]
MODE_LABEL = {
    "single_core": "Single core (no allocator)",
    "wf_dru": "WF+DRU (4 cores)",
    "eru": "ERU (4 cores)",
    "tdta": "TDTA (4 cores)",
}


def build(bin_path: str) -> None:
    cmd = [CXX, *CXXFLAGS, SRC, *EXTRA_SRCS, "-o", bin_path]
    print("building:", " ".join(cmd), file=sys.stderr)
    subprocess.run(cmd, check=True, cwd=ROOT)


def run_point(bin_path: str, mode: str, low_freq_hz: float, jobs: int):
    """Runs the eval binary once, returns LOW's average response time (us)
    over the jobs it actually completed (parsed from its own CSV output),
    or None if LOW completed zero jobs -- seen in practice for
    single_core at low frequencies, where LOW's source blocks on
    backpressure forever behind HIGH's own overload and never finishes a
    single job. There's no finite response time to report for "never
    finished," so the caller treats None as a gap in that line rather
    than crashing or plotting a made-up number.
    HIGH's/LOW's stderr summary line is echoed for visibility."""
    proc = subprocess.run(
        [bin_path, mode, str(low_freq_hz), str(jobs)],
        cwd=ROOT, check=True, capture_output=True, text=True,
    )
    for line in proc.stderr.splitlines():
        if line.startswith("HIGH:") or line.startswith("LOW "):
            print(f"    {line}", file=sys.stderr)
    rows = list(csv.reader(io.StringIO(proc.stdout)))
    if not rows:
        print(f"    WARNING: no LOW jobs completed for mode={mode} freq={low_freq_hz}", file=sys.stderr)
        return None
    response_us = [float(r[3]) for r in rows]
    return sum(response_us) / len(response_us)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("out", nargs="?", default=os.path.join(ROOT, "interference_eval.png"))
    ap.add_argument("--freqs", default="50,60,70,80,90")
    ap.add_argument("--jobs", type=int, default=100)
    ap.add_argument("--repeats", type=int, default=3)
    ap.add_argument("--modes", default=",".join(ALL_MODES))
    args = ap.parse_args()
    freqs = [float(f) for f in args.freqs.split(",")]
    modes = args.modes.split(",")
    for m in modes:
        if m not in ALL_MODES:
            ap.error(f"unknown mode {m!r} (expected one of {ALL_MODES})")

    bin_path = os.path.join(ROOT, "build", "interference_eval")
    os.makedirs(os.path.dirname(bin_path), exist_ok=True)
    build(bin_path)

    means = {mode: [] for mode in modes}
    stdevs = {mode: [] for mode in modes}
    for low_freq_hz in freqs:
        for mode in modes:
            print(f"  {mode:11s} LOW={low_freq_hz:5.0f}Hz  ({args.repeats} repeats)", file=sys.stderr)
            per_repeat = []
            for rep in range(args.repeats):
                avg_us = run_point(bin_path, mode, low_freq_hz, args.jobs)
                if avg_us is None:
                    print(f"    rep {rep + 1}/{args.repeats}: LOW completed zero jobs (skipped)", file=sys.stderr)
                    continue
                per_repeat.append(avg_us)
                print(f"    rep {rep + 1}/{args.repeats}: LOW avg response {avg_us:8.1f}us", file=sys.stderr)
            if not per_repeat:
                print(f"    -> all {args.repeats} repeats completed zero LOW jobs; plotting a gap here", file=sys.stderr)
                means[mode].append(math.nan)
                stdevs[mode].append(0.0)
                continue
            mean_us = statistics.mean(per_repeat)
            stdev_us = statistics.stdev(per_repeat) if len(per_repeat) > 1 else 0.0
            means[mode].append(mean_us)
            stdevs[mode].append(stdev_us)
            print(f"    -> mean {mean_us:8.1f}us  stdev {stdev_us:8.1f}us"
                  + (f"  ({args.repeats - len(per_repeat)} repeat(s) skipped: zero jobs)"
                     if len(per_repeat) < args.repeats else ""), file=sys.stderr)

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, ax = plt.subplots(figsize=(8, 5))
    for mode in modes:
        ax.errorbar(freqs, means[mode], yerr=stdevs[mode], marker="o", capsize=3, label=MODE_LABEL[mode])
    ax.set_xlabel("LOW task source frequency (Hz)")
    ax.set_ylabel("LOW task average end-to-end response time (µs)")
    ax.set_title(f"LOW-priority task response time under HIGH-priority interference "
                 f"(mean +/- stdev, {args.repeats} repeats)")
    ax.legend()
    ax.grid(True, alpha=0.3)
    fig.tight_layout()
    fig.savefig(args.out, dpi=150)
    print(f"saved: {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()

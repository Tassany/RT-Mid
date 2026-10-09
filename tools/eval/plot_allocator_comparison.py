#!/usr/bin/env python3
"""Compares end-to-end response time under four core placements for the
same fixed pipeline (Ts -> {T0,T1,T2,T3} -> Tm, Table I's "high" workload
values): everything forced onto core 0 ("single_core", the old allocator
stub's literal behavior), WF+DRU spreading subtasks across 4 cores
("wf_dru"), ERU (Equilibrium Remaining Utilization, Algorithm 2 of Wu
et al. 2023 — specs/eru-allocator/) also across 4 cores ("eru"), and TDTA
(Topology-based DAG Task Allocation, Algorithm 3 of Wu et al. 2023 —
specs/tdta-allocator/) also across 4 cores ("tdta"). See
specs/allocator-comparison-eval/spec.md.

Builds tools/eval/allocator_comparison_eval_main.cpp once, then for each
frequency in the sweep runs it once per mode, parses its per-job CSV
(stdout), averages response time, and plots all as lines.

No codegen involved (deleted from this tree) — the eval binary builds its
DeploymentPlan in-memory and is a fixed, self-contained topology.

Usage: python3 tools/eval/plot_allocator_comparison.py [out.png]
       [--freqs 50,100,150,200,250,300] [--jobs 100]
       [--modes single_core,wf_dru,eru,tdta]

--modes selects which of the four to run and plot (comma-separated,
order preserved) — e.g. --modes wf_dru,eru,tdta to compare the three real
allocators without single_core's much larger values dominating the
y-axis scale.

Real-time-accurate numbers require running this on a machine with a
PREEMPT_RT kernel under sudo (see the "RT priority not applied" warning
otherwise printed by the eval binary) — without it, numbers still plot but
are noisier/less representative, same caveat as tests/performance_test.cpp.
"""
import argparse
import csv
import io
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CXX = "g++"
CXXFLAGS = ["-std=c++17", "-O2", "-Wall", "-Wextra", f"-I{ROOT}/src", f"-I{ROOT}/include"]
SRC = os.path.join(ROOT, "tools", "eval", "allocator_comparison_eval_main.cpp")
# allocator.cpp's dispatcher references every strategy unconditionally,
# so linking it needs every strategy's own .cpp too (see Makefile's
# ALLOCATOR_SRCS comment -- kept in sync with it by hand, since this
# script builds independently of the Makefile).
EXTRA_SRCS = [os.path.join(ROOT, "src", "dag.cpp"), os.path.join(ROOT, "src", "team_manager.cpp"),
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


def run_point(bin_path: str, mode: str, freq_hz: float, jobs: int) -> float:
    """Runs the eval binary once, returns the average response time (us)
    over the jobs it actually completed (parsed from its own CSV output,
    not recomputed here, so this stays consistent with what the binary's
    own stderr summary reports)."""
    proc = subprocess.run(
        [bin_path, mode, str(freq_hz), str(jobs)],
        cwd=ROOT, check=True, capture_output=True, text=True,
    )
    rows = list(csv.reader(io.StringIO(proc.stdout)))
    if not rows:
        raise RuntimeError(f"no jobs completed for mode={mode} freq={freq_hz}: {proc.stderr}")
    response_us = [float(r[3]) for r in rows]
    return sum(response_us) / len(response_us)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("out", nargs="?", default=os.path.join(ROOT, "allocator_comparison.png"))
    ap.add_argument("--freqs", default="50,100,150,200,250,300")
    ap.add_argument("--jobs", type=int, default=100)
    ap.add_argument("--modes", default=",".join(ALL_MODES))
    args = ap.parse_args()
    freqs = [float(f) for f in args.freqs.split(",")]
    modes = [m.strip() for m in args.modes.split(",")]
    for m in modes:
        if m not in ALL_MODES:
            ap.error(f"unknown mode {m!r} (expected one of {ALL_MODES})")

    bin_path = os.path.join(ROOT, "build", "allocator_comparison_eval")
    os.makedirs(os.path.dirname(bin_path), exist_ok=True)
    build(bin_path)

    results = {mode: [] for mode in modes}
    for freq_hz in freqs:
        for mode in modes:
            avg_us = run_point(bin_path, mode, freq_hz, args.jobs)
            results[mode].append(avg_us)
            print(f"  {mode:11s} {freq_hz:6.0f}Hz -> avg response {avg_us:8.1f}us", file=sys.stderr)

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, ax = plt.subplots(figsize=(8, 5))
    for mode in modes:
        ax.plot(freqs, results[mode], marker="o", label=MODE_LABEL[mode])
    ax.set_xlabel("Source frequency (Hz)")
    ax.set_ylabel("Average end-to-end response time (µs)")
    ax.set_title(" vs ".join(MODE_LABEL[m].split(" (")[0] for m in modes) + ": response time vs. load")
    ax.legend()
    ax.grid(True, alpha=0.3)
    fig.tight_layout()
    fig.savefig(args.out, dpi=150)
    print(f"saved: {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()

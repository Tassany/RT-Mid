#!/usr/bin/env python3
"""Plots rrc::compute_wcrt's theoretical R(tau_LOW) bound (Sect. IV-A,
Eq. 1-4) for tools/eval/interference_topology.hpp's own HIGH/LOW plan,
across single_core/wf_dru/eru/tdta -- the calculated counterpart to
tools/eval/plot_interference_eval.py's MEASURED response-time plot, for
the identical topology (both tools share interference_topology.hpp/.cpp,
so they can never silently drift onto different plans).

Deterministic, not noisy: rrc::compute_wcrt takes no measurements and
burns no CPU time -- same (topology, WCETs, mode, low_freq_hz) always
gives the same R. No --repeats here, unlike plot_interference_eval.py.

The two plots can disagree, and that disagreement is itself informative,
not a bug in either one: Eq. 1-4 compute a *worst-case* upper bound
(deliberately conservative -- Eq. 3's ceil(R/T_j) term assumes every job
of the higher-priority task over that whole window interferes maximally),
while plot_interference_eval.py measures *typical-case* behavior across
whatever releases a real (or simulated) run actually produced. A
strategy can look best on one and not the other without either number
being wrong.

Usage: python3 tools/eval/plot_wcrt_eval.py [out.png]
       [--freqs 50,60,70,80,90] [--modes single_core,wf_dru,eru,tdta]
"""
import argparse
import csv
import io
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CXX = "g++"
CXXFLAGS = ["-std=c++17", "-O2", "-Wall", "-Wextra", f"-I{ROOT}/src", f"-I{ROOT}/include", f"-I{ROOT}/tools/eval"]
SRC = os.path.join(ROOT, "tools", "eval", "wcrt_eval_main.cpp")
# allocator.cpp's dispatcher references every strategy unconditionally, so
# linking it needs every strategy's own .cpp too (see Makefile's
# ALLOCATOR_SRCS comment -- kept in sync with it by hand, since this
# script builds independently of the Makefile).
EXTRA_SRCS = [os.path.join(ROOT, "tools", "eval", "interference_topology.cpp"),
              os.path.join(ROOT, "src", "dag.cpp"),
              os.path.join(ROOT, "src", "allocator.cpp"), os.path.join(ROOT, "src", "dru.cpp"),
              os.path.join(ROOT, "src", "eru.cpp"), os.path.join(ROOT, "src", "ied.cpp"),
              os.path.join(ROOT, "src", "tdta.cpp"), os.path.join(ROOT, "src", "rrc.cpp")]

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


def run_point(bin_path: str, mode: str, low_freq_hz: float):
    """Runs the eval binary once, returns (R_LOW, D_LOW, schedulable_LOW)
    parsed from its own CSV line (stdout)."""
    proc = subprocess.run(
        [bin_path, mode, str(low_freq_hz)],
        cwd=ROOT, check=True, capture_output=True, text=True,
    )
    row = next(csv.reader(io.StringIO(proc.stdout.strip())))
    # mode,low_freq_hz,R_HIGH,D_HIGH,schedulable_HIGH,R_LOW,D_LOW,schedulable_LOW
    r_low, d_low, schedulable_low = float(row[5]), float(row[6]), row[7] == "yes"
    for line in proc.stderr.splitlines():
        print(f"    {line}", file=sys.stderr)
    return r_low, d_low, schedulable_low


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("out", nargs="?", default=os.path.join(ROOT, "wcrt_eval.png"))
    ap.add_argument("--freqs", default="50,60,70,80,90")
    ap.add_argument("--modes", default=",".join(ALL_MODES))
    args = ap.parse_args()
    freqs = [float(f) for f in args.freqs.split(",")]
    modes = args.modes.split(",")
    for m in modes:
        if m not in ALL_MODES:
            ap.error(f"unknown mode {m!r} (expected one of {ALL_MODES})")

    bin_path = os.path.join(ROOT, "build", "wcrt_eval")
    os.makedirs(os.path.dirname(bin_path), exist_ok=True)
    build(bin_path)

    r_low = {mode: [] for mode in modes}
    d_low = {mode: [] for mode in modes}
    schedulable = {mode: [] for mode in modes}
    for freq_hz in freqs:
        for mode in modes:
            r, d, ok = run_point(bin_path, mode, freq_hz)
            r_low[mode].append(r)
            d_low[mode].append(d)
            schedulable[mode].append(ok)

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, ax = plt.subplots(figsize=(8, 5))
    for mode in modes:
        line, = ax.plot(freqs, r_low[mode], marker="o", label=MODE_LABEL[mode])
        # Mark unschedulable points (R > D) with a red X on top of the line.
        bad_x = [f for f, ok in zip(freqs, schedulable[mode]) if not ok]
        bad_y = [r for r, ok in zip(r_low[mode], schedulable[mode]) if not ok]
        if bad_x:
            ax.scatter(bad_x, bad_y, marker="x", color="red", s=80, zorder=5)
    # One deadline curve (D_LOW is the same implicit-deadline-equals-period
    # value for every mode at a given frequency).
    ax.plot(freqs, d_low[modes[0]], linestyle="--", color="gray", label="D_LOW (deadline)")
    ax.set_xlabel("LOW task source frequency (Hz)")
    ax.set_ylabel("Theoretical R(tau_LOW) bound (µs, Eq. 1-4)")
    ax.set_title("RRC worst-case response-time bound for LOW (red X = unschedulable, R > D)")
    ax.legend()
    ax.grid(True, alpha=0.3)
    fig.tight_layout()
    fig.savefig(args.out, dpi=150)
    print(f"saved: {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()

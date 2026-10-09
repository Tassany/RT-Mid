#!/usr/bin/env python3
"""Prints a Table-II-style table (Wu et al. 2023's own "Tasks Deadline
Miss Ratios" shape, same convention tests/performance_test.cpp already
reproduces for the MCFlow paper) for the LOW-priority task's deadline
miss ratio, across 50/60/70/80/90 Hz, one column per core-placement mode
(single_core/wf_dru/eru/tdta), each cell averaged over --repeats runs.

Companion to tools/eval/plot_interference_eval.py (same binary, same
HIGH/LOW interference scenario -- see tools/eval/interference_eval_main.cpp's
own file comment for the topology and the two-priority-fields caveat) but
reporting miss ratio instead of response time: miss ratio is parsed
directly from the eval binary's own stderr summary line ("LOW :
jobs=... avg_response_us=... miss_ratio=...") rather than recomputed from
the per-job CSV, so it always matches exactly what the binary itself
computed against LOW's own deadline.

Usage: python3 tools/eval/table_interference_eval.py [out.txt]
       [--freqs 50,60,70,80,90] [--jobs 100] [--repeats 10]
       [--modes single_core,wf_dru,eru,tdta]
"""
import argparse
import math
import os
import re
import statistics
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CXX = "g++"
CXXFLAGS = ["-std=c++17", "-O2", "-Wall", "-Wextra", f"-I{ROOT}/src", f"-I{ROOT}/include", f"-I{ROOT}/tools/eval"]
SRC = os.path.join(ROOT, "tools", "eval", "interference_eval_main.cpp")
TOPOLOGY_SRC = os.path.join(ROOT, "tools", "eval", "interference_topology.cpp")
# allocator.cpp's dispatcher references every strategy unconditionally, so
# linking it needs every strategy's own .cpp too (see Makefile's
# ALLOCATOR_SRCS comment -- kept in sync with it by hand, since this
# script builds independently of the Makefile).
EXTRA_SRCS = [os.path.join(ROOT, "src", "dag.cpp"), os.path.join(ROOT, "src", "team_manager.cpp"),
              os.path.join(ROOT, "src", "allocator.cpp"), os.path.join(ROOT, "src", "dru.cpp"),
              os.path.join(ROOT, "src", "eru.cpp"), os.path.join(ROOT, "src", "ied.cpp"),
              os.path.join(ROOT, "src", "tdta.cpp")]

ALL_MODES = ["single_core", "wf_dru", "eru", "tdta"]
MODE_LABEL = {  # short form, used as table column headers
    "single_core": "Single",
    "wf_dru": "WF+DRU",
    "eru": "ERU",
    "tdta": "TDTA",
}
MODE_CAPTION_LABEL = {  # long form, used in auto-generated LaTeX captions
    "single_core": "single-core",
    "wf_dru": "WF+DRU",
    "eru": "ERU",
    "tdta": "TDTA",
}

LOW_LINE_RE = re.compile(r"^LOW\s*:\s*jobs=(\d+)\s+avg_response_us=([\d.]+)\s+miss_ratio=([\d.]+)")
HIGH_LINE_RE = re.compile(r"^HIGH\s*:\s*jobs=(\d+)\s+avg_response_us=([\d.]+)\s+miss_ratio=([\d.]+)")


def fmt_miss_ratio(x: float) -> str:
    """Formats a miss ratio for the LaTeX table: 2 decimals, trailing zero
    dropped unless that would remove the decimal point entirely (1.00 ->
    "1.0", 0.20 -> "0.2", but 0.01/0.37 keep both digits) -- matches the
    author's own example table's mixed precision."""
    s = f"{x:.2f}"
    if s.endswith("0"):  # drop one trailing zero: "1.00"->"1.0", "0.20"->"0.2"; "0.01"/"0.37" keep both digits
        s = s[:-1]
    return s


def build_latex_table(freqs, modes, low_miss, caption: str, label: str) -> str:
    """Renders the LOW task's miss ratio as a LaTeX table in the same
    structure as the paper's own Table II: one row per frequency, one
    column per mode, no HIGH column (see interference_eval_main.cpp's
    stderr output / --low-only for HIGH's own numbers when sanity-checking
    this table's validity)."""
    col_spec = "|c|" + "c|" * len(modes)
    lines = [
        r"\begin{table}[]",
        r"\centering",
        rf"\begin{{tabular}}{{{col_spec}}}",
        r"\hline",
        r"\textbf{} & " + " & ".join(rf"\textbf{{{MODE_LABEL[m]}}}" for m in modes) + r" \\ \hline",
    ]
    for i, freq in enumerate(freqs):
        cells = " & ".join(fmt_miss_ratio(low_miss[m][i]) for m in modes)
        lines.append(rf"{freq:g} & {cells} \\ \hline")
    lines += [
        r"\end{tabular}",
        rf"\caption{{{caption}}}",
        rf"\label{{{label}}}",
        r"\end{table}",
    ]
    return "\n".join(lines)


def build(bin_path: str) -> None:
    cmd = [CXX, *CXXFLAGS, SRC, TOPOLOGY_SRC, *EXTRA_SRCS, "-o", bin_path]
    print("building:", " ".join(cmd), file=sys.stderr)
    subprocess.run(cmd, check=True, cwd=ROOT)


def run_point(bin_path: str, mode: str, low_freq_hz: float, jobs: int):
    """Runs the eval binary once, returns (low_miss_ratio, high_miss_ratio)
    parsed from its own stderr summary lines -- the binary's own
    computation against each task's real deadline, not re-derived here.

    A task that completes zero jobs (interference_eval_main.cpp prints
    "<LABEL>: no jobs completed" instead of a miss_ratio line -- seen in
    practice for LOW under single_core at low frequencies, where LOW's
    source blocks on backpressure forever behind HIGH's own overload and
    never finishes a single job) is scored as miss_ratio=1.0: zero
    completions under that overload is unambiguously "missed everything,"
    not a parse failure."""
    proc = subprocess.run(
        [bin_path, mode, str(low_freq_hz), str(jobs)],
        cwd=ROOT, check=True, capture_output=True, text=True,
    )
    low_miss = high_miss = None
    for line in proc.stderr.splitlines():
        m = LOW_LINE_RE.match(line)
        if m:
            low_miss = float(m.group(3))
        elif line.startswith("LOW") and "no jobs completed" in line:
            low_miss = 1.0
        m = HIGH_LINE_RE.match(line)
        if m:
            high_miss = float(m.group(3))
        elif line.startswith("HIGH") and "no jobs completed" in line:
            high_miss = 1.0
    if low_miss is None:
        raise RuntimeError(f"could not parse LOW's miss_ratio for mode={mode} freq={low_freq_hz}: {proc.stderr}")
    return low_miss, high_miss


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", nargs="?", default=None, help="also write the table text to this file")
    ap.add_argument("--freqs", default="50,60,70,80,90")
    ap.add_argument("--jobs", type=int, default=100)
    ap.add_argument("--repeats", type=int, default=10)
    ap.add_argument("--modes", default=",".join(ALL_MODES))
    ap.add_argument("--low-only", action="store_true",
                     help="omit the HIGH miss-ratio sub-column, one column per mode (just LOW) instead of two")
    ap.add_argument("--latex", action="store_true",
                     help="print a paper-style LaTeX table (LOW only) instead of the plain-text one")
    ap.add_argument("--caption", default=None, help="LaTeX table caption (--latex only); auto-generated if omitted")
    ap.add_argument("--label", default="tab:deadline", help="LaTeX \\label{} value (--latex only)")
    args = ap.parse_args()
    freqs = [float(f) for f in args.freqs.split(",")]
    modes = args.modes.split(",")
    for m in modes:
        if m not in ALL_MODES:
            ap.error(f"unknown mode {m!r} (expected one of {ALL_MODES})")

    bin_path = os.path.join(ROOT, "build", "interference_eval")
    os.makedirs(os.path.dirname(bin_path), exist_ok=True)
    build(bin_path)

    # {low,high}_miss[mode][freq_index] = mean miss ratio over --repeats runs.
    low_miss = {mode: [] for mode in modes}
    high_miss = {mode: [] for mode in modes}
    for low_freq_hz in freqs:
        for mode in modes:
            print(f"  {mode:11s} LOW={low_freq_hz:5.0f}Hz  ({args.repeats} repeats)", file=sys.stderr)
            low_reps, high_reps = [], []
            for rep in range(args.repeats):
                lm, hm = run_point(bin_path, mode, low_freq_hz, args.jobs)
                low_reps.append(lm)
                if hm is not None:
                    high_reps.append(hm)
                print(f"    rep {rep + 1}/{args.repeats}: LOW miss_ratio={lm:.2f}"
                      + (f"  HIGH miss_ratio={hm:.2f}" if hm is not None else "  HIGH miss_ratio=?"),
                      file=sys.stderr)
            mean_lm = statistics.mean(low_reps)
            mean_hm = statistics.mean(high_reps) if high_reps else math.nan
            low_miss[mode].append(mean_lm)
            high_miss[mode].append(mean_hm)
            print(f"    -> mean LOW miss_ratio {mean_lm:.2f}  mean HIGH miss_ratio {mean_hm:.2f}", file=sys.stderr)

    # Safety check (printed to stderr, same either way) -- HIGH missing its
    # own deadline invalidates whatever LOW's numbers claim to show.
    worst_high = max((v for mode in modes for v in high_miss[mode] if not math.isnan(v)), default=0.0)
    if worst_high > 0.0:
        print(f"\nNOTE: HIGH itself missed deadlines in at least one cell (worst miss_ratio={worst_high:.2f}) --",
              file=sys.stderr)
        print("that mode/frequency's LOW number is only meaningful once HIGH's own miss_ratio is ~0;",
              file=sys.stderr)
        print("see interference_topology.hpp's HIGH_PERIOD_US comment.", file=sys.stderr)

    if args.latex:
        caption = args.caption or ("Task deadline miss ratios for "
                                    + ", ".join(MODE_CAPTION_LABEL[m] for m in modes[:-1])
                                    + (" and " if len(modes) > 1 else "") + MODE_CAPTION_LABEL[modes[-1]] + ".")
        table_text = build_latex_table(freqs, modes, low_miss, caption, args.label)
    else:
        # Same shape/style as performance_test.cpp's own "Table II
        # (reproduced, avg of N runs)" printf block. Default: two
        # sub-columns per mode (HIGH, LOW), since this scenario only has
        # two priority classes and the comparison axis of interest is the
        # mode, not the priority class alone. --low-only drops back to one
        # column per mode (just LOW).
        lines = []
        lines.append(f"Table II-style (avg of {args.repeats} runs x {args.jobs} jobs): "
                     f"Deadline Miss Ratio{' (LOW only)' if args.low_only else ''}")
        if args.low_only:
            lines.append("      " + "".join(f"{MODE_LABEL[m]:>10s}" for m in modes))
            for i, freq in enumerate(freqs):
                row = f"{freq:4g}Hz" + "".join(f"{low_miss[m][i]:10.2f}" for m in modes)
                lines.append(row)
        else:
            lines.append("      " + "".join(f"{MODE_LABEL[m]:^14s}" for m in modes))
            lines.append("      " + "".join(f"{'HIGH':>7s}{'LOW':>7s}" for _ in modes))
            for i, freq in enumerate(freqs):
                row = f"{freq:4g}Hz" + "".join(f"{high_miss[m][i]:7.2f}{low_miss[m][i]:7.2f}" for m in modes)
                lines.append(row)
        table_text = "\n".join(lines)

    print("\n" + table_text)
    if args.out:
        with open(args.out, "w") as f:
            f.write(table_text + "\n")
        print(f"\nsaved: {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()

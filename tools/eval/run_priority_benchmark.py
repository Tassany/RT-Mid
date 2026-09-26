#!/usr/bin/env python3
"""Runs MCFlow's Section VI-C "Real-time Performance" experiment (Table
II/III: deadline miss ratio and average response time, High/Med fixed at
200/100 Hz, Low swept 50-90 Hz) against this project's own scheduler, and
prints a side-by-side comparison against MCFlow's own published numbers.

For each of the 5 deployment plans tools/eval/gen_priority_plans.py
generates: runs the codegen tool into its own generated_bench_<hz>hz/
directory (kept separate per frequency so runs don't clobber each other),
builds tools/eval/latency_eval_main.cpp against that generated pipeline,
runs it, and parses its CSV output.

Requires: the 5 plans/mcflow_priority_*hz.json files (run
gen_priority_plans.py first if missing), a C++17 compiler, and enough real
CPU cores (Table I assigns cores 0-3) — the workloads are real busy-spins,
so running this under CPU oversubscription (throttled VM/container) will
not reproduce meaningful numbers.

Usage: python3 tools/eval/run_priority_benchmark.py [jobs_per_task]
"""
import csv
import io
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CXX = "g++"
CXXFLAGS = ["-std=c++17", "-O2", "-Wall", "-Wextra", f"-I{ROOT}/src", f"-I{ROOT}/include"]

LOW_FREQS = [50, 60, 70, 80, 90]
TASK_NAME = {1: "High", 2: "Med", 3: "Low"}

# MCFlow's own published numbers (paper Table II/III), for comparison.
MCFLOW_MISS_RATIO = {
    50: {"High": 0.00, "Med": 0.00, "Low": 0.00},
    60: {"High": 0.00, "Med": 0.00, "Low": 0.00},
    70: {"High": 0.00, "Med": 0.00, "Low": 0.06},
    80: {"High": 0.00, "Med": 0.00, "Low": 0.75},
    90: {"High": 0.00, "Med": 0.00, "Low": 1.00},
}
MCFLOW_RESPONSE_US = {
    50: {"High": 3918, "Med": 8127, "Low": 14918},
    60: {"High": 3929, "Med": 8065, "Low": 12615},
    70: {"High": 3926, "Med": 8063, "Low": 12881},
    80: {"High": 3931, "Med": 8129, "Low": 13574},
    90: {"High": 3928, "Med": 8045, "Low": 18919},
}


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, cwd=ROOT, **kw)


def build_and_run_one(hz: int, jobs_per_task: int):
    plan = f"plans/mcflow_priority_{hz}hz.json"
    gendir = f"generated_bench_{hz}hz"
    os.makedirs(os.path.join(ROOT, gendir), exist_ok=True)

    codegen_bin = f"{gendir}/codegen_main"
    run([CXX, *CXXFLAGS, "tools/codegen/codegen_main.cpp", "src/dag.cpp",
         "src/parser_json.cpp", "-o", codegen_bin])
    run([f"./{codegen_bin}", plan, gendir])

    eval_bin = f"{gendir}/latency_eval"
    run([CXX, *CXXFLAGS, f"-I{gendir}", "tools/eval/latency_eval_main.cpp",
         "src/dag.cpp", "src/team_manager.cpp", "src/parser_json.cpp",
         f"{gendir}/pipeline_generated.cpp", "-o", eval_bin])

    proc = run([f"./{eval_bin}", plan, str(jobs_per_task)],
               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    print(proc.stderr, file=sys.stderr)

    rows = list(csv.DictReader(io.StringIO(proc.stdout)))
    per_task = {}
    for task_id, name in TASK_NAME.items():
        task_rows = [r for r in rows if int(r["task_id"]) == task_id]
        if not task_rows:
            continue
        n = len(task_rows)
        misses = sum(1 for r in task_rows if r["deadline_met"] == "0")
        avg_us = sum(int(r["response_ns"]) for r in task_rows) / n / 1000.0
        per_task[name] = {"miss_ratio": misses / n, "avg_us": avg_us, "n": n}
    return per_task


def main():
    jobs_per_task = int(sys.argv[1]) if len(sys.argv) > 1 else 200

    results = {}
    for hz in LOW_FREQS:
        print(f"--- running {hz} Hz ({jobs_per_task} jobs per task) ---", file=sys.stderr)
        results[hz] = build_and_run_one(hz, jobs_per_task)

    def row(hz, get):
        cells = []
        for name in ("High", "Med", "Low"):
            cells.append(get(hz, name))
        return cells

    print("\nTable II: Tasks Deadline Miss Ratios (this work / MCFlow)")
    print(f"{'Hz':>4}  {'High':>16}  {'Med':>16}  {'Low':>16}")
    for hz in LOW_FREQS:
        r = results[hz]
        def cell(hz, name, r=r):
            v = r.get(name, {}).get("miss_ratio")
            return f"{v:.2f}/{MCFLOW_MISS_RATIO[hz][name]:.2f}" if v is not None else "n/a"
        cells = row(hz, cell)
        print(f"{hz:>4}  {cells[0]:>16}  {cells[1]:>16}  {cells[2]:>16}")

    print("\nTable III: Average Response Times in us (this work / MCFlow)")
    print(f"{'Hz':>4}  {'High':>16}  {'Med':>16}  {'Low':>16}")
    for hz in LOW_FREQS:
        r = results[hz]
        def cell(hz, name, r=r):
            v = r.get(name, {}).get("avg_us")
            return f"{v:.0f}/{MCFLOW_RESPONSE_US[hz][name]}" if v is not None else "n/a"
        cells = row(hz, cell)
        print(f"{hz:>4}  {cells[0]:>16}  {cells[1]:>16}  {cells[2]:>16}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Generates the 5 deployment plans for MCFlow's Section VI-C "Real-time
Performance" experiment (Table I: CPU/workload assignment per subtask;
Table II/III: deadline miss ratio and response time as the Low-priority
task's rate varies while High and Medium stay fixed).

Topology per task (paper Fig. 8 / this project's own Fig. 1): Ts -> {T0,T1,
T2,T3} -> Tm. Three tasks share the same cores, distinguished only by
priority (fixed-priority, one priority per task, shared by all its
subtasks — paper Section 3's model). component_type is source/intermediate/
sink (cpp_class/header point at BenchSource/BenchIntermediate/BenchSink4 in
src/mcflow_bench_components.hpp); each subtask's real workload comes from
its own config.workload_us, taken straight from Table I.

Usage: python3 tools/eval/gen_priority_plans.py [out_dir]  (default: plans)
"""
import json
import os
import sys

# Table I: (core, workload_us) per subtask, per priority level.
TABLE_I = {
    "high": {
        "Ts": (0, 900), "T0": (0, 1800), "T1": (1, 1800),
        "T2": (2, 1800), "T3": (3, 1800), "Tm": (0, 900),
    },
    "med": {
        "Ts": (1, 900), "T0": (0, 0), "T1": (1, 1800),
        "T2": (2, 1800), "T3": (3, 1800), "Tm": (1, 1800),
    },
    "low": {
        "Ts": (2, 900), "T0": (0, 1800), "T1": (1, 900),
        "T2": (2, 4500), "T3": (3, 3600), "Tm": (2, 900),
    },
}

PRIORITY = {"high": 30, "med": 20, "low": 10}
FIXED_HZ = {"high": 200.0, "med": 100.0}  # low varies per file
LOW_FREQS_HZ = [50, 60, 70, 80, 90]

SUBTASK_ORDER = ["Ts", "T0", "T1", "T2", "T3", "Tm"]


def build_plan(low_hz: float) -> dict:
    freqs = dict(FIXED_HZ, low=low_hz)
    subtasks_by_task = {}
    id_counter = 1
    id_of = {}  # (level, name) -> id

    for level in ("high", "med", "low"):
        period_us = round(1e6 / freqs[level])
        entries = []
        for name in SUBTASK_ORDER:
            core, workload_us = TABLE_I[level][name]
            sid = id_counter
            id_counter += 1
            id_of[(level, name)] = sid
            if name == "Ts":
                component_type, cpp_class = "source", "BenchSource"
                fields = {"output_type": "double"}
            elif name == "Tm":
                component_type, cpp_class = "sink", "BenchSink4"
                fields = {"input_type": "std::array<double,4>"}
            else:
                component_type, cpp_class = "intermediate", "BenchIntermediate"
                fields = {"input_type": "double", "output_type": "double"}
            entry = {
                "id": sid,
                "component_type": component_type,
                "cpp_class": cpp_class,
                "header": "mcflow_bench_components.hpp",
                "priority": PRIORITY[level],
                "period_us": period_us,
                "deadline_us": period_us,  # implicit deadline (paper Section 3)
                "wcet_us": workload_us,
                "core": core,
                "config": {"workload_us": workload_us},
            }
            entry.update(fields)
            entries.append(entry)
        subtasks_by_task[level] = entries

    tasks = []
    connections = []
    for task_index, level in enumerate(("high", "med", "low"), start=1):
        tasks.append({"id": task_index, "subtasks": subtasks_by_task[level]})
        ts = id_of[(level, "Ts")]
        tm = id_of[(level, "Tm")]
        for mid in ("T0", "T1", "T2", "T3"):
            mid_id = id_of[(level, mid)]
            connections.append({"upstream": ts, "downstream": mid_id})
            connections.append({"upstream": mid_id, "downstream": tm})

    return {
        "hosts": [{"name": "localhost", "address": "127.0.0.1"}],
        "tasks": tasks,
        "connections": connections,
        "allocation": {"strategy": "worst_fit", "sort_by": "none"},
    }


def main() -> None:
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "plans"
    os.makedirs(out_dir, exist_ok=True)
    for hz in LOW_FREQS_HZ:
        plan = build_plan(float(hz))
        path = os.path.join(out_dir, f"mcflow_priority_{hz}hz.json")
        with open(path, "w") as f:
            json.dump(plan, f, indent=2)
        print(f"wrote {path}")


if __name__ == "__main__":
    main()

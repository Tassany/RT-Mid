# Testing & anomaly-detection strategy

Survey of how to evaluate RT-Mid's behavior and catch anomalies beyond
"it compiled and the existing 7 tests pass." Each item names the specific
anomaly it catches and the tool/technique, with an eye toward turning it
into a repeatable script rather than a one-off manual check. Cross-referenced
against [backlog/](backlog/) for current status per item.

## 1. Memory safety and concurrency (sanitizers)

`Dispatcher` / `CoreIdleController` / the ring buffer are the highest-risk
code (RULES.md §5 already flags this and it's still pending).

- **ThreadSanitizer** (`-fsanitize=thread`) on the existing test suite plus a
  repeated start/stop cycle of `TeamManager` — catches real data races
  between the main thread and the shared idle thread, without depending on
  a written argument catching everything.
- **AddressSanitizer + UBSanitizer** (`-fsanitize=address,undefined`) on the
  same binaries — catches ring buffer overflow, use-after-free on shutdown,
  UB in timing arithmetic.
- Becomes a `make test-sanitized` target: same tests, different class of
  anomaly surfaced.

## 2. Runtime scheduling invariants (assert-based)

Checks that don't exist today as asserts but should fail loudly:

- No subtask starts before its computed `next_release_ns`.
- Every subtask has `core >= 0` and within the configured core count after
  the allocator runs; no duplicate subtask ids.
- The DAG has no cycles and no orphan subtask (partially covered already by
  `dag_arbitrary_shapes_test.cpp` — promote from "one of the test files" to
  "breaks the build if it ever fails").
- The ring buffer never silently drops a write/read (ties into the known
  per-job sequencing limitation — turning the limitation into a test that
  exposes it).

## 3. Timing regression against the paper's own numbers

The empirical oracle already exists — it's missing an automatic guard:

- `tools/eval/run_priority_benchmark.py` already compares against MCFlow's
  published numbers. Add a **fence**: if RT-Mid's own deadline-miss ratio
  drifts from a saved baseline by more than a set margin (e.g. 5
  percentage points) at any frequency, the script exits non-zero — anomaly
  = performance regression, not just "did it run."
- Version the `latency_eval` CSVs as a baseline; compare the latest run
  against it with median + IQR (jitter in RT systems tends to have a long
  tail — a plain z-score is the wrong statistic here).

## 4. Allocator correctness (once WF+DRU exists)

An independent checker, not "ran without crashing":

- A script that reads a plan + the allocator's output and re-implements the
  paper's own rule (sort by descending remaining utilization of
  successors, place on the core with the most remaining capacity), then
  diffs the result — catches an allocator that "looks right" but diverges
  from the model (RULES.md §4).

## 5. Input robustness (parser)

- `JsonParser::parse()` handles external input (the deployment plan) and
  has only ever been tested against well-formed plans. A simple mutation
  fuzzer (malformed/truncated JSON, wrong types) — no need for
  libFuzzer/AFL — catches crashes/UB on hostile input in the Configuration
  layer.

## 6. Lifecycle and resources

- Repeated start/stop of `TeamManager` (hundreds of cycles) with thread
  count and open-fd count checked before/after (`/proc/<pid>/task`,
  `/proc/<pid>/fd`) — catches a pthread/eventfd/timerfd leak that only
  shows up after many runs, not the first one.
- Fault injection: `SIGTERM` at random points in the lifecycle, verifying
  the termination protocol (MCFlow Section V-A) always completes without
  hanging.

## 7. Real scheduling fidelity (OS-level observability)

- `perf sched` or reading `/proc/<pid>/task/<tid>/stat` during a real run,
  comparing each `SCHED_FIFO` thread's actual wake time against its
  expected release — detects priority inversion or excess context
  switching. Directly relevant to the earlier finding about per-core vs.
  per-(core,priority) idle threads, now partially addressed by
  `CoreIdleController`.

## Classification against RULES.md §9

The "elaborate work" gate (more than one ~150-line commit, a new
architectural piece, or touching more than one mapped layer) applies
unevenly across these seven:

| # | Approach | §9 elaborate? | Why |
|---|----------|----------------|-----|
| 1 | Sanitizers on `make test` | No, to add the target | Escalates if findings need a real multi-layer fix (then also §5) |
| 2 | Runtime scheduling invariants | **Yes** | Crosses DAG core, allocator, Dispatcher; concurrency-sensitive |
| 3 | Timing regression fence | No | Contained to `tools/eval/` |
| 4 | Allocator correctness checker | **Yes — and blocked** | New piece, tied to the not-yet-written core contribution (§4) |
| 5 | Parser fuzzing | No | Contained to the Configuration layer |
| 6 | Lifecycle / fault injection | **Yes** | Crosses `TeamManager`, `Dispatcher`, `CoreIdleController`; concurrency-sensitive |
| 7 | Scheduling fidelity via perf/ftrace | No | External analysis tooling, doesn't modify RT-Mid source |

Items 2 and 6 go through `grill-with-docs` → `spec-driven-development`
before implementation. Item 4 waits on the allocator itself. Items 1, 3, 5,
7 can be implemented directly, subject to RULES.md §3 (test per function
touched) and §5 where concurrency is involved.

# Single-Core vs WF+DRU vs ERU Response-Time Comparison — Progress

## 2026-09-30 — added a third mode: `eru`

Extended both files (small, additive change to already-implemented
tooling — not re-run through spec→plan→tasks, since it's the same
existing CLI/plot shape gaining one more value of an already-open enum,
not a new architectural piece):

- `tools/eval/allocator_comparison_eval_main.cpp`: accepts `eru` as a
  third `<mode>` value (`plan.allocation.strategy = "eru"`,
  `num_cores=4`, `sort_by`/`weight` left at their defaults since ERU
  doesn't consult them — see `specs/eru-allocator/spec.md`). Renamed
  `WF_DRU_NUM_CORES` → `ALLOCATED_NUM_CORES` (shared by both `wf_dru` and
  `eru`).
- `tools/eval/plot_allocator_comparison.py`: `MODES`/`MODE_LABEL` gained
  `eru`; title updated to mention all three.
- Ran the sweep (50–300 Hz, 100 jobs/point) in this sandbox (no
  `PREEMPT_RT`/`sudo`, same caveat as always). First attempt was noisy —
  a concurrent `make test` from another session was competing for CPU
  during the run. Re-ran once that finished; clean result: `wf_dru` and
  `eru` both stay flat around 3.7–4.0ms average response time across the
  whole frequency sweep (100/100 jobs completed at every point),
  `single_core` degrades sharply above ~100Hz exactly as the original
  2-mode chart already showed, falling further and further behind (job
  count drops as low as 28/100 at 300Hz — the driver's own "bailed 5
  periods behind" guard).
- Saved as `allocator_comparison_3way.png` — **not** overwriting
  `allocator_comparison.png`, which is owned by `root` in this sandbox
  (not writable by the working user); the author can `rm`/`chown` it and
  re-run `python3 tools/eval/plot_allocator_comparison.py` to reuse the
  original filename if wanted.
- `make test`: `dru_test`/`eru_test`/`test_flux` confirmed unaffected
  (built/ran individually — `tests/tdta_test.cpp` currently fails to
  *link* for an unrelated, in-progress reason: `specs/tdta-allocator/`'s
  `tdta.cpp` now calls `eru::detail::equilibrium_remaining_utilization_place`
  but `Makefile`'s `EXTRA_SRCS.tdta_test` doesn't yet include
  `$(ALLOCATOR_SRCS)` — that feature's own Phase 6/7 work, not touched
  here).

## 2026-09-30 — `--modes` flag; dropped `single_core` from the default comparison

Author's request: `single_core`'s values (tens to hundreds of ms) were
dwarfing `wf_dru`/`eru`'s (~3.7ms) on a shared y-axis, making the two real
allocators indistinguishable from each other in the 3-way chart. Rather
than a one-off script, added `--modes` to `plot_allocator_comparison.py`
(comma-separated, validated against the three known modes, defaults to
all three so existing usage is unchanged) so any subset can be plotted —
ran `--modes wf_dru,eru` for a two-line chart, saved separately as
`allocator_comparison_wfdru_eru.png` (kept `allocator_comparison_3way.png`
too, since both are useful for different purposes).

Result: with `single_core` out of the picture, `wf_dru` and `eru` both sit
flat around 3.7–3.8ms across the whole 50–300Hz sweep, `wf_dru` ticking up
to ~6.5ms at 300Hz while `eru` stays flat — a small, real difference that
was invisible on the 3-way chart's scale.

**Found and fixed in passing (blocking, not a one-off — affected every
binary in the repo, not just this tool):** `specs/tdta-allocator/` had
landed a third dispatcher branch in `src/allocator.cpp`
(`tdta::apply_tdta_allocation`, unconditionally referenced like the other
two), but `Makefile`'s `ALLOCATOR_SRCS` — the single list that's supposed
to prevent exactly this class of bug (see its own comment) — hadn't been
updated to include `src/tdta.cpp`/`src/ied.cpp`. This broke linking for
`dru_test`, `eru_test`, `test_flux`, `performance_test`, and this tool's
own `EXTRA_SRCS` (all of which use `$(ALLOCATOR_SRCS)` or its manually-kept-
in-sync Python equivalent). Fixed both: `Makefile`'s `ALLOCATOR_SRCS` now
lists all five `.cpp` files; `EXTRA_SRCS.tdta_test` simplified to just
`src/dag.cpp $(ALLOCATOR_SRCS)` (the explicit `ied.cpp`/`tdta.cpp` there
were now redundant); this script's own `EXTRA_SRCS` list gained
`ied.cpp`/`tdta.cpp` too. Verified: `dru_test`/`eru_test`/`test_flux` all
pass again.

**Still broken, not touched (unrelated, another session's in-progress
work):** `tests/tdta_test.cpp` itself crashes (uncaught
`std::runtime_error`, "needs 9.000000, best available core 0 has 1.000000
remaining") partway through its test list — a test bug in
`specs/tdta-allocator/`'s own suite (Phase 6/7 area), not something this
change touched or should fix.

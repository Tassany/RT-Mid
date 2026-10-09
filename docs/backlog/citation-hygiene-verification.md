# Verify unresolved paper citations

**Status:** 🟢 Ready — no dependency, just requires sitting down with both PDFs

## What needs to be done

Check these citations against the actual paper/MCFlow PDFs and fix or
remove whichever turn out wrong (RULES.md §6 — a wrong citation is worse
than none):

- `"paper Section IV"` / `"Section IV-B"` / `"Section IV-C"` — appears in
  `component.hpp`, `ring_buffer.hpp`, `adapter.hpp`, `component_registry.hpp`,
  `deployment_plan.hpp`, `team_manager.hpp`/`.cpp`. Not yet checked against
  either PDF this session.
- `"System Model, pg. 2"` in `parser_json.cpp` — doesn't name which paper.

## Why

Already confirmed (2026-09-25, against the real MCFlow PDF): Section
V-A/V-B/V-C and VI-C are MCFlow (Huang et al. 2012); Section 3 and 4.5 are
this paper's own. Section IV and its subsections are the remaining unknowns
— same category of problem that already turned up once (stale "Section
V-B/V-C" citations that were actually MCFlow, not this paper).

## Relevant files

Full citation inventory and confirmed/unverified split: the "Qual paper o
comentário está citando?" section of the
[Anatomia RT-Mid](https://claude.ai/artifact/Lqq2LfSZfy7g1iSE5fH3sb) artifact.

# Narrow ComponentRegistry Scope — Progress

updated: 2026-09-28
status: all phases complete, single commit ready for author review
blockers: none
next_session: none — author stages/commits per RULES.md §1

## Checkpoints

### CP: Phase 1 (DAG::Node dead field) — 2026-09-28
tests: 7/0/0 (`make clean && make test`, exit 0)
done: dag.hpp/dag.cpp field+signature removal, all 6 call sites
      (parser_json.cpp x2, codegen_main.cpp x2, dag_arbitrary_shapes_test
      x3, integration_pipeline_test x1)
criteria_met: DAG::Node has no ComponentBase*/component; add_node(id)
              only; all call sites compile
issues: none

### CP: Phase 2 (wire_component direct construction) — 2026-09-28
tests: 7/0/0
done: adapter.hpp wire_component rewritten (drops component_type param,
      constructs ConcreteType via config_type directly);
      codegen_main.cpp emission updated; adapter_test.cpp's 9 call sites
      + 6 RT_MID_REGISTER_COMPONENT lines removed
criteria_met: ComponentRegistry::create/has called only from
              parser_json.cpp; adapter_test.cpp touches no
              ComponentRegistry symbol
issues: wire_component's rewritten Doxygen comment initially came in at
        174 words, then 153 after a first trim — both over the RULES.md
        §10 120-word cap. Retrimmed to 118 words. Caught by word-counting
        each touched Doxygen block explicitly rather than assuming a
        rewrite was automatically compliant.

### CP: Phase 3 (component_registry.hpp comment) — 2026-09-28
tests: 7/0/0
done: file header comment rewritten to describe the closed
      source/intermediate/sink vocabulary and validate_components as the
      sole real caller
criteria_met: 114 words (RULES.md §10 cap: 120)
issues: none

### CP: Phase 4 (full verification) — 2026-09-28
tests: 7/0/0 (`make clean && make test`, exit 0)
build: pass (make clean && make test, includes codegen regeneration)
done: all tasks.md items
rework: none
criteria_met: all spec.md Acceptance Criteria — see below
issues: none

## Acceptance criteria cross-check (spec.md)

- [x] wire_component constructs ConcreteType directly, no
      ComponentRegistry::create()/static_cast
- [x] ComponentRegistry::create()/has() called only from
      JsonParser::validate_components
- [x] DAG::Node has no ComponentBase*/component; add_node(id) only
- [x] Every add_node call site updated, compiles
- [x] component_registry.hpp header comment rewritten, 114/120 words
- [x] adapter_test.cpp has zero RT_MID_REGISTER_COMPONENT calls
- [x] validate_components behavior unchanged (not touched)
- [x] All 7 test binaries build and pass
- [ ] Single commit — pending: author stages and commits (RULES.md §1,
      assistant never executes git write operations)

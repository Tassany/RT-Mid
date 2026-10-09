# Narrow ComponentRegistry Scope — Tasks

Behavior-preserving refactor: every touched function already has test
coverage (RULES.md §3 is satisfied by the existing tests, not new ones).
"Test" steps below are baseline-green-before / regression-green-after,
not new test authorship — per plan.md's Technical Approach.

Lands as **one commit** (author's explicit choice) — phases below are
execution order within that commit, not separate commits.

## Status Legend
- `[ ]` Not started · `[x]` Complete · `[~]` In progress · `[C]` Checkpoint

## Phase 1: DAG::Node dead field removal

- [x] Baseline: `make clean && make test` on unmodified tree — all 7
      binaries green (exit 0).
- [x] Implement: `src/dag.hpp` — removed `Node::component`
      (`ComponentBase*`), changed `add_node(int id, ComponentBase*)` to
      `add_node(int id)`, dropped `#include "component.hpp"`. Doxygen
      comments rewritten to match.
- [x] Implement: `src/dag.cpp` — updated `DAG::add_node` body.
- [x] Implement: updated every `add_node` call site —
      `src/parser_json.cpp` (2), `tools/codegen/codegen_main.cpp` (shape
      DAG build + generated-code emission, 2), `tests/dag_arbitrary_shapes_test.cpp`
      (3), `tests/integration_pipeline_test.cpp` (1).
- [C] Checkpoint: `grep -rn "add_node(" src tools tests` — all call sites
      1-argument, confirmed. `make clean && make test` — all 7 binaries
      green (exit 0).

## Phase 2: wire_component direct construction

- [x] Baseline: `adapter_test` green before changes (part of Phase 1's
      full-suite baseline).
- [x] Implement: `src/adapter.hpp` `wire_component` — dropped the
      `component_type` parameter; constructs `ConcreteType` directly via
      `typename ConcreteType::config_type`. Doxygen comment rewritten.
- [x] Implement: `tools/codegen/codegen_main.cpp` line ~324 — dropped the
      component_type string fragment from the emitted call.
- [x] Implement: `tests/adapter_test.cpp` — dropped the string argument
      from all 9 `wire_component<...>(...)` calls, deleted all 6
      `RT_MID_REGISTER_COMPONENT(...)` lines.
- [C] Checkpoint: `grep -n "ComponentRegistry\|RT_MID_REGISTER_COMPONENT"
      tests/adapter_test.cpp` — empty. `grep -rn "ComponentRegistry::" src
      tools` — only `src/parser_json.cpp`. `make clean && make test` —
      all 7 binaries green (exit 0), including `codegen_pipeline_test`
      (regenerates and compiles the generated pipeline from
      `plans/deployment_plan.json`).

## Phase 3: component_registry.hpp comment rewrite

- [x] Implement: `src/component_registry.hpp` lines 3-26 — rewrote the
      file header comment: closed role vocabulary
      (`source`/`intermediate`/`sink`), states the registry is used only
      by `validate_components`, example key changed to `"source"`.
- [C] Checkpoint: 114 words (≤120, RULES.md §10). `make clean && make
      test` — all 7 binaries green (exit 0).

## Phase 4: Full verification

- [x] Grep sweep: `grep -rn "ComponentRegistry::" src tools` — only
      `src/parser_json.cpp` (2 call sites).
- [x] `make clean && make test` — all seven test binaries build and pass
      (`deployment_plan_fields_test`, `component_registry_test`,
      `adapter_test`, `dag_arbitrary_shapes_test`,
      `integration_pipeline_test`, `codegen_pipeline_test`,
      `parser_json_test`), exit 0.
- [C] Final checkpoint: all listed binaries build and pass; all spec.md
      Acceptance Criteria checked off — see `progress.md`.

## Build Verification

- [x] `make clean && make test` exits 0.
- [C] Final checkpoint: all seven test binaries green, single commit
      ready for the author to stage and write (RULES.md §1 — assistant
      never executes `git add`/`commit`).

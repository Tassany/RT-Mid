# Narrow ComponentRegistry Scope

## Overview

`ComponentRegistry` (`src/component_registry.hpp`) was built as a generic
name-to-class plugin mechanism, but every real use in this codebase
resolves exactly three fixed role strings (`source`/`intermediate`/`sink`
— see `CONTEXT.md`). Two consumers use it where the concrete C++ type is
already known at compile time (`wire_component` in `src/adapter.hpp`, and
generated codegen wiring), making the registry lookup there pure
indirection. A separate field, `DAG::Node::component`, is written by
generated code but never read anywhere. This work removes that
comprovedly-unnecessary indirection and dead code so the author can defend
every remaining line of this AI-generated file for the paper, without
changing the Component layer's role in the architecture (kept per
[ADR-0001](../../docs/adr/0001-keep-component-layer-narrow-registry-scope.md)).

## User Stories

- As the author defending this codebase for the paper, I want
  `ComponentRegistry` used only where it's genuinely needed (resolving an
  unknown-at-compile-time string from JSON), so I can explain every call
  site without falling back on "the AI generated it this way."
- As the author, I want dead fields removed so a future reader (including
  a future me) doesn't waste time tracing a pointer that nothing reads.

## Acceptance Criteria

- [ ] `wire_component<ConcreteType>` (`src/adapter.hpp`) constructs
      `ConcreteType` directly via `typename ConcreteType::config_type`,
      with no call to `ComponentRegistry::create()` and no `static_cast`
      from `ComponentBase*`.
- [ ] `ComponentRegistry::create()`/`has()` are called only from
      `JsonParser::validate_components` (`src/parser_json.cpp`) — no other
      call site in `src/` or `tools/`.
- [ ] `DAG::Node` has no `ComponentBase*`/`component` member;
      `DAG::add_node` takes only `id`.
- [ ] Every call site of `add_node` (`src/parser_json.cpp`,
      `tools/codegen/codegen_main.cpp`, and the generated-code template
      inside it, `tests/dag_arbitrary_shapes_test.cpp`,
      `tests/integration_pipeline_test.cpp`) updated to the new signature
      and compiles.
- [ ] `src/component_registry.hpp`'s top comment (currently lines 3-26)
      rewritten to describe the actual closed role vocabulary
      (`source`/`intermediate`/`sink`), not a speculative arbitrary-name
      example; stays within RULES.md §10's 120-word cap.
- [ ] `tests/adapter_test.cpp` has zero `RT_MID_REGISTER_COMPONENT` calls
      (they test the wiring layer only, per the file's own scope comment).
- [ ] `JsonParser::validate_components`'s behavior is unchanged — still
      constructs a real component via the registry and cross-checks its
      `kind()` against the DAG-topology-derived expected role.
- [ ] Every existing test binary still builds (per its own header
      comment's `g++` line) and passes:
      `deployment_plan_fields_test`, `component_registry_test`,
      `adapter_test`, `dag_arbitrary_shapes_test`,
      `integration_pipeline_test`, `codegen_pipeline_test`,
      `parser_json_test`.
- [ ] All of the above lands in a single commit (author's explicit scope
      choice, overriding the initial 3-commit recommendation from the
      grill).

## Non-Goals

- Removing or restructuring the Component layer itself
  (`component.hpp`/`ComponentBase`/`Component<I,O,C>` and subclasses) —
  settled as "keep" in ADR-0001.
- Any change to `JsonParser::validate_components`'s logic — the
  kind()-vs-topology cross-check is deliberate defense-in-depth, not
  redundant, per the grill.
- Touching `tests/deployment_plan_fields_test.cpp` or
  `tests/component_registry_test.cpp` — both exercise the registry
  through the real parser path and are unaffected by this change.
- Introducing a build system (CMake or otherwise) — out of scope, tests
  keep their per-file `g++` build comments.
- Any paper text edit — moot since the Component layer isn't changing.

## Dependencies

- [ADR-0001](../../docs/adr/0001-keep-component-layer-narrow-registry-scope.md)
  and [CONTEXT.md](../../CONTEXT.md) — the decisions this spec executes.
- [docs/backlog/reconcile-component-layer.md](../../docs/backlog/reconcile-component-layer.md)
  — closed by ADR-0001; this spec is unrelated follow-through, not what
  closes that item.

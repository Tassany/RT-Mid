# Narrow ComponentRegistry Scope — Technical Plan

## Technical Approach

Two independent, behavior-preserving simplifications, both scoped by the
grill (see spec.md Dependencies):

1. **`wire_component` stops going through `ComponentRegistry`.** It
   already receives `ConcreteType` as an explicit template argument
   (codegen knows the concrete class at generation time — `s.cpp_class`).
   Every `Component`/`SourceComponent`/`SinkComponent` subclass already
   exposes `config_type`. So `wire_component` can build the
   `ComponentInstance` directly — `make_shared<ConfigType>(config.get<...>())`
   then `make_unique<ConcreteType>(cfg.get())` — instead of routing through
   `ComponentRegistry::create()` + `static_cast`. This leaves
   `ComponentRegistry` used only by `JsonParser::validate_components`,
   which is the one call site that genuinely receives a `component_type`
   string without knowing the C++ type at compile time.

2. **`DAG::Node` drops its unused `ComponentBase* component` field.**
   Confirmed by full-codebase search: written only by codegen-generated
   code, read nowhere. Removing it also lets `dag.hpp` drop its
   `#include "component.hpp"` — the graph/topology layer no longer needs
   to know the Component layer exists at all, which it never should have
   for `add_node`'s purposes.

Both changes are internal to already-tested call paths (`wire_component`'s
behavior is fully exercised by `tests/adapter_test.cpp`; `add_node`'s by
`tests/dag_arbitrary_shapes_test.cpp` and others) — existing tests are the
regression net, not new tests. RULES.md §3 requires a test for non-trivial
touched functions; both functions already have one, so the task is "keep
these green," not "write new coverage."

## Key Decisions

| Decision | Choice | Rationale | Needs sign-off? |
|---|---|---|---|
| Where `ComponentRegistry::create()`/`has()` remain callable | Only `JsonParser::validate_components` | That's the sole genuine unknown-type-at-compile-time use; matches ADR-0001 | No — settled in grill |
| `DAG::Node::component` field | Removed, `add_node(id)` | Confirmed dead by search | No — settled in grill |
| `validate_components` logic | Unchanged | Deliberate defense-in-depth (registration-mismatch + config-parse smoke test), not redundant | No — settled in grill |
| `wire_component`'s `component_type` string parameter | Dropped from the signature | It becomes dead the moment `wire_component` stops passing it to the registry; keeping it would be the same category of thing this change removes. Touches `tools/codegen/codegen_main.cpp:324` (emitted string) and every direct caller (`tests/adapter_test.cpp`). | No — confirmed by author |

## Interface Contracts

`src/adapter.hpp`:
```cpp
// before
template<typename ConcreteType, typename UpstreamReader, typename... DownstreamWriters>
WiredNode wire_component(int id, const std::string& component_type,
                          const nlohmann::json& config,
                          UpstreamReader upstream, DownstreamWriters... downstream) {
    WiredNode node;
    node.instance = ComponentRegistry::instance().create(component_type, config);
    auto* concrete = static_cast<ConcreteType*>(node.instance.component.get());
    ...
}

// after
template<typename ConcreteType, typename UpstreamReader, typename... DownstreamWriters>
WiredNode wire_component(int id, const nlohmann::json& config,
                          UpstreamReader upstream, DownstreamWriters... downstream) {
    using ConfigType = typename ConcreteType::config_type;
    auto cfg = std::make_shared<ConfigType>(config.get<ConfigType>());
    auto component = std::make_unique<ConcreteType>(cfg.get());
    auto* concrete = component.get();

    WiredNode node;
    node.instance.config    = cfg;
    node.instance.component = std::move(component);
    ...
}
```
`component_type` is dropped from the parameter list. Every call site's
argument list loses that string argument too:
`tools/codegen/codegen_main.cpp:324` (the emitted call — drop the
`"\"" << s.component_type << "\", "` fragment) and every
`wire_component<...>(id, "...", ...)` call in `tests/adapter_test.cpp`.

`src/dag.hpp` / `src/dag.cpp`:
```cpp
// before
struct Node { int id; std::vector<int> predecessors, successors; ComponentBase* component; };
void add_node(int id, ComponentBase* component);

// after
struct Node { int id; std::vector<int> predecessors, successors; };
void add_node(int id);
```
Drops `#include "component.hpp"` from `dag.hpp`.

Call sites touched by the `add_node` signature change:
`src/parser_json.cpp` (x2), `tools/codegen/codegen_main.cpp` (x2: the
shape-only DAG build at line ~150, and the generated-code emission at
line ~328), `tests/dag_arbitrary_shapes_test.cpp` (x3),
`tests/integration_pipeline_test.cpp` (x1).

## Data Model

No data model changes — `ComponentInstance`, `WiredNode`, `SubtaskInfo`
are untouched. This is a call-graph/dead-field simplification, not a
schema change.

## Implementation Phases

1. **`DAG::Node` field removal** (`src/dag.hpp`, `src/dag.cpp`, all
   `add_node` call sites) — independent of the `wire_component` change,
   safe to do first, smaller blast radius.
2. **`wire_component` direct construction** (`src/adapter.hpp`) + its
   downstream consequences (`tools/codegen/codegen_main.cpp`'s emitted
   call, `tests/adapter_test.cpp`'s now-orphaned `RT_MID_REGISTER_COMPONENT`
   calls).
3. **`component_registry.hpp` comment rewrite** — trivial, no behavior
   change, bundled here since it's a one-file doc-only edit.
4. **Full verification** — build and run every affected test binary.

Landed as a single commit per the author's explicit choice (spec.md
Acceptance Criteria), but implemented/verified in this order internally.

## Risks and Mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| A call site of `add_node` or `wire_component` is missed, breaking the build | Build failure, easy to catch | Grep for both symbols across the whole tree as a final check before the checkpoint, not just the files listed above |
| `config.get<ConfigType>()` throws for a config JSON that was previously never actually parsed at this call site under the old registry path (unlikely — the registry factory did the same `j.get<ConfigType>()` call) | Same as before — no new failure mode | Behavior is identical to what `ComponentRegistry`'s factory lambda already did; not a new risk, just relocated code |
| Dropping `wire_component`'s `component_type` parameter (if approved) ripples into codegen's generated-code string template, easy to get an off-by-one in the emitted comma/quote | Generated `.cpp` fails to compile | Rebuild and run `codegen_pipeline_test.cpp`, which compiles the actually-generated file, not just codegen itself |

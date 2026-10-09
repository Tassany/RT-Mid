# Keep the Component layer; narrow ComponentRegistry to parse-time role resolution

Status: accepted. Supersedes an informal decision recorded 2026-09-25 to
drop the Component layer entirely, which was never carried out —
`component.hpp`/`component_registry.hpp` stayed active and wired into
`dag.hpp`, a contradiction tracked in
[docs/backlog/reconcile-component-layer.md](../backlog/reconcile-component-layer.md).

**Decision:** keep the Component layer. It's one of the four layers the
MCFlow-inspired paper architecture describes, and paper fidelity is the
reason to keep it, not just inertia.

**Decision:** `ComponentRegistry` is scoped to what it's actually used
for — resolving a `component_type` string to a component instance at
deployment-plan parse time (`JsonParser::validate_components`), the one
call site that genuinely doesn't know the concrete C++ type at compile
time. Runtime wiring (`wire_component`, driven by codegen) constructs the
concrete type directly instead, since codegen already knows it at compile
time; going through the registry there was redundant indirection, not a
second legitimate use.

## Consequences

- `DAG::Node`'s `ComponentBase*` is removed: it was written by generated
  code but never read anywhere, confirmed by search across the codebase.
- `RT_MID_REGISTER_COMPONENT` is still required for every `component_type`
  an application uses, but is no longer required just to call
  `wire_component` directly (e.g. in tests that exercise the wiring layer
  without going through the parser).

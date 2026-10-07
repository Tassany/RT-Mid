# Reconcile the Component layer decision

**Status:** ⚪ Done — resolved as "Keep it". See
[ADR-0001](../adr/0001-keep-component-layer-narrow-registry-scope.md).

## What needs to be done

Project memory recorded an earlier decision to drop the Component layer
entirely (judged not to make sense as implemented, with a noted consequence
for paper Section 4.1). The current code contradicts that: `component.hpp`
and `component_registry.hpp` are active and used by `dag.hpp`. Pick one,
explicitly, and act on it:

- **Keep it** — update the earlier decision on record, and make sure paper
  Section 4 still describes it accurately.
- **Actually remove it** — this time follow through in code, which means
  reworking `dag.hpp`'s `Node` and anything depending on `ComponentBase*`.

## Why

Architectural decisions in code are supposed to be tracked against the
paper as they happen (RULES.md §7), not discovered as a contradiction
later — which is exactly what happened here.

## Relevant files

`src/component.hpp`, `src/component_registry.hpp`, `src/dag.hpp`. See the
"Componente" section of the [Anatomia RT-Mid](https://claude.ai/artifact/Lqq2LfSZfy7g1iSE5fH3sb)
artifact for where this sits in the current architecture.

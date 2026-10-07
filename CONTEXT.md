# RT-Mid

DAG scheduling middleware for real-time subtask pipelines, with automatic
subtask-to-core allocation as the paper's main contribution.

## Language

**component_type**:
The string field on a deployment-plan subtask naming its role in the
pipeline. Always exactly one of `"source"`, `"intermediate"`, or `"sink"` —
it selects which registered factory builds the subtask's component, never
an application-specific device or sensor class.
_Avoid_: component class, component kind (that's `ComponentKind`, the enum
`ComponentBase::kind()` returns), sensor type.

**ComponentRegistry**:
Resolves a `component_type` string to a real `ComponentBase` instance at
deployment-plan parse time — the one point where the concrete C++ type
genuinely isn't known until the JSON is read. Not a general plugin
mechanism for arbitrary application-defined classes; every real
registration uses the closed `component_type` vocabulary above.
_Avoid_: component factory, plugin registry.

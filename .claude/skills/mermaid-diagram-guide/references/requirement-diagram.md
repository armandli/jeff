# Requirement Diagram

**Keyword**: `requirementDiagram`
**Use for**: System requirements (SysML), traceability, requirement-to-element relationships

## Requirement Types

```
requirement req_name {
    id: 1.1
    text: The system shall do X
    risk: high
    verifymethod: test
}
```

**Requirement type keywords:**
- `requirement` — generic requirement
- `functionalRequirement` — functional behavior
- `interfaceRequirement` — interface specification
- `performanceRequirement` — performance constraints
- `physicalRequirement` — physical/hardware constraints
- `designConstraint` — design limitations

## Required Fields

| Field | Values |
|-------|--------|
| `id` | Any string/number |
| `text` | Requirement statement |
| `risk` | `low`, `medium`, `high` |
| `verifymethod` | `analysis`, `inspection`, `test`, `demonstration` |

## Element Definition

Elements represent system components that satisfy requirements:

```
element elem_name {
    type: hardware
    docref: design_spec_v1
}
```

Fields:
- `type` — user-defined string (e.g., `software`, `hardware`, `test_suite`)
- `docref` — reference to external document

## Relationship Types

```
req_name - relationship_type -> elem_name
```

| Relationship | Meaning |
|-------------|---------|
| `contains` | Requirement contains sub-requirement |
| `copies` | Copied from another requirement |
| `derives` | Derived from another requirement |
| `satisfies` | Element satisfies requirement |
| `verifies` | Element verifies requirement |
| `refines` | Refines another requirement/element |
| `traces` | Traces to another requirement/element |

## Examples

**Functional requirement with verification:**
```
requirementDiagram
    functionalRequirement user_auth {
        id: REQ-001
        text: The system shall authenticate users with MFA
        risk: high
        verifymethod: test
    }

    functionalRequirement session_timeout {
        id: REQ-002
        text: Sessions shall expire after 30 minutes of inactivity
        risk: medium
        verifymethod: inspection
    }

    element auth_module {
        type: software
        docref: auth_design_v2
    }

    element test_suite {
        type: test
        docref: auth_test_plan
    }

    user_auth - satisfies -> auth_module
    user_auth - verifies -> test_suite
    user_auth - contains -> session_timeout
```

**Performance requirement:**
```
requirementDiagram
    performanceRequirement api_latency {
        id: PERF-001
        text: API shall respond within 200ms at p99
        risk: high
        verifymethod: test
    }

    designConstraint rate_limit {
        id: CONST-001
        text: API shall not exceed 1000 req/sec per client
        risk: low
        verifymethod: analysis
    }

    element api_gateway {
        type: software
        docref: api_spec_v3
    }

    api_latency - satisfies -> api_gateway
    rate_limit - satisfies -> api_gateway
    api_latency - derives -> rate_limit
```

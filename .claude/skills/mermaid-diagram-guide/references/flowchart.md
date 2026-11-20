# Flowchart

**Keyword**: `flowchart`
**Use for**: Processes, workflows, decision trees, data pipelines

## Direction

```
flowchart TB   # top-to-bottom (default)
flowchart TD   # same as TB
flowchart BT   # bottom-to-top
flowchart LR   # left-to-right
flowchart RL   # right-to-left
```

## Node Shapes

| Syntax | Shape |
|--------|-------|
| `A[Text]` | Rectangle |
| `A(Text)` | Rounded rectangle |
| `A([Text])` | Stadium/pill |
| `A[[Text]]` | Subroutine |
| `A[(Text)]` | Cylinder (database) |
| `A((Text))` | Circle |
| `A{Text}` | Diamond (decision) |
| `A{{Text}}` | Hexagon |
| `A[/Text/]` | Parallelogram |
| `A[\Text\]` | Parallelogram alt |
| `A[/Text\]` | Trapezoid |
| `A[\Text/]` | Trapezoid alt |

New shape syntax (v11+): `A@{ shape: rect, label: "Text" }`

## Edge Types

| Syntax | Appearance |
|--------|------------|
| `A --> B` | Arrow |
| `A --- B` | Open line |
| `A -.-> B` | Dotted arrow |
| `A ==> B` | Thick arrow |
| `A --o B` | Circle end |
| `A --x B` | Cross end |
| `A <--> B` | Bidirectional |
| `A -->|label| B` | Arrow with label |
| `A -- label --> B` | Arrow with label (alt) |

## Subgraphs

```
flowchart LR
    subgraph id [Title]
        direction TB
        A --> B
    end
    id --> C
```

## Comments

```
%% This is a comment
```

## Styling

```
flowchart LR
    A --> B
    style A fill:#f9f,stroke:#333,stroke-width:2px
    classDef myClass fill:#bbf,stroke:#00f
    class B myClass
```

## Examples

**Decision flow:**
```
flowchart TD
    A[Request] --> B{Valid?}
    B -->|Yes| C[Process]
    B -->|No| D[Return Error]
    C --> E[Respond]
```

**With subgraph:**
```
flowchart LR
    subgraph frontend[Frontend]
        UI[React App]
    end
    subgraph backend[Backend]
        API[REST API]
        DB[(Database)]
    end
    UI -->|HTTP| API
    API --> DB
```

**Pipeline:**
```
flowchart LR
    A([Source]) --> B[/Transform/]
    B --> C[(Store)]
    C --> D[Report]
```

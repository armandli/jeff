# State Diagram

**Keyword**: `stateDiagram-v2` (use v2 — v1 is legacy)
**Use for**: State machines, entity lifecycle, UI state, protocol state transitions

## Basic Syntax

```
stateDiagram-v2
    [*] --> StateA
    StateA --> StateB : event
    StateB --> [*]
```

- `[*]` is the initial/final pseudo-state — transition direction determines which it is
- `: label` adds a transition label (event/condition)

## State Definition

```
stateDiagram-v2
    state "Long Description" as s1
    s1 --> s2
```

Or inline description:
```
s1: This is the description
```

## Composite States (Nested)

```
stateDiagram-v2
    [*] --> Active
    state Active {
        [*] --> Idle
        Idle --> Processing : start
        Processing --> Idle : done
    }
    Active --> [*] : shutdown
```

## Concurrent States (Fork / Join)

```
stateDiagram-v2
    [*] --> Running
    state Running {
        [*] --> Thread1
        [*] --> Thread2
        --
        [*] --> Monitor
    }
```

Use `--` separator to define concurrent regions within a composite state.

## Fork and Join

```
stateDiagram-v2
    state fork <<fork>>
    state join <<join>>

    [*] --> fork
    fork --> BranchA
    fork --> BranchB
    BranchA --> join
    BranchB --> join
    join --> [*]
```

## Choice

```
stateDiagram-v2
    state decide <<choice>>
    [*] --> decide
    decide --> Yes : if condition
    decide --> No : else
```

## Notes

```
stateDiagram-v2
    StateA --> StateB
    note right of StateA
        This is a note
    end note
```

## Direction

```
stateDiagram-v2
    direction LR
    A --> B
```

Options: `TB` (default), `LR`, `RL`, `BT`

## Styling

```
stateDiagram-v2
    classDef error fill:#f00,color:white
    class ErrorState error
```

## Examples

**Order lifecycle:**
```
stateDiagram-v2
    [*] --> Draft
    Draft --> Submitted : submit()
    Submitted --> Approved : approve()
    Submitted --> Rejected : reject()
    Approved --> Shipped : ship()
    Shipped --> Delivered : deliver()
    Rejected --> [*]
    Delivered --> [*]

    note right of Approved
        Triggers warehouse
        notification
    end note
```

**TCP connection states:**
```
stateDiagram-v2
    direction LR
    [*] --> CLOSED
    CLOSED --> LISTEN : passive open
    LISTEN --> SYN_RCVD : SYN
    SYN_RCVD --> ESTABLISHED : ACK
    CLOSED --> SYN_SENT : active open
    SYN_SENT --> ESTABLISHED : SYN+ACK
    ESTABLISHED --> FIN_WAIT : close
    FIN_WAIT --> [*] : FIN+ACK
```

**UI state with nested:**
```
stateDiagram-v2
    [*] --> Idle
    state Idle {
        [*] --> Empty
        Empty --> HasInput : type
        HasInput --> Empty : clear
    }
    Idle --> Loading : submit
    Loading --> Success : response
    Loading --> Error : failure
    Error --> Idle : retry
    Success --> [*]
```

# Sequence Diagram

**Keyword**: `sequenceDiagram`
**Use for**: Inter-service communication, API call flows, protocol interactions, actor interactions over time

## Participants

```
sequenceDiagram
    participant A as Alice
    participant B as Bob
    actor U as User        %% renders as stick figure
```

Participants are auto-created on first use if not declared. Declaration order controls column order.

## Message Types

| Syntax | Description |
|--------|-------------|
| `A->>B: msg` | Solid line with arrowhead |
| `A-->>B: msg` | Dotted line with arrowhead |
| `A->B: msg` | Solid line, no arrowhead |
| `A-->B: msg` | Dotted line, no arrowhead |
| `A-xB: msg` | Solid line with X (async) |
| `A--xB: msg` | Dotted line with X |
| `A-)B: msg` | Open arrow (async) |
| `A--)B: msg` | Dotted open arrow |

## Activations

Show when an actor is active/processing:
```
A->>+B: request
B-->>-A: response
```

Or explicitly:
```
activate B
deactivate B
```

## Control Flow Blocks

**Loop:**
```
loop Every minute
    A->>B: Heartbeat
    B-->>A: OK
end
```

**Alt/else (conditional):**
```
alt Condition
    A->>B: path 1
else Other condition
    A->>B: path 2
else
    A->>B: default
end
```

**Optional:**
```
opt Optional action
    A->>B: maybe
end
```

**Parallel:**
```
par Action 1
    A->>B: msg1
and Action 2
    A->>C: msg2
end
```

**Critical region:**
```
critical Acquire lock
    A->>B: update
option Timeout
    A->>A: retry
end
```

**Break:**
```
break on exception
    A->>B: error handler
end
```

## Notes

```
Note right of A: note text
Note left of B: note text
Note over A,B: note spanning both
```

## Actor Grouping (boxes)

```
box Blue Group1
    participant A
    participant B
end
```

## Numbering

```
sequenceDiagram
    autonumber
    A->>B: first
    B->>A: second
```

## Comments

```
%% This is a comment
```

## Examples

**HTTP request/response:**
```
sequenceDiagram
    participant C as Client
    participant S as Server
    participant DB as Database

    C->>+S: GET /users/42
    S->>+DB: SELECT * FROM users WHERE id=42
    DB-->>-S: user record
    S-->>-C: 200 OK {user}
```

**Auth flow with alt:**
```
sequenceDiagram
    actor U as User
    participant App
    participant Auth

    U->>App: login(username, password)
    App->>+Auth: validate credentials
    alt Valid
        Auth-->>-App: token
        App-->>U: 200 OK
    else Invalid
        Auth-->>-App: 401
        App-->>U: login failed
    end
```

**Async event processing:**
```
sequenceDiagram
    participant Producer
    participant Queue
    participant Consumer

    Producer-)Queue: publish(event)
    Note over Queue: async delivery
    Queue-)Consumer: deliver(event)
    Consumer->>Queue: ack
```

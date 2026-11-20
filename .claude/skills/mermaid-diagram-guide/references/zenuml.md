# ZenUML

**Keyword**: `zenuml`
**Use for**: Sequence diagrams with a code-like syntax — good for developers comfortable with procedural/OOP notation

## Key Differences from sequenceDiagram

ZenUML uses a different, more code-like syntax. Participants are inferred from method call notation (`A.method()`), and nesting is expressed with `{}` blocks rather than `end` keywords.

## Participants

Participants are auto-created from usage. Declare explicitly to control order:
```
zenuml
    @Actor Alice
    @Database DB
    Alice -> DB: query
```

Annotations: `@Actor`, `@Boundary`, `@Control`, `@Entity`, `@Database`, `@Collections`

## Message Types

```
// Sync call (blocking)
A -> B: doSomething()

// Async message
A -> B: fireAndForget()

// Object creation
new B("args")

// Self-call
A -> A: internalMethod()
```

## Return Values

Three forms:
```
return value
@return value
A --> B: value     // explicit reply arrow
```

## Nesting Calls

Use `{}` to express call hierarchies:
```
zenuml
    A -> B.processOrder() {
        B -> C.validatePayment() {
            C --> B: valid
        }
        B -> D.updateInventory()
        B --> A: orderConfirmed
    }
```

## Control Flow

**Loop:**
```
while (hasItems) {
    A -> B: processItem()
}

for (item in list) {
    A -> B: handle(item)
}
```

**Conditional:**
```
if (condition) {
    A -> B: path1()
} else if (other) {
    A -> B: path2()
} else {
    A -> B: default()
}
```

**Optional:**
```
opt (optional) {
    A -> B: maybeCall()
}
```

**Parallel:**
```
par {
    A -> B: task1()
    C -> D: task2()
}
```

**Error handling:**
```
try {
    A -> B: riskyOperation()
} catch {
    A -> ErrorHandler: handleError()
} finally {
    A -> Cleanup: release()
}
```

## Examples

**HTTP request flow:**
```
zenuml
    @Actor Client
    @Boundary API
    @Entity DB

    Client -> API.getUser(id) {
        API -> DB.findById(id) {
            DB --> API: user
        }
        if (user == null) {
            API --> Client: 404
        } else {
            API --> Client: 200 user
        }
    }
```

**Async processing:**
```
zenuml
    @Actor User
    Producer -> Queue: publish(event)
    Consumer -> Queue: subscribe()
    while (running) {
        Queue -> Consumer: deliver(event)
        Consumer -> Handler: process(event)
        Consumer -> Queue: ack()
    }
```

**Error handling flow:**
```
zenuml
    Client -> Service.call() {
        try {
            Service -> DB.query() {
                DB --> Service: result
            }
            Service --> Client: success(result)
        } catch {
            Service -> Logger: logError()
            Service --> Client: error(500)
        }
    }
```

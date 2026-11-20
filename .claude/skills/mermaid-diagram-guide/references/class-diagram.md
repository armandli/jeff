# Class Diagram

**Keyword**: `classDiagram`
**Use for**: OOP design, data models, domain models, interface contracts

## Class Definition

```
classDiagram
    class Animal {
        +String name
        +int age
        +eat() void
        +sleep() bool
    }
```

## Member Visibility

| Prefix | Visibility |
|--------|------------|
| `+` | Public |
| `-` | Private |
| `#` | Protected |
| `~` | Package/Internal |

## Member Classifiers (suffixes)

| Suffix | Meaning |
|--------|---------|
| `*` | Abstract |
| `$` | Static |

Examples: `+method()* void`, `+count$ int`

## Colon syntax (alternative)

```
class Dog
Dog : +String name
Dog : +bark() void
```

## Relationship Types

| Syntax | Relationship | Description |
|--------|-------------|-------------|
| `A <\|-- B` | Inheritance | B extends A |
| `A *-- B` | Composition | B is part of A (owns lifecycle) |
| `A o-- B` | Aggregation | B belongs to A (independent lifecycle) |
| `A --> B` | Association | A uses B |
| `A -- B` | Link (solid) | General association |
| `A ..> B` | Dependency | A depends on B |
| `A ..\|> B` | Realization | B implements A |
| `A .. B` | Link (dashed) | General dashed |

Reverse direction: flip the arrow (e.g., `B \|>-- A` instead of `A <\|-- B`)

## Multiplicity / Cardinality

```
Customer "1" --> "0..*" Order : places
```

Common cardinality: `1`, `0..*`, `1..*`, `0..1`, `n`, `*`

## Namespace / Package

```
classDiagram
    namespace Shapes {
        class Circle {
            +float radius
        }
        class Rectangle {
            +float width
            +float height
        }
    }
```

## Notes

```
note "This is a note"
note for Animal "This note applies to Animal"
```

## Generic Types

```
class Container~T~ {
    +T value
    +get() T
}
```

## Examples

**Inheritance hierarchy:**
```
classDiagram
    Animal <|-- Dog
    Animal <|-- Cat
    class Animal {
        +String name
        +eat() void
        +speak()* void
    }
    class Dog {
        +speak() void
    }
    class Cat {
        +speak() void
    }
```

**Domain model with relationships:**
```
classDiagram
    class Order {
        +int id
        +Date createdAt
        +submit() void
    }
    class OrderItem {
        +int quantity
        +float unitPrice
    }
    class Product {
        +String name
        +float price
    }
    Order "1" *-- "1..*" OrderItem : contains
    OrderItem --> "1" Product : references
```

**Interface + implementation:**
```
classDiagram
    class Repository~T~ {
        <<interface>>
        +findById(id) T
        +save(entity T) void
        +delete(id) void
    }
    class UserRepository {
        +findById(id) User
        +save(entity User) void
        +delete(id) void
    }
    Repository <|.. UserRepository
```

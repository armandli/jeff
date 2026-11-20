# Entity Relationship Diagram

**Keyword**: `erDiagram`
**Use for**: Database schema design, data modeling, relational data structures

## Relationship Syntax

```
ENTITY1 CARDINALITY--CARDINALITY ENTITY2 : "label"
```

## Cardinality (Crow's Foot Notation)

| Left side | Right side | Meaning |
|-----------|-----------|---------|
| `\|o` | `o\|` | Zero or one |
| `\|\|` | `\|\|` | Exactly one |
| `}o` | `o{` | Zero or more |
| `}\|` | `\|{` | One or more |

**Relationship types:**
- `--` identifying (solid line) — child cannot exist without parent
- `..` non-identifying (dashed line) — independent entities

**Aliases**: `one or more`, `zero or more`, `zero or one`, `only one`, `1+`, `0+`, `1`, `*`

## Common Patterns

```
A ||--o{ B : "has"        %% one A has zero or more B
A ||--|{ B : "contains"   %% one A has one or more B
A }o--o{ B : "links"      %% many-to-many optional
A ||--|| B : "is"         %% one-to-one
```

## Attribute Definition

```
ENTITY {
    type attributeName key
    string name
    int id PK
    int foreignId FK
    string email UK
    date createdAt "comment"
}
```

Key types: `PK` (Primary Key), `FK` (Foreign Key), `UK` (Unique Key)

## Entity Names

- Simple: `CUSTOMER`, `ORDER`
- With spaces (quoted): `"order item"`, `"user profile"`

## Comments

```
%% This is a comment
```

## Examples

**Basic e-commerce schema:**
```
erDiagram
    CUSTOMER {
        int id PK
        string name
        string email UK
    }
    ORDER {
        int id PK
        int customerId FK
        date placedAt
        string status
    }
    ORDER_ITEM {
        int id PK
        int orderId FK
        int productId FK
        int quantity
        float unitPrice
    }
    PRODUCT {
        int id PK
        string name
        float price
        int stock
    }

    CUSTOMER ||--o{ ORDER : places
    ORDER ||--|{ ORDER_ITEM : contains
    ORDER_ITEM }o--|| PRODUCT : references
```

**Blog schema:**
```
erDiagram
    USER ||--o{ POST : writes
    POST ||--o{ COMMENT : has
    POST }o--o{ TAG : tagged

    USER {
        int id PK
        string username UK
        string email UK
    }
    POST {
        int id PK
        int authorId FK
        string title
        text body
        date publishedAt
    }
    COMMENT {
        int id PK
        int postId FK
        int userId FK
        text content
    }
    TAG {
        int id PK
        string name UK
    }
```

**One-to-one with non-identifying:**
```
erDiagram
    EMPLOYEE {
        int id PK
        string name
    }
    EMPLOYEE_DETAIL {
        int employeeId FK
        string address
        string phone
    }
    EMPLOYEE ||..|| EMPLOYEE_DETAIL : "has details"
```

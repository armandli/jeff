# C4 Diagram

**Keywords**: `C4Context`, `C4Container`, `C4Component`, `C4Dynamic`, `C4Deployment`
**Use for**: Software architecture documentation at multiple levels of abstraction

## C4 Levels

| Keyword | Level | Shows |
|---------|-------|-------|
| `C4Context` | System Context | System + external actors/systems |
| `C4Container` | Container | Applications, databases, services within the system |
| `C4Component` | Component | Internal modules within a container |
| `C4Dynamic` | Dynamic | Sequence of interactions (numbered) |
| `C4Deployment` | Deployment | Infrastructure nodes and deployed instances |

## Element Types

**People:**
```
Person(alias, "Label", "Description")
Person_Ext(alias, "Label", "Description")   %% external
```

**Systems:**
```
System(alias, "Label", "Description")
System_Ext(alias, "Label", "Description")
SystemDb(alias, "Label", "Description")     %% database
SystemQueue(alias, "Label", "Description")  %% queue
```

**Containers:**
```
Container(alias, "Label", "Technology", "Description")
ContainerDb(alias, "Label", "Technology", "Description")
ContainerQueue(alias, "Label", "Technology", "Description")
```

**Components:**
```
Component(alias, "Label", "Technology", "Description")
ComponentDb(alias, "Label", "Technology", "Description")
```

**Deployment:**
```
Deployment_Node(alias, "Label", "Technology", "Description")
Node(alias, "Label", "Technology", "Description")
```

## Boundaries

Group related elements:
```
System_Boundary(alias, "Label") {
    Container(...)
}
Enterprise_Boundary(alias, "Label") {
    System(...)
}
Container_Boundary(alias, "Label") {
    Component(...)
}
Boundary(alias, "Label", "type") {
    Node(...)
}
```

## Relationships

```
Rel(from, to, "Label")
Rel(from, to, "Label", "Technology")
BiRel(from, to, "Label")
Rel_U(from, to, "Label")   %% Up
Rel_D(from, to, "Label")   %% Down
Rel_L(from, to, "Label")   %% Left
Rel_R(from, to, "Label")   %% Right
RelIndex(index, from, to, "Label")   %% for C4Dynamic
```

## Styling

```
UpdateElementStyle(alias, $bgColor="grey", $fontColor="white", $borderColor="red")
UpdateRelStyle(from, to, $textColor="blue", $lineColor="blue")
UpdateLayoutConfig($c4ShapeInRow="3", $c4BoundaryInRow="1")
```

## Examples

**System Context:**
```
C4Context
    title Internet Banking System - System Context

    Person(customer, "Customer", "A bank customer")
    System(banking, "Internet Banking System", "Allows customers to view accounts and make payments")
    System_Ext(email, "E-mail System", "Sends notifications")
    System_Ext(mainframe, "Mainframe Banking System", "Stores core banking data")

    Rel(customer, banking, "Uses", "HTTPS")
    Rel(banking, email, "Sends emails", "SMTP")
    Rel(banking, mainframe, "Reads/writes data", "XML/HTTPS")
```

**Container Diagram:**
```
C4Container
    title Internet Banking - Containers

    Person(customer, "Customer")

    System_Boundary(banking, "Internet Banking System") {
        Container(spa, "SPA", "React", "Single-page app")
        Container(api, "API", "Node.js", "REST API")
        ContainerDb(db, "Database", "PostgreSQL", "User and account data")
    }

    System_Ext(email, "E-mail System")

    Rel(customer, spa, "Uses", "HTTPS")
    Rel(spa, api, "Calls", "JSON/HTTPS")
    Rel(api, db, "Reads/writes", "SQL")
    Rel(api, email, "Sends email", "SMTP")
```

**Component Diagram:**
```
C4Component
    title API Application - Components

    Container_Boundary(api, "API Application") {
        Component(auth, "Auth Controller", "Express", "Handles login/logout")
        Component(accounts, "Accounts Controller", "Express", "Manages accounts")
        Component(authService, "Auth Service", "Node.js", "Validates credentials")
        ComponentDb(tokenStore, "Token Store", "Redis", "JWT tokens")
    }

    ContainerDb(db, "Database", "PostgreSQL")

    Rel(auth, authService, "Uses")
    Rel(authService, tokenStore, "Reads/writes")
    Rel(accounts, db, "Reads/writes", "SQL")
```

**Dynamic Diagram:**
```
C4Dynamic
    title Sign-in flow

    Person(customer, "Customer")
    Container(spa, "SPA", "React")
    Container(api, "API", "Node.js")
    ContainerDb(db, "Database", "PostgreSQL")

    RelIndex(1, customer, spa, "Submit credentials")
    RelIndex(2, spa, api, "POST /auth/login")
    RelIndex(3, api, db, "SELECT user WHERE email=?")
    RelIndex(4, db, api, "User record")
    RelIndex(5, api, spa, "JWT token")
    RelIndex(6, spa, customer, "Redirect to dashboard")
```

# Block Diagram

**Keyword**: `block-beta`
**Use for**: System component layout, architecture overview with explicit positioning, grid-based diagrams

Note: Beta feature (v11+).

## Basic Block

```
block-beta
    A B C
```

Blocks are placed in a single row left-to-right.

## Columns

Control grid layout:
```
block-beta
    columns 3
    A B C
    D E F
```

With 3 columns, blocks wrap automatically — 4th block starts a new row.

## Block Labels

```
block-beta
    A["Label Text"]
    B["Another Block"]
```

## Block Width (spanning columns)

```
block-beta
    columns 3
    A:2 B
    C D E
```

`A:2` spans 2 columns.

## Block Shapes

```
block-beta
    A["Rectangle"]          %% default
    B("Rounded")            %% rounded corners
    C(["Stadium"])          %% pill/stadium
    D[["Subroutine"]]       %% subroutine (double border)
    E[("Cylinder")]         %% database cylinder
    F(("Circle"))           %% circle
    G{"Diamond"}            %% rhombus
    H{{"Hexagon"}}          %% hexagon
    I[/"Parallelogram"/]    %% parallelogram
    J(["Arrow block"])      %% block arrow
```

## Composite Blocks (Nesting)

Group sub-blocks inside a parent:
```
block-beta
    block Parent["System"]
        A["Service A"]
        B["Service B"]
    end
    C["External"]
```

Specify columns within composite:
```
block-beta
    block App["Application"] : 2
        columns 2
        Web["Web UI"]
        API["REST API"]
    end
    DB[("Database")]
```

## Edges / Connections

```
block-beta
    A --> B
    A -- B
    A --> B: "label"
```

Edges are defined after block declarations.

## Space / Padding

Insert empty space:
```
block-beta
    columns 4
    A space B C
```

`space` occupies one column slot.

## Styling

```
block-beta
    A["Block"]
    style A fill:#f9f,stroke:#333
    classDef myStyle fill:#bbf
    class A myStyle
```

## Examples

**Three-tier architecture:**
```
block-beta
    columns 1
    block frontend["Frontend"] : 1
        columns 3
        web["Web App"]
        mobile["Mobile App"]
        cli["CLI"]
    end
    block backend["Backend"] : 1
        columns 2
        api["API Server"]
        worker["Worker"]
    end
    block data["Data"] : 1
        columns 3
        postgres[("PostgreSQL")]
        redis[("Redis")]
        s3["S3"]
    end

    web --> api
    mobile --> api
    cli --> api
    api --> postgres
    api --> redis
    worker --> postgres
    worker --> s3
```

**Microservices layout:**
```
block-beta
    columns 3
    gateway["API Gateway"]:3
    auth["Auth\nService"]
    users["User\nService"]
    orders["Order\nService"]
    authdb[("Auth DB")]
    userdb[("User DB")]
    orderdb[("Order DB")]

    gateway --> auth
    gateway --> users
    gateway --> orders
    auth --> authdb
    users --> userdb
    orders --> orderdb
```

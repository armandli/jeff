# Architecture Diagram

**Keyword**: `architecture-beta`
**Use for**: Infrastructure diagrams, cloud architecture, system component layout

Note: This is a beta feature (v11+). The keyword may change in future stable releases.

## Core Elements

### Services

Individual components/nodes:
```
service id(icon)[Label]
service id(icon)[Label] in groupId
```

### Groups

Containers that hold services:
```
group id(icon)[Label]
group id(icon)[Label] in parentGroupId
```

### Junctions

4-way routing node for complex edge paths:
```
junction id
junction id in groupId
```

## Icons

Built-in icons: `cloud`, `database`, `disk`, `internet`, `server`

Custom icons via Iconify: `"logos:react"`, `"mdi:github"`, etc.

```
service api(server)[API Server]
service db(database)[Database]
service cdn(cloud)[CDN]
```

## Edges (Connections)

```
serviceA:SIDE -- SIDE:serviceB
serviceA:SIDE --> SIDE:serviceB
serviceA:SIDE <-- SIDE:serviceB
serviceA:SIDE <--> SIDE:serviceB
```

**Sides**: `T` (top), `B` (bottom), `L` (left), `R` (right)

```
db:R --> L:server      %% db right-side connects to server left-side
```

Group-qualified (when service is inside a group):
```
api{group1}:R --> L:db{group2}
```

## Direction

No global direction keyword — layout is controlled by edge side assignments.

- Use `L/R` sides for horizontal flow
- Use `T/B` sides for vertical flow

## Examples

**Three-tier web app:**
```
architecture-beta
    group client[Client]
    group app[Application]
    group data[Data]

    service browser(internet)[Browser] in client
    service lb(server)[Load Balancer] in app
    service api1(server)[API 1] in app
    service api2(server)[API 2] in app
    service db(database)[Database] in data
    service cache(disk)[Cache] in data

    browser:R --> L:lb
    lb:R --> L:api1
    lb:R --> L:api2
    api1:R --> L:db
    api2:R --> L:db
    api1:B --> T:cache
    api2:B --> T:cache
```

**Cloud infrastructure:**
```
architecture-beta
    group vpc[VPC]
    group public[Public Subnet] in vpc
    group private[Private Subnet] in vpc

    service igw(internet)[Internet Gateway]
    service alb(server)[Load Balancer] in public
    service web1(server)[Web Server 1] in public
    service web2(server)[Web Server 2] in public
    service rds(database)[RDS] in private
    service elasticache(disk)[ElastiCache] in private

    igw:R --> L:alb
    alb:B --> T:web1
    alb:B --> T:web2
    web1:R --> L:rds
    web2:R --> L:rds
    web1:B --> T:elasticache
```

**Microservices:**
```
architecture-beta
    service gateway(server)[API Gateway]
    service auth(server)[Auth Service]
    service users(server)[User Service]
    service orders(server)[Order Service]
    service userdb(database)[Users DB]
    service orderdb(database)[Orders DB]
    service queue(disk)[Message Queue]

    gateway:R --> L:auth
    gateway:B --> T:users
    gateway:B --> T:orders
    users:R --> L:userdb
    orders:R --> L:orderdb
    orders:B --> T:queue
```

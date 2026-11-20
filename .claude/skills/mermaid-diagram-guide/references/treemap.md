# Treemap Diagram

**Keyword**: `treemap-beta`
**Use for**: Hierarchical data with proportional area visualization — code size, budget allocation, disk usage, category breakdowns

Note: Beta feature (v11+).

## Basic Syntax

```
treemap-beta
    "Section"
        "Leaf Item": numericValue
```

- **Section nodes** (no value): quoted label, acts as a parent grouping
- **Leaf nodes** (with value): quoted label + `: numericValue`
- Hierarchy is defined by indentation

## Rules

- Leaf nodes must have a numeric value
- Section nodes must not have a value
- Values must be positive numbers
- Sections can be nested to any depth

## Configuration

```
---
config:
  treemap:
    valueFormat: ",.0f"       # number format (d3 format string)
---
treemap-beta
    ...
```

Common `valueFormat` patterns:
- `".0f"` — integer (e.g. `42`)
- `",.0f"` — comma-separated integer (e.g. `1,234`)
- `"$,.2f"` — currency (e.g. `$1,234.56`)
- `".1%"` — percentage (e.g. `42.0%`)
- `".2s"` — SI notation (e.g. `1.2k`)

## Styling

```
treemap-beta
    classDef highlight fill:#f90,color:#000
    "Section"
        "Important Item":::highlight: 100
```

## Examples

**Codebase size by module:**
```
treemap-beta
    "Backend"
        "auth": 4200
        "users": 3100
        "payments": 8500
        "notifications": 1200
    "Frontend"
        "components": 12000
        "pages": 5600
        "utils": 900
        "styles": 2100
    "Tests"
        "unit": 7800
        "integration": 4300
        "e2e": 1500
```

**Annual budget allocation:**
```
---
config:
  treemap:
    valueFormat: "$,.0f"
---
treemap-beta
    "Engineering"
        "Salaries": 2400000
        "Infrastructure": 180000
        "Tooling": 60000
    "Marketing"
        "Campaigns": 350000
        "Content": 120000
    "Operations"
        "Office": 200000
        "Legal": 80000
        "HR": 150000
```

**Disk usage:**
```
---
config:
  treemap:
    valueFormat: ".2s"
---
treemap-beta
    "Root"
        "home"
            "documents": 5400000000
            "downloads": 22000000000
            "videos": 85000000000
        "usr"
            "local": 4200000000
            "lib": 2100000000
        "var"
            "log": 800000000
            "cache": 1500000000
```

# Mindmap

**Keyword**: `mindmap`
**Use for**: Brainstorming, feature breakdowns, knowledge organization, hierarchical ideation

## Structure

Hierarchy is defined entirely by **indentation**. The first non-whitespace line is the root.

```
mindmap
    Root
        Branch A
            Leaf 1
            Leaf 2
        Branch B
            Leaf 3
```

Use consistent indentation (spaces or tabs, but not mixed).

## Node Shapes

| Syntax | Shape |
|--------|-------|
| `Root` | Default (rounded rectangle) |
| `[Square]` | Rectangle |
| `(Rounded)` | Rounded rectangle |
| `((Circle))` | Circle |
| `(((Cloud)))` | Cloud / bubble |
| `{{Hexagon}}` | Hexagon |
| `!Bang!` | Bang / explosion |

Apply to any node, including root:
```
mindmap
    root((Central Idea))
        Topic A[Feature]
        Topic B((Option))
        Topic C{{Constraint}}
```

## Icons

Attach icons using `::icon()` with an icon class (requires compatible icon library like Font Awesome):
```
mindmap
    Root
        Design::icon(fa fa-palette)
        Development::icon(fa fa-code)
        Testing::icon(fa fa-bug)
```

## CSS Classes

Apply custom styling via `:::className`:
```
mindmap
    Root
        Urgent:::urgent
        Normal
```

Paired with CSS:
```css
.urgent { fill: #f00; color: white; }
```

## Examples

**Software project breakdown:**
```
mindmap
    root((Project Alpha))
        Frontend
            React Components
            State Management
            Routing
        Backend
            REST API
            Database
                PostgreSQL
                Redis Cache
            Auth
        DevOps
            CI/CD
            Docker
            Monitoring
        Testing
            Unit Tests
            Integration Tests
            E2E Tests
```

**Feature brainstorm:**
```
mindmap
    root[User Dashboard]
        Analytics
            Charts
            Exports
            Filters
        Notifications
            Email
            Push
            In-app
        Settings
            Profile
            Security
                2FA
                Sessions
            Preferences
```

**Architecture overview:**
```
mindmap
    root((System))
        Services
            Auth Service
            Payment Service
            Notification Service
        Data Layer
            Primary DB
            Read Replicas
            Cache
        Infrastructure
            Load Balancer
            CDN
            Object Storage
```

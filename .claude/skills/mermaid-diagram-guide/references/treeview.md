# TreeView Diagram

**Keyword**: `treeView-beta`
**Use for**: Directory/file tree visualization, hierarchical structure display, project layout documentation

Note: Beta feature (v11.14.0+).

## Basic Syntax

```
treeView-beta
    "root"
        "child1"
            "grandchild1"
            "grandchild2"
        "child2"
```

- All node labels are enclosed in double quotes
- Hierarchy is defined entirely by indentation
- The first entry is the root node

## Rules

- Every node must be quoted
- Consistent indentation is required (spaces or tabs, not mixed)
- No explicit value/weight fields (unlike treemap)
- Connection lines are auto-drawn based on hierarchy depth

## Configuration

```
---
config:
  treeView:
    rowIndent: 10       # spacing per indentation level (default 10)
    paddingX: 5         # horizontal padding
    paddingY: 5         # vertical padding
    lineThickness: 1    # branch line thickness
---
treeView-beta
    ...
```

## Theme Variables

Customizable via theme:
- `labelFontSize` — default `'16px'`
- `labelColor` — default `'black'`
- `lineColor` — default `'black'`

## Examples

**Project directory structure:**
```
treeView-beta
    "my-app"
        "src"
            "components"
                "Button.tsx"
                "Input.tsx"
                "Modal.tsx"
            "pages"
                "index.tsx"
                "about.tsx"
            "utils"
                "api.ts"
                "helpers.ts"
            "main.tsx"
        "tests"
            "unit"
                "Button.test.tsx"
            "e2e"
                "login.spec.ts"
        "public"
            "index.html"
            "favicon.ico"
        "package.json"
        "tsconfig.json"
```

**Monorepo layout:**
```
treeView-beta
    "monorepo"
        "packages"
            "api"
                "src"
                "tests"
                "package.json"
            "web"
                "src"
                "public"
                "package.json"
            "shared"
                "src"
                "package.json"
        "tools"
            "scripts"
            "config"
        "package.json"
        ".github"
            "workflows"
                "ci.yml"
                "deploy.yml"
```

**File system tree:**
```
treeView-beta
    "/var/www"
        "html"
            "index.html"
            "assets"
                "css"
                    "main.css"
                "js"
                    "app.js"
                "img"
        "logs"
            "access.log"
            "error.log"
        "conf"
            "nginx.conf"
```

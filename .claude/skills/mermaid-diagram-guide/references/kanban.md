# Kanban Diagram

**Keyword**: `kanban`
**Use for**: Task boards, workflow visualization, sprint tracking

Note: Beta feature (v11+).

## Basic Structure

```
kanban
    columnId["Column Title"]
        itemId["Item Description"]
        itemId2["Another Item"]
    column2Id["Column 2"]
        item3["Item in column 2"]
```

**Rules:**
- Columns are top-level entries with `[Title]`
- Items are indented under their column
- IDs must be unique across the diagram
- Indentation is required — items must be visually nested under columns
- **Content containing special characters** (parentheses `()`, brackets `[]`, colons `:`, etc.) **must use double-quoted labels**: `["Label with (parens)"]`. Unquoted labels treat these as syntax and will cause parse errors.

## Item Metadata

Attach metadata with `@{ key: value }` syntax:

```
kanban
    todo["To Do"]
        task1["Build login page"]@{ assigned: alice, priority: High, ticket: PROJ-42 }
```

**Supported metadata keys:**

| Key | Description |
|-----|-------------|
| `assigned` | Name of assignee |
| `ticket` | Ticket/issue ID |
| `priority` | `Very High`, `High`, `Low`, `Very Low` |

## Ticket Links

Configure a base URL to auto-link ticket IDs:

```
---
config:
  kanban:
    ticketBaseUrl: 'https://jira.company.com/browse/#TICKET#'
---
kanban
    todo["To Do"]
        task1["Fix bug"]@{ ticket: BUG-123 }
```

`#TICKET#` is replaced with the ticket value.

## Examples

**Simple sprint board:**
```
kanban
    backlog["Backlog"]
        feat1["User authentication"]
        feat2["Dashboard redesign"]
        feat3["Export to CSV"]

    inprogress["In Progress"]
        feat4["Payment integration"]@{ assigned: bob, priority: High }
        feat5["API rate limiting"]@{ assigned: alice }

    review["In Review"]
        feat6["Mobile responsive UI"]@{ ticket: PROJ-87 }

    done["Done"]
        feat7["Initial setup"]
        feat8["CI/CD pipeline"]
```

**With full metadata:**
```
kanban
    todo["To Do"]
        t1["Implement OAuth2"]@{ assigned: alice, priority: Very High, ticket: AUTH-1 }
        t2["Add refresh tokens"]@{ assigned: alice, priority: High, ticket: AUTH-2 }

    inprogress["In Progress"]
        t3["Write API docs"]@{ assigned: bob, priority: Low, ticket: DOCS-5 }

    blocked["Blocked"]
        t4["Deploy to staging"]@{ assigned: carol, priority: High, ticket: OPS-12 }

    done["Done"]
        t5["Set up repo"]@{ assigned: alice }
```

# Gantt Diagram

**Keyword**: `gantt`
**Use for**: Project timelines, sprint planning, release schedules, task dependencies

## Basic Structure

```
gantt
    title My Project
    dateFormat YYYY-MM-DD
    section Phase 1
        Task A :a1, 2024-01-01, 7d
        Task B :a2, after a1, 5d
```

## Date Formats

Input format (default `YYYY-MM-DD`):
```
dateFormat YYYY-MM-DD
dateFormat DD/MM/YYYY
dateFormat MM-DD-YYYY
```

Axis display format:
```
axisFormat %Y-%m-%d
axisFormat %d %b
axisFormat %b %Y
```

## Task Syntax

```
Task Name :modifiers, taskId, startDate, endDate|duration
```

- `startDate` — absolute (`2024-01-15`) or relative (`after taskId`)
- `endDate` — absolute date
- `duration` — `Nd` (days), `Nw` (weeks), `Nh` (hours)
- `taskId` — optional identifier for dependency references

Minimal forms:
```
Task A : 2024-01-01, 7d         %% start + duration, no ID
Task B :b1, after a1, 3d        %% ID + dependency + duration
Task C : 2024-01-01, 2024-01-08 %% start + end date
```

## Task Modifiers

| Modifier | Effect |
|----------|--------|
| `done` | Mark as completed (greyed out) |
| `active` | Highlight as in-progress |
| `crit` | Mark as critical path (red) |
| `milestone` | Render as a diamond milestone (0d duration) |

Combine: `:done, crit, a1, 2024-01-01, 5d`

## Sections

Sections group tasks visually:
```
section Design
    Wireframes :des1, 2024-01-01, 5d
section Development
    Backend :dev1, after des1, 10d
```

## Excludes

Skip weekends or specific dates:
```
excludes weekends
excludes 2024-12-25, 2024-01-01
```

## Today Marker

```
todayMarker off        %% disable
todayMarker stroke-width:5px,stroke:#f00   %% custom style
```

## Examples

**Sprint plan:**
```
gantt
    title Sprint 23
    dateFormat YYYY-MM-DD
    axisFormat %d %b

    section Design
        UI mockups     :des1, 2024-01-08, 3d
        Review         :des2, after des1, 1d

    section Development
        Backend API    :dev1, after des2, 5d
        Frontend       :dev2, after des2, 5d
        Integration    :int1, after dev1, 2d

    section QA
        Testing        :crit, qa1, after int1, 3d
        Bug fixes      :qa2, after qa1, 2d

    section Release
        Deploy         :milestone, after qa2, 0d
```

**With completed tasks:**
```
gantt
    title Q1 Roadmap
    dateFormat YYYY-MM-DD

    section Features
        Feature A  :done, f1, 2024-01-01, 14d
        Feature B  :active, f2, 2024-01-15, 21d
        Feature C  :f3, after f2, 14d
```

**Critical path:**
```
gantt
    title Release Pipeline
    dateFormat YYYY-MM-DD

    section Pipeline
        Build   :crit, b1, 2024-02-01, 1d
        Test    :crit, t1, after b1, 2d
        Deploy  :crit, milestone, after t1, 0d
```

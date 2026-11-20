---
name: mermaid-diagram-guide
description: Reference guide for Mermaid diagram syntax covering all major diagram types used in software engineering. Use when creating or writing mermaid diagrams, asking "how do I write a mermaid X diagram", "what is the syntax for mermaid", "create a sequence/flowchart/class/ER/gantt/state/C4/architecture diagram in mermaid", or "help me with mermaid". Do NOT use for rendering diagrams or non-mermaid diagram tools.
argument-hint: "[diagram-type]"
---

## Validation

To validate a diagram, write its content (without markdown fences) to a `.mmd` file. A `PostToolUse`
hook will automatically run `mmdc -i <file> -o <tmp>.svg` and feed any syntax errors back so you can
fix them. If `mmdc` is not installed, validation is skipped; the user can install it with:

```
npm install -g @mermaid-js/mermaid-cli
```

## Overview

Mermaid diagrams are declared with a diagram type keyword followed by content. All diagrams can be embedded in markdown with triple-backtick fences:

````markdown
```mermaid
<diagram-type>
  <content>
```
````

## Diagram Types Quick Reference

| Diagram | Keyword | Best For |
|---------|---------|----------|
| Flowchart | `flowchart` | Processes, workflows, decision trees |
| Sequence | `sequenceDiagram` | Inter-service/actor message flows |
| Class | `classDiagram` | OOP structure, data models |
| ER | `erDiagram` | Database schemas |
| State | `stateDiagram-v2` | State machines, lifecycle |
| Gantt | `gantt` | Project timelines |
| Requirement | `requirementDiagram` | System requirements (SysML) |
| C4 | `C4Context` / `C4Container` / `C4Component` | Software architecture |
| ZenUML | `zenuml` | Alternative sequence syntax |
| Mindmap | `mindmap` | Brainstorming, hierarchical ideas |
| Architecture | `architecture-beta` | Infrastructure, system components |
| Kanban | `kanban` | Task boards |
| Block | `block-beta` | Component layout |
| Packet | `packet-beta` | Network packet structure |
| Treemap | `treemap-beta` | Hierarchical data proportions |
| TreeView | `treeView-beta` | Directory/file trees |

## References

For complete syntax details on each diagram type, see:

- [Flowchart](references/flowchart.md)
- [Sequence Diagram](references/sequence-diagram.md)
- [Class Diagram](references/class-diagram.md)
- [Entity Relationship Diagram](references/er-diagram.md)
- [Gantt](references/gantt.md)
- [State Diagram](references/state-diagram.md)
- [Requirement Diagram](references/requirement-diagram.md)
- [C4 Diagram](references/c4-diagram.md)
- [ZenUML](references/zenuml.md)
- [Mindmap](references/mindmap.md)
- [Architecture](references/architecture.md)
- [Kanban](references/kanban.md)
- [Block Diagram](references/block-diagram.md)
- [Packet](references/packet.md)
- [Treemap](references/treemap.md)
- [TreeView](references/treeview.md)

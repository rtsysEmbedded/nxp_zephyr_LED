# Architecture documentation: portable GPIO library

| Document | Content |
|---|---|
| [DESIGN_REVIEW.md](DESIGN_REVIEW.md) | Review method, quality goals, findings, revised architecture, verified MCXN947 facts |
| [adr/](adr/) | Architecture Decision Records (one decision per file) |
| [diagrams/](diagrams/) | PlantUML sources (C4 and UML views) |

## Conventions

- One view per `.puml` file; the file name prefix gives the reading order (context to detail).
- C4 views use the C4-PlantUML standard library bundled with PlantUML (`!include <C4/...>`).
- UML views include [`diagrams/_style.iuml`](diagrams/_style.iuml); change appearance only there.
- Rendered images are build artefacts and are not committed.
- New decisions get a new ADR file with the next number; accepted ADRs are superseded, not edited.

## Rendering

Requirements: Java 11 or newer, Graphviz (`dot`), PlantUML (verified with 1.2024.7).

```bash
cd docs/architecture/diagrams
java -jar /path/to/plantuml.jar -tsvg -o ../build *.puml
```

Editors: the VS Code extension "PlantUML" (jebbs.plantuml) renders previews with the same sources.

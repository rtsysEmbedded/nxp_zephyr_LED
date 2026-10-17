# Architecture documentation: portable HAL (GPIO, I2C, SPI)

| Document | Content |
|---|---|
| [GUIDE.md](GUIDE.md) | **Start here.** Step-by-step tutorial: problem, building blocks, worked examples, concurrency, interrupts, platforms, self-check questions, glossary |
| [BUS_GUIDE.md](BUS_GUIDE.md) | **I2C / SPI tutorial**: the protocols bit by bit, hardware vs bit-bang, building blocks, bus sharing, errors, self-check questions |
| [BUS_DESIGN.md](BUS_DESIGN.md) | I2C / SPI design and design review: requirements, layers, bit-bang timing, lock hierarchy, failure-mode analysis, verified MCXN947 facts, impact on GPIO |
| [DESIGN_REVIEW.md](DESIGN_REVIEW.md) | Review method, quality goals, findings, revised architecture, verified MCXN947 facts |
| [adr/](adr/) | Architecture Decision Records (one decision per file): 0001-0004 GPIO, 0005-0008 I2C/SPI and the unified HAL |
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

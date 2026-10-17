# Architecture documentation: portable HAL (GPIO, I2C, SPI)

| Document | Content |
|---|---|
| [HAL_ARCHITECTURE.md](HAL_ARCHITECTURE.md) | **Start here.** The whole `hal` package: modules, dependency rules, inheritance vs composition, complete class hierarchy, worked example (SPI in GPIO mode drives an I/O expander), source tree and CMake targets, name map |
| [GUIDE.md](GUIDE.md) | **GPIO and I/O-expander tutorial.** Step-by-step tutorial: problem, building blocks, worked examples, concurrency, interrupts, platforms, self-check questions, glossary |
| [BUS_GUIDE.md](BUS_GUIDE.md) | **I2C / SPI tutorial**: the protocols bit by bit, hardware vs bit-bang, building blocks, bus sharing, errors, self-check questions |
| [BUS_DESIGN.md](BUS_DESIGN.md) | I2C / SPI design and design review: requirements, layers, bit-bang timing, lock hierarchy, failure-mode analysis, verified MCXN947 facts, impact on GPIO |
| [DESIGN_REVIEW.md](DESIGN_REVIEW.md) | GPIO design review (dated record, original names): method, quality goals, findings, verified MCXN947 facts |
| [adr/](adr/) | Architecture Decision Records (one decision per file): 0001-0004 GPIO, 0005-0008 I2C/SPI and the unified HAL, 0009 one package structure |
| [diagrams/](diagrams/) | PlantUML sources: `01`-`10` GPIO and expanders, `11`-`20` I2C/SPI, `21`-`24` whole package (package, inheritance, collaboration, object example) |

## Conventions

- One view per `.puml` file; the file name prefix gives the reading order (context to detail).
- C4 views use the C4-PlantUML standard library bundled with PlantUML (`!include <C4/...>`).
- UML views include [`diagrams/_style.iuml`](diagrams/_style.iuml); change appearance only there.
- Rendered images are build artefacts and are not committed.
- New decisions get a new ADR file with the next number; accepted ADRs are superseded, not edited. The only allowed change is a
  dated cross-reference note ("Updated YYYY-MM-DD: ...") when a statement about the documents themselves becomes outdated.

## Rendering

Requirements: Java 11 or newer, Graphviz (`dot`), PlantUML (verified with 1.2024.7).

```bash
cd docs/architecture/diagrams
java -jar /path/to/plantuml.jar -tsvg -o ../build *.puml
```

Editors: the VS Code extension "PlantUML" (jebbs.plantuml) renders previews with the same sources.

# ADR-0001: Layering and polymorphism strategy

| Field | Value |
|---|---|
| Status | Accepted (design review 2026-10-16) |
| Supersedes | Concept-based `GpioBackend` template from design iteration 1 |
| Related | [DESIGN_REVIEW.md](../DESIGN_REVIEW.md) findings M-1, M-2, m-2; diagrams `03`, `04`, `04b` |

## Context

The library must run on Linux, Zephyr, FreeRTOS and bare-metal, and one board can mix
native SoC pins with I2C/SPI expander pins. Design iteration 1 chose compile-time
polymorphism for pins (`Output<Backend>` constrained by a C++20 concept). Iteration 2
introduced run-time interfaces (`IGpioPort`, `IBus`). The two are inconsistent: an array of
ports holding both `McuxGpioPort` and `ExpanderPort` requires type erasure, which a
template parameter on the pin cannot provide without duplicating every pin type per port type.

## Decision

1. **Three run-time layers**, each with one responsibility:
   - `OutputPin` / `InputPin`: stateless value handles `{IGpioPort*, mask, active_low}`.
   - `IGpioPort`: one GPIO controller (native or expander). It owns all mutable pin state.
   - `IBus`: one physical I2C/SPI bus. It owns the bus mutex.
2. **Dynamic polymorphism (virtual) only at `IGpioPort`, `IBus`, `IExpanderChip`, `IConfigSource`.**
   These boundaries are crossed once per operation; the cost (one indirect call) is negligible
   compared with an I2C transfer and acceptable for native writes.
3. **Static polymorphism (link-time selection) for the OSAL and platform adapters.** CMake cache
   variables `GPIO_OS` and `GPIO_SILICON` (set in `CMakePresets.json`) choose which source
   directory is compiled. The portable core contains no `#if` on the platform and no OS include.
4. **Language baseline: C++17**, compiled with `-fno-exceptions -fno-rtti`; no heap after `init()`;
   errors are returned as `Result<T>` (own minimal type; `std::expected` needs C++23).
5. **Two-phase initialisation**: constructors never access hardware; `GpioSystem::init()` and
   `start()` return `Result`.

## Alternatives considered

| Alternative | Reason rejected |
|---|---|
| Fully static (templates/concepts for pins and ports) | Heterogeneous port collections still need type erasure; template instantiation per pin/port pair increases flash; C++20 required. |
| C function-pointer tables | Equivalent cost to a vtable with less type safety. |
| `#ifdef` per platform inside the core | Untestable combinatorics; violates the open/closed principle. |
| Per-pin objects owning state and a mutex | Creates a second source of truth beside the port shadow (finding C-1). |

## Consequences

- Positive: new silicon or a new expander means adding one class; the core and the application stay unchanged.
- Positive: mock `IGpioPort` / `IBus` give clean host-test seams.
- Negative: vtables need `-fno-rtti`-compatible design (no `dynamic_cast`); interface destructors are protected and non-virtual to avoid pulling in `operator delete`.
- Negative: C++17 excludes concepts; interface contracts are documented and checked by tests instead.

# ADR-0009: One `hal` package: modules, dependency rules, common base, composition root

| Field | Value |
|---|---|
| Status | Accepted (package review 2026-10-17) |
| Amends | [ADR-0001](0001-layering-and-polymorphism.md) (CMake variables `GPIO_OS`/`GPIO_SILICON` renamed), [ADR-0003](0003-configuration-single-source-json.md) (file names), [ADR-0005](0005-bus-layering-controller-device-register.md), [ADR-0008](0008-unified-hal-initialisation-order.md) (who creates the objects) |
| Related | [HAL_ARCHITECTURE.md](../HAL_ARCHITECTURE.md); diagrams `21`-`24` |

## Context

GPIO, I/O expanders, I2C and SPI were designed one after another. They are one package, and they depend on each other:

- an I/O expander **is** a GPIO port (its pins are ordinary `OutputPin` objects);
- an I/O expander **uses** an I2C or SPI device;
- an I2C or SPI controller in GPIO (bit-bang) mode **uses** GPIO pins;
- an SPI device **uses** a GPIO pin as chip select.

The documents described these as separate libraries joined by amendment notes, used different names for the
same things (`IBus` / `IRegisterDevice`, `GpioSystem` / `hal::System`, `gpio.json` for a file that also holds buses),
had no common base for the elements `hal::System` initialises, and did not say who creates platform objects
without making `system` depend on every platform.

## Decision

1. **One package `hal`** with the modules `core`, `osal`, `config`, `gpio`, `i2c`, `spi`, `regs`, `expander`, `system`,
   and `hal::platform::{mcux, zephyr, linux, host}` for platform adapters (diagram `21`).
2. **Dependencies point downwards only** (no cycles):
   `core ← osal ← gpio ← {i2c, spi} ← regs ← expander`; `config ← core`; `system ← {gpio, regs, config}`;
   platform adapters depend on the module interfaces they implement. A module defines interfaces; higher modules
   implement or use them (dependency inversion: `gpio` defines `IGpioPort`, `expander` implements it).
3. **One CMake target per module** (`hal_core`, `hal_osal`, `hal_gpio`, ...). Each module has its own include root;
   `target_link_libraries` is exactly the arrow set of diagram `21`. A forbidden `#include` fails to compile because
   the include path is not visible.
4. **Common base `hal::core::IComponent`** (`init()`, `id()`, `health()`), inherited by `IGpioPort`,
   `II2cController` and `ISpiController`. Everything with hardware state that `hal::System` initialises is an
   `IComponent`. Value handles (pins, devices, register devices, transactions) are not.
5. **Inheritance only for interfaces ("is a"), reuse only by composition ("has a / uses")**. Example: `BitBangSpi`
   **is an** `ISpiController` and **has** `gpio::OutputPin` / `gpio::InputPin` members; it does not inherit from any
   GPIO class. No implementation inheritance across modules, no multiple inheritance of implementations.
6. **Composition root is generated.** `hal_codegen.py` emits, per target, `hal_config.hpp` (descriptor tables) and
   `hal_instances.cpp`, which defines every component object statically, in topological order, and hands
   `hal::System` an array of `IComponent*`. Only this generated file names platform classes. `system` therefore
   depends on no platform.
7. **Names**: configuration `config/hal.json`, `config/hal.schema.json`, generator `tools/hal_codegen.py`, outputs
   `hal_config.hpp`, `hal_instances.cpp`, `hal.overlay`, `hal.conf`; CMake variables `HAL_OS`, `HAL_SILICON`.
   Living documents and diagrams use these names; dated review records keep their original names as history.

## Alternatives considered

| Alternative | Reason rejected |
|---|---|
| Separate libraries (gpio-lib, bus-lib) with an adapter between them | The dependencies run in both directions (expander uses bus, bus uses GPIO pins); two libraries would need a cycle or a third "glue" library. |
| `BitBangSpi` inherits from a GPIO port class | Violates substitutability: code holding an `IGpioPort*` could call `write_masked()` on an SPI controller. The SPI controller would also expose GPIO methods it must not offer. |
| A deep common base class with shared implementation (`Peripheral` with mutex, state machine, logging) | Couples unrelated modules through one base; changes ripple everywhere. The common part is only the lifecycle interface. |
| `hal::System` creates objects with `new` from a registry of factories | Needs heap and RTTI-like registration; platform classes would leak into `system`. |

## Consequences

- One diagram set describes the whole package (`21` package, `22` inheritance, `23` collaboration, `24` object example).
- Adding a module (for example UART, PWM, ADC) follows the same template: interface in its module, inheriting
  `IComponent`; implementations in `hal::platform`; configuration in `hal.json`; generated instantiation.
- The build enforces the package structure; a dependency violation is a compile error, not a review finding.

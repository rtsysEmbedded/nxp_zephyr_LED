# The `hal` Package: One Architecture for GPIO, I/O Expanders, I2C and SPI

This is the entry document. It shows how all parts of the library form **one package**: which modules exist,
which module may use which, where inheritance is used and where composition is used, and how one concrete example
(SPI in GPIO mode driving an I/O expander) becomes real objects at run time.

Detailed tutorials: [GUIDE.md](GUIDE.md) (GPIO, expanders), [BUS_GUIDE.md](BUS_GUIDE.md) (I2C, SPI).
Decision record for this document: [ADR-0009](adr/0009-one-hal-package-structure.md).

| Section | Content |
|---|---|
| 1 | The modules of the package |
| 2 | Dependency rules between modules |
| 3 | Inheritance vs composition: the two relationships, and when each is used |
| 4 | The common base `IComponent` |
| 5 | The complete inheritance hierarchy |
| 6 | How the modules use each other |
| 7 | Worked example: SPI in GPIO mode drives an I/O expander |
| 8 | Source tree, namespaces and CMake targets |
| 9 | Name map: earlier documents → current names |
| 10 | Adding a new module (UART, PWM, ADC, ...) |
| 11 | Self-check questions |

---

## 1. The modules of the package

| Module | Namespace | Responsibility | Main types |
|---|---|---|---|
| core | `hal::core` | Types every module needs | `Result<T>`, `Error`, `IComponent`, `ComponentId`, `Health`, `Duration` |
| osal | `hal::osal` | Same concurrency primitives on every OS (implementation chosen at link time) | `Mutex`, `ScopedLock`, `CriticalSection`, `Context`, `EventSignal`, `Clock` |
| config | `hal::config` | Access to the generated or loaded configuration | `IConfigSource`, `PinDesc`, `PortDesc`, `BusDesc`, `DeviceDesc` |
| gpio | `hal::gpio` | Pins and GPIO controllers | `IGpioPort`, `OutputPin`, `InputPin`, `OpenDrainPin`, `EventDispatcher` |
| i2c | `hal::i2c` | I2C buses and devices | `II2cController`, `I2cDevice`, `I2cTransaction`, `BitBangI2c` |
| spi | `hal::spi` | SPI buses and devices | `ISpiController`, `SpiDevice`, `SpiTransaction`, `ChipSelect`, `BitBangSpi` |
| regs | `hal::regs` | Register access on top of an I2C or SPI device | `IRegisterDevice`, `I2cRegisterDevice`, `SpiRegisterDevice`, `ISpiFraming` |
| expander | `hal::expander` | I/O expander chips as GPIO ports | `ExpanderPort`, `IExpanderChip`, `Pca95xxChip`, `Mcp23x17Chip`, `ShiftRegChip` |
| system | `hal::system` | Start-up and lookup by name | `System` |
| platform | `hal::platform::{mcux, zephyr, linux, host}` | Implementations that call the OS or the SDK | `McuxGpioPort`, `McuxLpi2c`, `McuxLpspi`, `ZephyrI2c`, `LinuxSpidev`, ... |
| generated | (per build) | Composition root: defines every object | `hal_config.hpp`, `hal_instances.cpp` |

## 2. Dependency rules between modules

Diagram: [`21_package_hal.puml`](diagrams/21_package_hal.puml).

```
                 system ──────────────┐
                   │                  │
   expander ───► regs ───► i2c ──┐    │
       │                   spi ──┤    │
       └──────────────────────► gpio ◄┘
                                 │
                       osal ◄────┘
                        │
   config ──────────► core ◄── (everything)

   platform ──► gpio, i2c, spi, osal   (implements their interfaces)
   generated ─► system, expander, platform  (creates the objects)
```

| Rule | Why |
|---|---|
| Arrows point downwards only; no cycles | A module can be understood, built and tested using only the modules below it |
| A lower module **defines** an interface, a higher module **implements or uses** it | `gpio` must not know expanders, yet an expander must be a GPIO port. Solution: `gpio` defines `IGpioPort`; `expander` implements it (dependency inversion) |
| Platform adapters depend on module interfaces, never the reverse | The portable modules contain no OS or SDK include |
| Only the generated composition root names platform classes | `system` stays portable; objects are static, no heap |

The build enforces these rules: one CMake target per module, and `target_link_libraries` contains exactly the
arrows above (section 8). An `#include` of a module that is not below you fails to compile.

## 3. Inheritance vs composition

This is the most important idea for reading the class diagrams.

| Relationship | Question it answers | UML notation | C++ |
|---|---|---|---|
| **Inheritance (generalisation / realisation)** | "Can this object be used **everywhere** the base is expected?" (is-a) | hollow triangle: solid line between interfaces, dashed line class → interface | `class ExpanderPort : public gpio::IGpioPort` |
| **Composition** | "Does this object **own** that part for its whole life?" (has-a) | filled diamond at the owner | a member object: `gpio::OutputPin sck_;` |
| **Association** | "Does this object **use** that one, which lives elsewhere?" | arrow | a pointer or reference member: `IGpioPort* port_;` |
| **Dependency** | "Does this object **mention** that type in a call?" | dashed arrow | a parameter or local variable |

**Rule of this package:** inheritance is used **only for interfaces**. Code is reused **by composition**.

### The example you asked about: SPI in GPIO mode

`BitBangSpi` creates SPI waveforms by switching GPIO pins. It reuses the GPIO module, but how?

| Option | What it would mean | Verdict |
|---|---|---|
| `class BitBangSpi : public gpio::SomeGpioClass` | A `BitBangSpi` **is a** GPIO thing: code holding an `IGpioPort*` could call `write_masked()` on the SPI controller | Wrong: an SPI controller is not a GPIO port; it would offer methods it must not offer (Liskov substitution principle) |
| `class BitBangSpi : public spi::ISpiController` with members `gpio::OutputPin sck_, mosi_; gpio::InputPin miso_;` | A `BitBangSpi` **is an** SPI controller and **has** three GPIO pins | **Correct**: every user of `ISpiController` can use it; it reuses the GPIO classes exactly as any application would |

```cpp
// Illustrative: how the modules meet in one class.
namespace hal::spi {
class BitBangSpi final : public ISpiController {      // is-a: SPI controller (and, through it, IComponent)
public:
    BitBangSpi(gpio::OutputPin sck, gpio::OutputPin mosi,
               gpio::InputPin miso, const IDelay& delay);  // parts handed in by the composition root
    Result<void> init() override;                      // from core::IComponent
    Result<void> configure(const SpiConfig& cfg) override;
    Result<void> transfer(std::span<SpiSegment> segs) override;
private:
    gpio::OutputPin sck_, mosi_;                       // has-a: reused GPIO classes
    gpio::InputPin  miso_;
    const IDelay&   delay_;
    osal::Mutex     mtx_;                              // the bus lock (level 2)
};
}
```

The same pattern appears everywhere in the package:

| Class | Is a (inherits) | Has / uses (composition, association) |
|---|---|---|
| `BitBangSpi` | `spi::ISpiController` | `gpio::OutputPin` ×2, `gpio::InputPin`, `IDelay`, `osal::Mutex` |
| `BitBangI2c` | `i2c::II2cController` | `gpio::OpenDrainPin` ×2, `IDelay`, `osal::Mutex` |
| `SpiDevice` | (value handle, no base) | `ISpiController*`, `ChipSelect` (which may hold a `gpio::OutputPin`) |
| `SpiRegisterDevice` | `regs::IRegisterDevice` | `spi::SpiDevice`, `ISpiFraming` |
| `ExpanderPort` | `gpio::IGpioPort` | `regs::IRegisterDevice&`, `IExpanderChip&`, optional `gpio::InputPin` (INT line), `osal::Mutex` |
| `McuxGpioPort` | `gpio::IGpioPort` | SDK `GPIO_Type*` |
| `McuxLpspi` | `spi::ISpiController` | SDK LPSPI instance, `osal::Mutex` |

## 4. The common base `IComponent`

`hal::System` must initialise ports, bus controllers and expanders in one generated order. They need one common
interface:

```cpp
// Illustrative.
namespace hal::core {
enum class Health : uint8_t { Ok, Degraded, Faulted };

class IComponent {
public:
    virtual Result<void> init() = 0;       // two-phase init: constructors never touch hardware
    virtual ComponentId  id() const = 0;   // index in the generated tables, used in error reports
    virtual Health       health() const = 0;
protected:
    ~IComponent() = default;               // no deletion through the interface, no operator delete needed
};
}
```

| Inherits `IComponent` (through its interface) | Does not |
|---|---|
| `IGpioPort` → `McuxGpioPort`, `ZephyrGpioPort`, `LinuxGpiodPort`, `ExpanderPort` | Pins: `OutputPin`, `InputPin`, `OpenDrainPin` |
| `II2cController` → `BitBangI2c`, `McuxLpi2c`, `ZephyrI2c`, `LinuxI2cDev` | Devices: `I2cDevice`, `SpiDevice`, `ChipSelect` |
| `ISpiController` → `BitBangSpi`, `McuxLpspi`, `ZephyrSpi`, `LinuxSpidev` | Register devices, transactions, framing strategies |

The rule: **a component owns hardware state and must be initialised; a handle only points to a component.**
Handles are small values that can be copied freely.

## 5. The complete inheritance hierarchy

Diagram: [`22_class_hal_inheritance.puml`](diagrams/22_class_hal_inheritance.puml).

| Interface (module) | Base | Portable implementations | Platform implementations |
|---|---|---|---|
| `IGpioPort` (gpio) | `IComponent` | `ExpanderPort` (expander) | `McuxGpioPort`, `ZephyrGpioPort`, `LinuxGpiodPort` |
| `II2cController` (i2c) | `IComponent` | `BitBangI2c` | `McuxLpi2c`, `ZephyrI2c`, `LinuxI2cDev` |
| `ISpiController` (spi) | `IComponent` | `BitBangSpi` | `McuxLpspi`, `ZephyrSpi`, `LinuxSpidev` |
| `IRegisterDevice` (regs) | - | `I2cRegisterDevice`, `SpiRegisterDevice` | - |
| `ISpiFraming` (regs) | - | `RwBitFraming`, `OpcodeFraming` | - |
| `IExpanderChip` (expander) | - | `Pca95xxChip`, `Mcp23x17Chip`, `ShiftRegChip` | - |
| `IDelay` (i2c/spi) | - | - | `CycleCounterDelay`, `TimerDelay`, `CalibratedLoopDelay` |
| `IConfigSource` (config) | - | generated tables | `JsonConfigSource` (Linux) |
| `IBusRecoveryHook` (i2c) | - | - | board-specific |

Depth is at most two levels (`IComponent` → module interface → class). There is no implementation inheritance across
modules and no multiple inheritance of implementations.

## 6. How the modules use each other

Diagram: [`23_class_hal_collaboration.puml`](diagrams/23_class_hal_collaboration.puml).

| From (module) | Uses (module) | How | Why |
|---|---|---|---|
| `BitBangI2c` (i2c) | `OpenDrainPin` (gpio) | composition ×2 | SCL and SDA are GPIO pins in open-drain mode |
| `BitBangSpi` (spi) | `OutputPin`, `InputPin` (gpio) | composition ×3 | SCK, MOSI, MISO |
| `ChipSelect` (spi) | `OutputPin` (gpio) | aggregation, 0..1 | GPIO chip select |
| `I2c/SpiRegisterDevice` (regs) | `I2cDevice` / `SpiDevice` | composition | register framing on top of a device |
| `ExpanderPort` (expander) | `IRegisterDevice` (regs) | association | reach the chip over I2C or SPI |
| `ExpanderPort` (expander) | `IGpioPort` (gpio) | realisation | be a GPIO port |
| `ExpanderPort` (expander) | `InputPin`, `EventDispatcher` (gpio) | association | INT line; report input changes |
| `System` (system) | `IComponent` (core) | aggregation of `IComponent*` | init table |
| `System` (system) | pins, register devices | creates handles by name | lookup before `start()` |

**The loop that is not a cycle.** At run time, objects form a chain that returns to the GPIO module:
an expander pin (`gpio::OutputPin`) → `ExpanderPort` → `SpiRegisterDevice` → `SpiDevice` → `BitBangSpi` →
native `gpio::OutputPin`s. At package level there is still no cycle, because the chain only ever goes through
**interfaces defined in lower modules**. The build forbids the one dangerous variant: bit-bang pins or chip selects on an
expander port, which would take an expander lock while holding a bus lock (ADR-0007).

## 7. Worked example: SPI in GPIO mode drives an I/O expander

Diagram: [`24_object_spi_gpio_expander.puml`](diagrams/24_object_spi_gpio_expander.puml).

**Configuration** (shape only; values illustrative):

```json
{
  "ports":   [ { "id": "gpio_a", "type": "native" },
               { "id": "exp_b",  "type": "mcp23s17", "driver": "lib", "device": "exp_b_dev", "can_sleep": true } ],
  "pins":    [ { "name": "soft_sck",  "port": "gpio_a", "index": 0, "dir": "out", "fail_safe": "low" },
               { "name": "soft_mosi", "port": "gpio_a", "index": 1, "dir": "out", "fail_safe": "low" },
               { "name": "soft_miso", "port": "gpio_a", "index": 2, "dir": "in" },
               { "name": "exp_b_cs",  "port": "gpio_a", "index": 3, "dir": "out", "fail_safe": "high" },
               { "name": "relay_1",   "port": "exp_b",  "index": 3, "dir": "out", "fail_safe": "low" } ],
  "buses":   [ { "id": "spi_soft", "type": "spi", "impl": "bitbang", "driver": "lib",
                 "pins": { "sck": "soft_sck", "mosi": "soft_mosi", "miso": "soft_miso" }, "delay": "cycle_counter" } ],
  "devices": [ { "id": "exp_b_dev", "bus": "spi_soft", "spi_mode": 0, "max_hz": 1000000,
                 "cs": { "kind": "gpio", "pin": "exp_b_cs", "active_high": false },
                 "registers": { "framing": "opcode", "chip": "mcp23s17", "hw_address": 0 } } ]
}
```

**What the generator does:** builds the graph `gpio_a → (soft_sck, soft_mosi, soft_miso, exp_b_cs) → spi_soft → exp_b_dev → exp_b → relay_1`,
checks that all bus pins and the chip select are on a native, lock-free port, and emits `hal_instances.cpp`:

```cpp
// Illustrative extract of generated code (composition root). Order = topological order.
static platform::mcux::McuxGpioPort gpio_a{ cfg::ports[0] };
static spi::BitBangSpi spi_soft{ gpio::OutputPin{gpio_a, cfg::pins[0]}, gpio::OutputPin{gpio_a, cfg::pins[1]},
                                 gpio::InputPin{gpio_a, cfg::pins[2]}, platform::mcux::cycle_counter_delay };
static spi::SpiDevice        exp_b_dev{ spi_soft, cfg::devices[0],
                                         spi::ChipSelect::gpio(gpio::OutputPin{gpio_a, cfg::pins[3]}) };
static regs::OpcodeFraming   exp_b_framing{ cfg::chips[0] };
static regs::SpiRegisterDevice exp_b_regs{ exp_b_dev, exp_b_framing };
static expander::Mcp23x17Chip  exp_b_chip{ cfg::chips[0] };
static expander::ExpanderPort  exp_b{ exp_b_regs, exp_b_chip, cfg::ports[1] };
static core::IComponent* const init_table[] = { &gpio_a, &spi_soft, &exp_b };
```

**What happens on `relay_1.set(High)`:**

| Step | Object (module) | Action | Lock |
|---|---|---|---|
| 1 | `relay_1` (gpio) | `port_->write_masked(bit 3, 1)` — virtual call into `ExpanderPort` | none |
| 2 | `exp_b` (expander) | computes the new shadow value | takes exp_b lock (level 1) |
| 3 | `exp_b_regs` (regs) | builds `[opcode W, OLATA, value]` | none |
| 4 | `exp_b_dev` (spi) | `begin()`: configure mode 0, assert `exp_b_cs` | takes spi_soft lock (level 2) |
| 5 | `spi_soft` (spi) | toggles `soft_sck`, writes `soft_mosi`, reads `soft_miso` | none (single `PSOR`/`PCOR` stores on `gpio_a`) |
| 6 | back up the chain | release CS, release spi_soft lock; commit shadow; release exp_b lock | reverse order |

`relay_1` and `soft_sck` are objects of **the same class** `gpio::OutputPin`. The only difference is the port they point to:
`exp_b` (an `ExpanderPort`) or `gpio_a` (a `McuxGpioPort`). That is the benefit of the common interface.

## 8. Source tree, namespaces and CMake targets

Planned layout (each module has its own include root, so the include path itself enforces the dependency rules):

```
lib/hal/
├── core/       include/hal/core/{result.hpp, error.hpp, component.hpp, time.hpp}
├── osal/       include/hal/osal/{mutex.hpp, critical_section.hpp, context.hpp, event_signal.hpp, clock.hpp}
├── config/     include/hal/config/{config_source.hpp, descriptors.hpp}
├── gpio/       include/hal/gpio/{port.hpp, pins.hpp, event_dispatcher.hpp}     src/
├── i2c/        include/hal/i2c/{controller.hpp, device.hpp, bitbang_i2c.hpp}   src/
├── spi/        include/hal/spi/{controller.hpp, device.hpp, bitbang_spi.hpp}   src/
├── regs/       include/hal/regs/{register_device.hpp, framing.hpp}             src/
├── expander/   include/hal/expander/{expander_port.hpp, chips.hpp}             src/
├── system/     include/hal/system/system.hpp                                   src/
└── platform/   mcux/  zephyr/  linux/  host/      (OSAL implementations + adapters)
config/hal.json, config/hal.schema.json, config/chips/*.json
tools/hal_codegen.py
```

| CMake target | `target_link_libraries` (PUBLIC) |
|---|---|
| `hal_core` | - |
| `hal_osal` | `hal_core` |
| `hal_config` | `hal_core` |
| `hal_gpio` | `hal_core`, `hal_osal` |
| `hal_i2c` | `hal_gpio`, `hal_osal`, `hal_core` |
| `hal_spi` | `hal_gpio`, `hal_osal`, `hal_core` |
| `hal_regs` | `hal_i2c`, `hal_spi` |
| `hal_expander` | `hal_gpio`, `hal_regs` |
| `hal_system` | `hal_gpio`, `hal_regs`, `hal_config` |
| `hal_platform_<x>` | `hal_gpio`, `hal_i2c`, `hal_spi`, `hal_osal` + SDK / OS libraries |
| `hal_generated` (per build) | `hal_system`, `hal_expander`, `hal_platform_<x>` |

`<x>` comes from `HAL_OS` / `HAL_SILICON` in `CMakePresets.json`.

## 9. Name map: earlier documents → current names

The dated review records (DESIGN_REVIEW.md, ADR-0001 to ADR-0004) keep the names they were written with. All other
documents and all diagrams use the current names.

| Earlier name | Current name | Decided in |
|---|---|---|
| `gpio::IBus` (`write_reg`, `read_reg`, `mutex()`, `recover()`) | `hal::regs::IRegisterDevice` (`write_reg`, `read_reg`); recovery in the bus controller; no `mutex()` | ADR-0005 |
| `IExpanderChip::frame()` | removed; `regs::ISpiFraming` and I2C register width | ADR-0005 |
| `GpioSystem` | `hal::system::System` | ADR-0008 |
| `gpio::Error`, `gpio::Result` | `hal::core::Error`, `hal::core::Result` | ADR-0005 |
| `McuxLpi2cBus`, `ZephyrI2cBus`, `LinuxI2cDevBus` (and SPI) | `McuxLpi2c`, `ZephyrI2c`, `LinuxI2cDev`, `McuxLpspi`, `ZephyrSpi`, `LinuxSpidev` | ADR-0005 |
| `config/gpio.json`, `gpio.schema.json`, `gpio_codegen.py`, `gpio_config.hpp`, `gpio.overlay`, `gpio.conf` | `config/hal.json`, `hal.schema.json`, `hal_codegen.py`, `hal_config.hpp`, `hal.overlay`, `hal.conf` | ADR-0009 |
| CMake `GPIO_OS`, `GPIO_SILICON` | `HAL_OS`, `HAL_SILICON` | ADR-0009 |
| (no common base) | `hal::core::IComponent` | ADR-0009 |

## 10. Adding a new module (UART, PWM, ADC, ...)

Every module follows the same template, so a new one fits without changing existing modules:

| Step | Example for UART |
|---|---|
| 1. Interface in its own module, inheriting `IComponent` | `hal::uart::IUartController : core::IComponent` |
| 2. Value handle for users | `hal::uart::UartPort` (points to the controller) |
| 3. Portable implementation if useful, built by composition | `SoftUart` **has** `gpio::OutputPin` (TX) and `gpio::InputPin` (RX) |
| 4. Platform implementations | `McuxLpuart` (LP_FLEXCOMM, `PERSEL = UART`), `ZephyrUart`, `LinuxTty` |
| 5. Configuration in `hal.json` + schema + generator checks | baud rate, pins, FLEXCOMM function conflicts |
| 6. CMake target with downward dependencies only | `hal_uart` → `hal_gpio`, `hal_osal`, `hal_core` |
| 7. ADR + diagrams | inheritance (22) and collaboration (23) updated |

## 11. Self-check questions

1. Why does `BitBangSpi` not inherit from a GPIO class, and how does it reuse the GPIO module instead?
2. Where is `IGpioPort` defined, and why not in the `expander` module?
3. Why are pins and devices not `IComponent`s?
4. Who creates the `McuxGpioPort` object, given that `hal::system` must not include platform headers?
5. Code in the `spi` module includes `hal/expander/expander_port.hpp`. What happens, and why is that good?
6. `relay_1` and `soft_sck` in section 7 are the same class. What is the only difference between them?

<details>
<summary>Answers</summary>

1. A `BitBangSpi` is an SPI controller, not a GPIO port; inheriting would let GPIO users call port methods on it (Liskov violation). It reuses GPIO by **composition**: it owns `OutputPin`/`InputPin` members and calls `set()`/`read()` like any application.
2. In `gpio`. Expanders must be usable everywhere a GPIO port is expected, but `gpio` must not depend on `expander` (that would be a cycle). So the lower module defines the interface and the higher module implements it (dependency inversion).
3. They own no hardware state and need no initialisation; they are small values pointing to a component. Only components appear in the init table.
4. The generated composition root (`hal_instances.cpp`). It defines all objects statically in topological order and hands `System` an array of `IComponent*`.
5. It fails to compile: `hal_spi` does not link `hal_expander`, so that include path is not visible. Good, because the dependency would create a package cycle (`expander → regs → spi → expander`).
6. The `port_` pointer: `relay_1` points to `exp_b` (`ExpanderPort`), `soft_sck` to `gpio_a` (`McuxGpioPort`). The virtual call through `IGpioPort` does the rest.

</details>

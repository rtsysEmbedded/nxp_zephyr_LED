# Portable GPIO Library: Architecture Design Review

| Item | Value |
|---|---|
| Version | 1.0 (draft for review) |
| Date | 2026-10-16 |
| Scope | Design iterations 1-3 (portable API; I2C/SPI expanders; FreeRTOS and bare-metal) and the current repository baseline |
| Reference target | FRDM-MCXN947, Zephyr board `frdm_mcxn947/mcxn947/cpu0`, Arm Cortex-M33 |
| Decisions | [ADR-0001](adr/0001-layering-and-polymorphism.md), [ADR-0002](adr/0002-concurrency-model.md), [ADR-0003](adr/0003-configuration-single-source-json.md), [ADR-0004](adr/0004-expander-driver-selection.md) |
| Diagrams | [diagrams/](diagrams/) (PlantUML sources, index in section 10) |

> **Amended 2026-10-17 by the I2C/SPI design** ([BUS_DESIGN.md](BUS_DESIGN.md) section 9): `IBus` is replaced by `hal::IRegisterDevice`, `GpioSystem` by `hal::System`, and the lock hierarchy is extended to buses. Names in this document are kept as originally reviewed.

---

## 1. Purpose and scope

This review checks the proposed portable C++ GPIO library against explicit quality goals,
records every defect found in the three design iterations, and states the corrected
architecture. It also lists defects in the current repository that the library work will touch.

Out of scope: pin multiplexing, clock gating and TrustZone attribution (board initialisation),
safety certification, and the Linux kernel / Zephyr expander drivers themselves.

## 2. Review method and tools

| Method | Used for | Artefact |
|---|---|---|
| C4 model, levels 1-3 | Structure at increasing zoom: context, build/run-time units, components | `01`-`03` |
| UML 2.5 class diagrams | Static structure, ownership, interfaces, realizations | `04`, `04b` |
| UML sequence diagrams | Lock order, context switches, failure paths | `05`, `06` |
| UML state machine | Lifecycle and error recovery of the asynchronous transfer engine | `07` |
| UML deployment diagram | Which artefacts are linked on which target | `08` |
| UML activity diagram | Configuration validation and generation pipeline | `09` |
| Architecture Decision Records (Nygard format) | Decisions with context, alternatives, consequences | `adr/` |
| Checklist review against prioritised quality goals | Finding classification | section 3, section 5 |
| Evidence from vendor data | Hardware facts verified against `svd/MCXN947_cm33_core0.xml` in this repository | section 7 |

Tooling: PlantUML 1.2024.7 with the bundled C4-PlantUML standard library, Graphviz 2.43 for
layout. Text-based diagrams are version-controlled and reviewed in pull-request diffs; rendered
images are build artefacts and are not committed.

Severity scale:

| Severity | Meaning |
|---|---|
| **Critical** | Implementing as written causes data corruption, wrong physical output, or deadlock. |
| **Major** | Design inconsistency or gap that causes defects on at least one target, or blocks portability. |
| **Minor** | Clarity, efficiency, or maintainability issue without direct functional failure. |

## 3. Quality goals (review criteria, in priority order)

| # | Goal | Measurable criterion |
|---|---|---|
| Q1 | Correctness under concurrency | No data race (ThreadSanitizer clean on host), no deadlock (fixed lock order), no blocking call in ISR context |
| Q2 | Safe physical outputs | Defined level at power-up, after bus error and after expander reset; no glitch during init |
| Q3 | Portability | Adding a platform or chip requires no change in the portable core or the application |
| Q4 | Determinism and bounded resources | No heap after `init()`; every blocking call has a configured timeout |
| Q5 | Configurability | Every project setting comes from JSON; invalid configuration fails the build |
| Q6 | Testability | Core testable on host with mock port, bus and OSAL |
| Q7 | Efficiency | Native write is one register store when hardware allows; no bus traffic for unchanged state |

## 4. Verdict

The layered structure (pin handle, port, bus, OSAL), the patterns (Bridge, Adapter, Strategy,
Factory, Observer with deferred execution) and the decision to prefer OS expander drivers are
sound and meet Q3, Q6 and Q7.

As written across the three iterations, however, the design contained **2 critical, 11 major and
8 minor** defects, mainly in concurrency (Q1), physical safety (Q2) and internal consistency.
All of them have a resolution in this revision; the diagrams and ADRs already reflect the
corrected design. Implementation can start once the open questions in section 8 are answered.

## 5. Findings

### 5.1 Critical

| ID | Area | Finding | Concrete failure | Resolution |
|---|---|---|---|---|
| C-1 | State ownership | Iteration 1 gave `Output` its own mutex and `state_`; iteration 2 gave the port a shadow register and mutex. Two sources of truth, three lock levels. | `Output::toggle()` changed `state_` before the write and never reverted it on error: after one I2C NACK the pin's idea of its level is permanently inverted relative to the chip. | Pins are stateless handles. State exists only in the port; `toggle()` maps to `toggle_masked()` (hardware PTOR on native ports, shadow on expanders). [ADR-0001](adr/0001-layering-and-polymorphism.md) |
| C-2 | Locking API | `LockGuard` called `lock()` and ignored its return value, while the FreeRTOS `lock()` returns `Timeout`. | On timeout the code enters the critical region without the lock: concurrent read-modify-write of the shadow, wrong relay or LED state. | `ScopedLock` with `[[nodiscard]] owns()`; callers return `Error::Timeout`. Diagram `05`. [ADR-0002](adr/0002-concurrency-model.md) |

### 5.2 Major

| ID | Area | Finding | Resolution |
|---|---|---|---|
| M-1 | Polymorphism | Iteration 1 used a compile-time `GpioBackend` concept on pins, iteration 2 run-time `IGpioPort`. A board mixing native and expander pins needs a heterogeneous port collection, which the template approach cannot express. | Virtual dispatch at `IGpioPort` / `IBus` / `IExpanderChip`; link-time selection for OSAL and platform. [ADR-0001](adr/0001-layering-and-polymorphism.md) |
| M-2 | Initialisation | The `Output` constructor called `open()` (hardware access). Without exceptions the error was stored but never observable, and global objects construct before clocks are enabled. | Two-phase: constructor stores only; `init()` / `start()` return `Result`. |
| M-3 | OSAL completeness | Only the bare-metal OSAL had a critical section; there was no clock abstraction; context detection assumed `xPortIsInsideInterrupt()`, which exists only in some FreeRTOS ports (it does exist in the Cortex-M ports). | Full OSAL set on every target: `Mutex`, `ScopedLock`, `CriticalSection`, `Context`, `EventSignal`, `Clock`. `in_isr()` falls back to the CMSIS IPSR read on Cortex-M. Diagram `04b`. |
| M-4 | Shared-state ownership | Which execution context may touch which field was never specified; iteration 3 proposed a no-op mutex on bare-metal without proving that no ISR touches the protected data. | Per-field ownership table (section 6.3). Expander shadows are never touched from an ISR; the bare-metal completion ISR writes only an atomic status. Diagram `07`. |
| M-5 | Observer lifetime | `subscribe()` was callable at any time while the dispatcher iterates the table. | Subscriptions only before `start()`; the table is frozen; dispatch needs no lock. |
| M-6 | Zephyr deferral | Iteration 1 deferred to "a work item" without naming the queue. Expander reads on the shared system work queue delay every other system work item for the bus duration. | Dedicated work queue; stack size and priority come from configuration. |
| M-7 | Fail-safe semantics | `fail_safe` appeared in the JSON example without defined behaviour; the initialisation order could drive a pin before its value was set. | Write the output latch before the direction register: `PDOR` before `PDDR` on native MCXN947 GPIO (both registers verified, section 7), `OUTPUT` before `CONFIG` on PCA95xx. Re-apply after a detected chip reset. Diagram `07`. |
| M-8 | Unverified hardware claim | Iteration 3 stated that "older i.MX RT1050/1060" lack `DR_SET`/`DR_CLEAR`. This was not checked against reference manuals and must not be relied on. | Decide `caps().atomic_write` from vendor data (SDK feature macros or SVD), never from a part-name list. For this project it is verified: MCXN947 GPIO has `PSOR`/`PCOR`/`PTOR`. |
| M-9 | Multi-core | The MCXN947 is dual-core (the board target names `cpu0`; `SEMA42_0` and `MAILBOX` are present in the SVD). An expander shadow or a bus used from both cores is protected by no OSAL primitive. | One owning core per port and per bus (`owner_core`), checked by the generator; other cores request changes through IPC. Writes to `PSOR`/`PCOR` from both cores to different pins are safe at hardware level because only bits written as 1 change. |
| M-10 | Devicetree generation | Generating a Zephyr overlay can collide with board DTS nodes; this repository's overlay already modifies `&green_led`. | The generator emits only new expander nodes and `&label` references; it never redefines a board node. [ADR-0003](adr/0003-configuration-single-source-json.md) |
| M-11 | Responsibility boundary | Pin mux, clock gating and security attributes were never assigned. The MCXN947 GPIO has `PCNS`/`ICNS` (non-secure control) and `LOCK` registers, which decide whether the calling security state may write the pin at all. | Precondition of the library: board init (Zephyr `pinctrl`, MCUXpresso `BOARD_InitPins`, TrustZone setup) is complete. The library checks readiness and reports `NotReady`; it never changes mux or security settings. |

### 5.3 Minor

| ID | Finding | Resolution |
|---|---|---|
| m-1 | `std::pair<Error, Level>` return type; no uniform error channel. | Own `Result<T>` (C++17-compatible; `std::expected` needs C++23). |
| m-2 | C++ standard never fixed; concepts implied C++20. | C++17 baseline, aligned with MISRA C++:2023 (based on C++17). |
| m-3 | Only the port-lock timeout was specified, not the bus-lock timeout. | Both are configured; both return `Error::Timeout`. |
| m-4 | Lost-edge window for expander interrupts was described but not closed. | Edge-triggered native IRQ plus INT level re-check loop, bounded by `irq_max_reads`. Diagram `06`. |
| m-5 | String lookup of pin names on the hot path was possible. | Name lookup only before `start()`; afterwards handles only. |
| m-6 | Linux `i2c-dev` gives no protection between processes. | One owning process per device; the kernel driver is the default (`driver: "os"`). |
| m-7 | IRQ priority values were written as raw numbers. The MCXN947 implements 3 priority bits (`nvicPrioBits = 3`, 8 levels). | Configuration uses logical levels 0-7; the generator emits the shifted value and validates against `configMAX_SYSCALL_INTERRUPT_PRIORITY`. |
| m-8 | Diagram-level inconsistency between "native port" (iteration 1, OS driver) and "native port" (iteration 3, SDK HAL). | Glossary (section 11): *native port* means a controller driven through the OS driver or the SDK HAL, decided per target. |

### 5.4 Repository baseline (current code)

| ID | Location | Finding | Resolution |
|---|---|---|---|
| R-1 | `src/main.cpp` | When `led_is_ready_dt()` fails, `err` is set but the loop still calls `led_on_dt()` / `led_off_dt()` on a device that is not ready; their return values are ignored. | Return or halt on `-ENODEV`; check every return value. |
| R-2 | `src/main.cpp`, `prj.conf` | `<iostream>` with `CONFIG_REQUIRES_FULL_LIBCPP=y` links the full libstdc++ stream implementation, a large flash/RAM cost on an MCU. `CONFIG_LOG=y` is already enabled. | Use `LOG_*` or `printk`. |
| R-3 | `src/main.cpp` | `SLEEP_MSEC 25U` is a hard-coded setting. | Move to the JSON configuration (ADR-0003). |
| R-4 | `README.rst`, `sample.yaml` | Both describe the upstream `hello_cpp_world` sample on `qemu_riscv32`, not this project. | Replace with project documentation (Markdown). |
| R-5 | `mcux_include.json`, `.vscode/settings.json` | Absolute machine-specific paths (`/home/ubuntu/nxp_zephyr/...`) are version-controlled. | Move to `CMakeUserPresets.json` (CMake's designated non-versioned file) and user-level VS Code settings. |
| R-6 | `CMakePresets.json` | `CMAKE_*_OUTPUT_DIRECTORY` are set to `$env{binaryDir}`. That syntax reads an environment variable named `binaryDir`, which no preset defines, so the values expand to empty strings unless the shell sets it. To be confirmed with `cmake --preset debug`. | Remove the three entries or use the `binaryDir` preset field only. |
| R-7 | `.gitignore` | Build directories are `${fileDir}/${presetName}`; `debug/` is ignored, `release/` is not. | Ignore both, or build out of tree. |
| R-8 | `prj.conf` | `CONFIG_PWM=y` is enabled but not used by the current source. | Remove unless a PWM LED is planned. |

## 6. Revised architecture

### 6.1 Layers and responsibilities

| Layer | Elements | Owns | Must not |
|---|---|---|---|
| API | `GpioSystem` (Facade + Factory), `OutputPin`, `InputPin`, `EventDispatcher` | Lifecycle, subscriptions | Hold pin state, include OS headers |
| Port | `IGpioPort`, `ExpanderPort`, `IExpanderChip` implementations | Shadow registers, port mutex, fail-safe | Call user callbacks under lock |
| Bus | `IBus` implementations | Bus mutex, recovery | Know about pins |
| OSAL | `Mutex`, `ScopedLock`, `CriticalSection`, `Context`, `EventSignal`, `Clock` | Mapping to OS primitives | Allocate memory |
| Platform adapters | `McuxGpioPort`, `ZephyrGpioPort`, `LinuxGpiodPort`, bus adapters | SDK / driver calls | Contain business logic |
| Configuration | `IConfigSource`, generated tables, `JsonConfigSource` | Descriptors | Be edited by hand (generated parts) |

### 6.2 Design patterns

| Pattern | Where | Purpose |
|---|---|---|
| Bridge (two levels) | Pin to `IGpioPort` to `IBus` | Abstraction and implementation vary independently; MCP23017 and MCP23S17 share one chip strategy over different buses |
| Adapter | OSAL, bus adapters | Uniform interface over Zephyr, FreeRTOS, Linux and SDK APIs |
| Strategy | `IExpanderChip` | Chip framing and interrupt semantics; register maps as data |
| Facade + Factory | `GpioSystem` | One entry point; objects built from configuration |
| Observer with deferred execution | `EventDispatcher` + `EventSignal` | Interrupts become callbacks in a context that may block |
| RAII | `ScopedLock`, `CriticalSection` | Release on every path without exceptions |

### 6.3 Concurrency contract

Lock order: `ExpanderPort::mtx_` (1) then `IBus::mutex()` (2). Never reversed, never deeper.

**Operation by context**

| Operation | Task / thread | ISR | Before scheduler | Bare-metal main loop |
|---|---|---|---|---|
| Native `set` / `toggle` (`atomic_write`) | yes | yes | yes | yes |
| Native `set` (no `atomic_write`) | yes (CriticalSection) | yes (CriticalSection) | yes | yes (CriticalSection) |
| Expander `set` (`can_sleep`) | yes | `Error::IsrContext` | yes, blocking | yes, asynchronous via `poll()` or blocking (configured) |
| `configure()`, `subscribe()` | before `start()` only | no | yes | before `start()` only |
| Observer callback | event task / work queue | never | n/a | inside `poll()` |

**Shared-state ownership**

| State | Writers | Readers | Protection |
|---|---|---|---|
| `ExpanderPort::shadow_out_`, `shadow_dir_` | task contexts (RTOS, Linux); main loop (bare-metal) | same | port mutex; none needed on bare-metal (single context) |
| `ExpanderPort::shadow_in_` | event context | event context | port mutex |
| Bare-metal transfer status | completion ISR | `poll()` | `std::atomic<uint8_t>` store/load (lock-free on every Cortex-M) |
| Pending IRQ bits | ISR | event context | FreeRTOS task notification; Zephyr work submission; bare-metal `std::atomic<uint32_t>::fetch_or` (LDREX/STREX on Cortex-M33; CriticalSection on Cortex-M0+) |
| Subscription table | `subscribe()` before `start()` | event context | immutable after `start()` |
| Native output register | any context | any context | hardware `PSOR`/`PCOR`/`PTOR` when `atomic_write` |

### 6.4 Lifecycle

```
construct (no HW)  ->  init(cfg): ports init, fail-safe latch, direction, pins configured
                   ->  subscribe(...) (optional)
                   ->  start(): freeze subscriptions, enable IRQs, start event context
                   ->  run: set/toggle/read, poll() on bare-metal
                   ->  on fault: Error::Io returned, port resync / Faulted state (diagram 07)
```

### 6.5 Configuration keys introduced by this review

| Key | Purpose | Finding |
|---|---|---|
| `schema_version` | Compatibility check of configuration files | ADR-0003 |
| `ports[].driver` (`"os"` / `"lib"`) | Expander driver selection | ADR-0004 |
| `ports[].owner_core`, `buses[].owner_core` | Single-core ownership | M-9 |
| `pins[].fail_safe` (required for outputs) | Defined level at init and after reset | M-7 |
| `irq.priority` (logical level) | Converted by generator | m-7 |
| `event.work_queue.stack_size`, `event.work_queue.priority`, `event.task.*` | Event context sizing | M-6 |
| `timeouts.port_lock_ms`, `timeouts.bus_lock_ms`, `timeouts.bus_transfer_ms` | Bounded blocking | C-2, m-3 |
| `recovery.max_retries`, `irq_max_reads` | Bus recovery and IRQ loop bounds | diagram 07, m-4 |
| `buses[].mode` (`blocking` / `rtos` / `async`) | MCUX bus adapter mode | diagram 07 |

## 7. Verified MCXN947 facts used in this review

Source: `svd/MCXN947_cm33_core0.xml` in this repository.

| Fact | SVD evidence | Design consequence |
|---|---|---|
| Core Cortex-M33 r2p0, FPU, MPU | `<cpu><name>CM33</name><revision>r2p0</revision>` | LDREX/STREX and BASEPRI available |
| 3 NVIC priority bits | `<nvicPrioBits>3</nvicPrioBits>` | 8 logical priority levels (m-7) |
| GPIO0-GPIO5 with `PDOR` 0x40, `PSOR` 0x44, `PCOR` 0x48, `PTOR` 0x4C, `PDIR` 0x50, `PDDR` 0x54 | GPIO0 base 0x40096000, GPIO1 base 0x40098000 | `atomic_write = true`; latch-before-direction possible (M-7) |
| Two IRQ lines per GPIO port (GPIO0: IRQ 17 and 18) | `<interrupt>` entries `GPIO00`, `GPIO01` | Expander INT pins can be routed to their own IRQ line and priority |
| `ICR0`-`ICR31` per-pin interrupt control, `ISFR` status | GPIO register list | Edge selection and flag clear per pin (diagram 06) |
| `PCNS`, `ICNS`, `LOCK` registers | GPIO register list | Security attribution and locking are a board-init precondition (M-11) |
| `SEMA42_0`, `MAILBOX` present | peripheral list | Dual-core coordination exists in hardware (M-9) |

## 8. Risks and open questions

| ID | Question / risk | Impact if unresolved |
|---|---|---|
| Q-A | Will `cpu1` ever drive GPIO or share an I2C bus with `cpu0`? | If yes, IPC design or SEMA42-based sharing is needed (M-9). |
| Q-B | Which expander part numbers are on the real hardware? | Decides `driver: "os"` vs `"lib"` per port (ADR-0004). |
| Q-C | Must the bare-metal build stay responsive during expander transfers? | Asynchronous engine (diagram 07) vs blocking with timeout. |
| Q-D | Is a functional-safety standard applicable (IEC 61508, ISO 26262)? | Enforcement level of MISRA C++:2023, fail-safe verification evidence. |
| Risk | FreeRTOS priority inheritance is simplified for nested mutexes. | Mitigated by maximum nesting depth 2 and fixed order. |
| Risk | Pulses shorter than one bus read are invisible through an expander. | Physical limit; documented; time-critical inputs must be native. |

## 9. Recommended implementation order

1. Repository baseline fixes R-1 to R-8.
2. `config/gpio.schema.json` and `tools/gpio_codegen.py` with validation tests.
3. Portable core with mock port, bus and OSAL; host unit tests (GoogleTest) under ThreadSanitizer.
4. Zephyr adapters on FRDM-MCXN947 (existing toolchain in this repository).
5. MCUXpresso adapters: FreeRTOS first, then bare-metal (blocking, then asynchronous).
6. Linux adapters (libgpiod v2).

## 10. Diagram index

| File | View | Notation |
|---|---|---|
| [`01_c4_context.puml`](diagrams/01_c4_context.puml) | System context | C4 level 1 |
| [`02_c4_container.puml`](diagrams/02_c4_container.puml) | Build-time and run-time units | C4 level 2 |
| [`03_c4_component.puml`](diagrams/03_c4_component.puml) | Components of the run-time library | C4 level 3 |
| [`04_class_core.puml`](diagrams/04_class_core.puml) | Portable core structure | UML class |
| [`04b_class_platform_osal.puml`](diagrams/04b_class_platform_osal.puml) | Platform realizations and OSAL mapping | UML class |
| [`05_seq_expander_write.puml`](diagrams/05_seq_expander_write.puml) | Expander write: locks, commit, failure | UML sequence |
| [`06_seq_expander_irq.puml`](diagrams/06_seq_expander_irq.puml) | Expander interrupt: deferral and dispatch | UML sequence |
| [`07_state_async_bus.puml`](diagrams/07_state_async_bus.puml) | Bare-metal asynchronous transfer engine | UML state machine |
| [`08_deployment_variants.puml`](diagrams/08_deployment_variants.puml) | Build variants per target | UML deployment |
| [`09_activity_config_pipeline.puml`](diagrams/09_activity_config_pipeline.puml) | Configuration validation and generation | UML activity |

## 11. Glossary

| Term | Meaning |
|---|---|
| Native port | A GPIO controller on the SoC, driven through the OS driver (Linux, Zephyr) or the SDK HAL (FreeRTOS, bare-metal). |
| Expander port | A GPIO controller reached over I2C or SPI. |
| Shadow register | RAM copy of a write-only or slow-to-read hardware register; the single source of truth for output state. |
| Event context | The non-ISR context that performs deferred interrupt work: FreeRTOS task, Zephyr work queue, Linux thread, or bare-metal `poll()`. |
| `can_sleep` | Port capability: operations may block and must not be called from an ISR. |
| `atomic_write` | Port capability: single-pin set/clear/toggle is one hardware store without read-modify-write. |
| Fail-safe level | Output level applied at initialisation and after a detected device reset. |

# Portable GPIO Library: A Step-by-Step Guide

This guide teaches the design from first principles. It assumes you know basic C++ and what a
GPIO pin, an interrupt and a mutex are. Every other term is defined the first time it appears
and again in the [glossary](#13-glossary).

Read it in order. Each section builds on the previous one. The reference documents
([DESIGN_REVIEW.md](DESIGN_REVIEW.md) and the [ADRs](adr/)) contain the full findings and the decisions;
this guide explains **why** they look the way they do.

| Section | You will learn |
|---|---|
| 1 | The problem the library solves |
| 2 | The five building blocks and what each one is responsible for |
| 3 | A write to a native pin, step by step |
| 4 | A write to an expander pin, step by step |
| 5 | The three concurrency bugs the design prevents, with timelines |
| 6 | Interrupts: why the ISR does almost nothing |
| 7 | What changes between Linux, Zephyr, FreeRTOS and bare-metal |
| 8 | Configuration without hard-coded values |
| 9 | Error handling and safe outputs |
| 10 | The design patterns, and the problem each one solves |
| 11 | How to add a new chip or a new platform |
| 12 | Self-check questions with answers |
| 13 | Glossary |

---

## 1. The problem

One line of application code must work on four execution environments:

```cpp
led.set(Level::High);
```

The pin behind that line can be of two very different kinds:

| | Native pin | Expander pin |
|---|---|---|
| Where it is | On the microcontroller (for example MCXN947 GPIO0 pin 10) | On an external chip (for example PCA9555) reached over I²C or SPI |
| How it is written | One store to a hardware register | A bus transfer of several bytes |
| Time | Nanoseconds | About 100 µs at 400 kHz I²C |
| Can it fail? | No | Yes: no acknowledge (NACK), timeout, bus stuck |
| Can it block the caller? | No | Yes: it waits for the bus |
| Can it be called from an interrupt handler? | Yes | **No**, because it can wait |

The four environments:

| Environment | Execution contexts that exist |
|---|---|
| Linux | threads |
| Zephyr | threads and interrupt handlers (ISRs) |
| FreeRTOS | tasks and ISRs |
| Bare-metal | one main loop and ISRs |

**Goal:** the application does not know which kind of pin or which environment it runs on.
**Constraint:** the library must not hide the differences that matter. An expander write *can*
fail and *can* wait, and the API must say so.

## 2. The five building blocks

Diagram: [`10_overview_simple.puml`](diagrams/10_overview_simple.puml). Render it with the command in the [README](README.md).
The text diagram below is the same picture:

```
Application ──► Pin ──► Port ──► Hardware          (native pin: one register store)
                         │
                         └─────► Bus ──► Hardware  (expander pin: I²C / SPI transfer)

OSAL (mutex, ISR check, event signal) is used by Port and Bus.
```

| Block | Responsibility (one sentence) | Holds state? | Holds a lock? |
|---|---|---|---|
| **Pin** (`OutputPin`, `InputPin`) | Remembers which port and which bit it is | No | No |
| **Port** (`IGpioPort`) | Represents one GPIO controller and **owns the state of all its pins** | Yes | Yes (port lock) |
| **Bus** (`IBus`) | Moves bytes over one physical I²C or SPI bus | No | Yes (bus lock) |
| **OSAL** | Gives the same mutex, ISR check and event signal on every OS | No | n/a |
| **Platform adapter** | Calls the real driver: libgpiod, Zephyr API, MCUXpresso SDK | No | No |

### The one rule that prevents most bugs

> **Mutable state lives in exactly one place: the Port.**

An early version of this design had the pin keep its own copy of the level *and* the port keep a
shadow copy. After one failed I²C write the two copies disagreed forever (review finding C-1).
Two copies of the same truth is the root of that bug, so the design allows only one.

A pin is therefore just a small value:

```cpp
// Illustrative sketch, not final code.
class OutputPin {
    IGpioPort* port_;     // which controller
    uint32_t   mask_;     // which bit(s): 1u << index
    bool       active_low_;
public:
    Result<void> set(Level l);   // forwards to port_->write_masked(...)
    Result<void> toggle();
};
```

You can copy it freely. It has no lock because it has nothing to protect.

## 3. Walkthrough A: writing a native pin

Target: FRDM-MCXN947. Its GPIO has these registers (verified in the SVD file in this repository):

| Register | Offset | Effect of writing a 1 to bit *n* |
|---|---|---|
| `PDOR` | 0x40 | Sets the output level of pin *n* to the written value (all 32 bits at once) |
| `PSOR` | 0x44 | Sets pin *n* high; other pins unchanged |
| `PCOR` | 0x48 | Sets pin *n* low; other pins unchanged |
| `PTOR` | 0x4C | Toggles pin *n*; other pins unchanged |

Because `PSOR`, `PCOR` and `PTOR` change only the bits you write as 1, **setting one pin is a single store
and cannot disturb another pin**. Compare with the alternative:

```
Read-modify-write on PDOR (what you must do if only PDOR exists):
  1. read PDOR            -> 0b0000
  2. set bit 3 in a copy  -> 0b1000
  3. write PDOR
If an interrupt handler changes bit 1 between step 1 and step 3,
step 3 overwrites it. The pin change is silently lost.
```

So the Port reports a capability:

```
caps().atomic_write = true   // PSOR/PCOR/PTOR exist: no lock needed for set/clear/toggle
```

and the native write path is: `set(High)` → `PSOR = (1u << index)`. It works from any task, from an
interrupt handler, and before the scheduler starts. This is the fast path; nothing else in the
library is allowed to slow it down.

If a chip had only `PDOR`, the Port would report `atomic_write = false` and protect the
read-modify-write with a short critical section. The library decides this from vendor data, not from
a part-name list.

## 4. Walkthrough B: writing an expander pin

Target: PCA9555, a 16-pin I²C expander. Its output pins are controlled through two 8-bit output
registers. **You cannot write a single bit; you must write a whole byte.**

### 4.1 Why the Port needs a shadow register

To set bit 3 of the first byte, you must also send the other seven bits. Where do they come from?

| Option | Problem |
|---|---|
| Read the byte from the chip, change bit 3, write it back | Doubles bus traffic and is itself a read-modify-write: another task can change the byte in between |
| Keep a **shadow register**: a RAM copy of what the chip currently outputs | None, as long as the copy is protected and kept correct |

The design uses the shadow.

### 4.2 The sequence

```
Application task            OutputPin          ExpanderPort                       I²C bus / chip
      |  set(High)             |                     |                                 |
      |----------------------->|  write_masked(bit3) |                                 |
      |                        |-------------------->|                                 |
      |                        |      [1] take PORT lock (with timeout)               |
      |                        |      [2] next = (shadow & ~mask) | (value & mask)    |
      |                        |      [3] next == shadow ?  yes -> return OK, no bus   |
      |                        |      [4] take BUS lock (with timeout)                |
      |                        |      [5] send register + 2 bytes ------------------->|
      |                        |                                  <---- ACK / NACK ---|
      |                        |      [6] ACK  -> shadow = next                       |
      |                        |          NACK -> shadow unchanged, resync_needed     |
      |                        |      [7] release BUS lock, then PORT lock            |
      |<-----------------------|---- Result (Ok / Io / Timeout) --------------------- |
```

Full version with all failure branches: [`05_seq_expander_write.puml`](diagrams/05_seq_expander_write.puml).

Why each step exists:

| Step | Reason |
|---|---|
| 1. Port lock | The shadow is shared by every task that writes any pin of this chip. |
| 1, 4. Timeouts | A stuck bus must not block a task forever. The wait ends with `Error::Timeout`. |
| 3. Skip if unchanged | No bus traffic when nothing changes. |
| 4. Bus lock | The same I²C bus may carry a sensor or an EEPROM. Two transfers must not interleave. |
| 6. Commit **after** ACK | The shadow must never claim a state the chip does not have. |
| 6. `resync_needed` | After an error the real chip state is unknown. The next service step reads the chip back and repairs the shadow. |
| 7. Release in reverse order | Standard practice; combined with the fixed acquisition order it rules out deadlock (section 5.2). |

### 4.3 Writing many pins at once

The Port's primitive is `write_masked(mask, value)`. If you set three LEDs that live on the same
byte, one call with `mask = 0b00010101` costs one transfer, not three. This is why the primitive
works on a mask and not on a single bit.

## 5. Concurrency: three bugs, three rules

"Thread-safe" is too vague to design against. Each rule below is aimed at one concrete failure.

### 5.1 Bug: the lost update

Two tasks write different pins of the same expander. Without a lock:

```
shadow = 0b0000_0000

Task A (LED on bit 1)                 Task B (relay on bit 4)
read shadow      -> 0b0000_0000
                                      read shadow      -> 0b0000_0000
compute          -> 0b0000_0010
                                      compute          -> 0b0001_0000
write chip       <- 0b0000_0010
                                      write chip       <- 0b0001_0000   (A's LED is off again)
shadow = 0b0000_0010
                                      shadow = 0b0001_0000
```

The LED never turned on, and no error was reported.

**Rule 1: the shadow is changed only while holding the Port lock, and the lock covers read, compute, bus write and commit.**

### 5.2 Bug: the deadlock

```
Task A: holds PORT lock, wants BUS lock
Task B: holds BUS  lock, wants PORT lock
-> both wait forever
```

**Rule 2: locks are always taken in the same order: Port lock first, Bus lock second. Never the reverse, never a third level.**

How the code guarantees it: only `ExpanderPort` ever takes both locks, and it always takes them in that order.
Nothing outside the Port can reach the Bus lock while holding the Port lock in the other order.

### 5.3 Bug: blocking inside an interrupt handler

An ISR interrupts a task. If the ISR tries to take a mutex held by the task it interrupted,
the ISR waits for the task, and the task cannot run until the ISR returns. Nothing can ever finish.
Even when no such cycle exists, an expander write inside an ISR would hold off every lower-priority
interrupt and task for the full I²C transfer.

**Rule 3: nothing that can wait runs in an ISR.** The library enforces this:

```
expander.set() called while osal::Context::in_isr() is true  ->  returns Error::IsrContext
```

A wrong call becomes an immediate, reported error, not a rare hang in the field.

### 5.4 Two more rules that remove whole classes of bugs

| Rule | What it removes |
|---|---|
| **Callbacks run with no library lock held.** | A callback that calls `set()` again would otherwise try to take a lock it already holds. |
| **Subscriptions are registered before `start()`; the table is frozen afterwards.** | The dispatcher can walk the table without a lock, because nobody can modify it. |

### 5.5 Two primitives, two meanings

| Primitive | Can wait? | Protects against | Use it for |
|---|---|---|---|
| `osal::Mutex` | Yes | another **task** | Port lock, Bus lock |
| `osal::CriticalSection` | **Never** | another **ISR** (it briefly disables interrupts) | The few bytes shared with an ISR; never around a bus transfer |

A mutex with priority inheritance is required (Zephyr `k_mutex`, a FreeRTOS **mutex**, not a binary
semaphore). Otherwise a low-priority task holding the lock can be starved by a medium-priority task
while a high-priority task waits for it (priority inversion).

## 6. Interrupts: the ISR does almost nothing

A button on an expander pulls the chip's `INT` line low. That line is wired to a **native** pin of
the microcontroller. The path:

```
 [ISR: very short]                          [event context: may wait]
 native GPIO interrupt                      task / work-queue thread / poll()
   1. clear the interrupt flag               4. lock Port, read the chip's input register over I²C
   2. raise a signal bit ----------------->  5. changed = new_inputs XOR old_inputs
   3. return                                 6. unlock, then call the subscribers' callbacks
```

Full version: [`06_seq_expander_irq.puml`](diagrams/06_seq_expander_irq.puml).

Why this split:

| Question | Answer |
|---|---|
| Why not read the chip inside the ISR? | The read is an I²C transfer; it waits (section 5.3). |
| What is the "signal"? | `osal::EventSignal`. FreeRTOS: a task notification. Zephyr: a work item on a dedicated queue. Bare-metal: a flag that `poll()` checks. Linux: a dedicated edge-event thread. |
| Why can callbacks do anything? | They run in the event context, which is allowed to wait and to call `set()`. |

### 6.1 The lost-edge problem and its fix

On a PCA9555 the `INT` line stays low until you **read the input register**. Suppose the INT pin
is configured to interrupt only on a falling edge:

```
INT:  ‾‾‾\______/‾‾‾‾          (1) falling edge -> ISR -> event context starts reading
          \  second input change while the read is in progress
INT stays low the whole time -> no new falling edge -> the second change is never reported
```

Fix: after each read, the event context checks the level of the `INT` pin. If it is still low, it
reads again. The loop is limited by `irq_max_reads` (a configuration value), so a stuck line cannot
trap the system.

### 6.2 A physical limit that code cannot fix

An input pulse shorter than one bus read can come and go without ever being seen through an
expander. Native pins do not have this limit. Therefore **time-critical inputs must be native pins**.
The configuration generator checks the roles you assign (section 8) to catch this early.

## 7. What changes per platform

The portable core never contains `#ifdef` for a platform. The platform-specific code is selected
at build time, by CMake, from separate directories.

| Need | Linux | Zephyr | FreeRTOS | Bare-metal |
|---|---|---|---|---|
| Native GPIO | libgpiod v2 | `gpio_dt_spec` API | MCUXpresso `fsl_gpio` | MCUXpresso `fsl_gpio` |
| I²C / SPI | `/dev/i2c-N`, spidev | `i2c_dt_spec`, `spi_dt_spec` | `fsl_lpi2c_freertos` | `fsl_lpi2c` (blocking or non-blocking) |
| Mutex | `std::mutex` | `k_mutex` | static FreeRTOS mutex | no-op: only one non-ISR context exists |
| Critical section | `std::mutex` | `k_spin_lock` | `taskENTER_CRITICAL` | save + disable + restore PRIMASK/BASEPRI |
| "Am I in an ISR?" | always false | `k_is_in_isr()` | `xPortIsInsideInterrupt()` | `__get_IPSR() != 0` |
| Deferred work | thread | work queue | task + notification | `poll()` in the main loop |
| Configuration | JSON read at start | JSON turned into devicetree + C++ tables | JSON turned into C++ tables | JSON turned into C++ tables |

### 7.1 Use the OS driver when one exists

For an expander chip that Linux or Zephyr already supports (for example PCA953x/PCA95xx), the kernel
or Zephyr driver already implements the shadow, the locks and the interrupt handling. The library
then sees it as an ordinary controller and does **not** use its own `ExpanderPort`. The library's own
expander code is used on FreeRTOS and bare-metal, where no driver exists, and for chips without a driver
([ADR-0004](adr/0004-expander-driver-selection.md)).

### 7.2 Bare-metal in detail

Bare-metal has no scheduler, so the problems change:

| Situation | Consequence |
|---|---|
| Main loop vs main loop | No preemption, so no lock is needed. `osal::Mutex` does nothing. |
| Main loop vs ISR | A mutex cannot help (the ISR cannot wait for the main loop). The design avoids sharing: the expander shadow is touched only by the main loop. |
| I²C is slow | Two modes, chosen in the configuration: **blocking with timeout**, or **asynchronous**. |

Asynchronous mode (state machine in [`07_state_async_bus.puml`](diagrams/07_state_async_bus.puml)):

```
write_masked()  : main loop updates the shadow, marks the port "dirty"        (no bus access)
poll()          : main loop, if the bus is idle and dirty: start a non-blocking transfer
completion ISR  : stores Done or Error into one atomic status variable        (nothing else)
poll()          : sees Done -> idle, or sees Error -> bus recovery
```

Several `write_masked()` calls between two `poll()` calls merge into one transfer.
The ISR writes only the atomic status; the main loop owns everything else. That is why no lock is needed.

### 7.3 Multi-core (MCXN947)

The MCXN947 has two cores. A port or bus used from both cores would have no protection from any
OSAL primitive, because mutexes of one core's RTOS are invisible to the other. The rule: **every
port and every bus has exactly one owning core** (`owner_core` in the configuration, checked at
build time). The other core asks the owner to act.

## 8. Configuration without hard-coded values

Nothing that describes a *project* appears in the C++ source: no pin numbers, no I²C addresses,
no timeouts, no stack sizes, no priorities. They are in `config/gpio.json`.

```
config/gpio.json ──► tools/gpio_codegen.py ──► checks ──► generated files ──► compiler
 (you edit this)      (runs at build time)     fail the    gpio_config.hpp
                                               build on    (+ devicetree overlay on Zephyr)
                                               any error
```

Diagram: [`09_activity_config_pipeline.puml`](diagrams/09_activity_config_pipeline.puml).

Example (shape only; the authoritative schema will live in `config/gpio.schema.json`):

```json
{
  "schema_version": 1,
  "ports": [
    { "id": "soc_gpio0", "type": "native",  "owner_core": 0 },
    { "id": "exp_a",     "type": "pca9555", "driver": "lib", "bus": "i2c1",
      "address": 32, "can_sleep": true, "int_pin": "soc_gpio1:12", "owner_core": 0 }
  ],
  "pins": [
    { "name": "relay_1", "port": "exp_a", "index": 3, "dir": "out",
      "active_low": false, "initial": "low", "fail_safe": "low" }
  ],
  "timeouts": { "port_lock_ms": 10, "bus_lock_ms": 20 }
}
```

Checks the generator performs (each one is a bug that would otherwise appear on the device):

| Check | Prevents |
|---|---|
| Pin used twice | Two owners of one pin |
| Index larger than port width | Writing a bit that does not exist |
| An expander pin used for a timing-critical role | Jitter of 100 µs or more in a fast signal |
| Interrupt priority not allowed by the RTOS | Kernel assertion or silent corruption (FreeRTOS requires ISRs that call `...FromISR()` to have a numerically high-enough priority) |
| Bus referenced but not defined | Null access at start-up |
| Output without `fail_safe` | An output with no defined safe level |
| Port or bus with two owning cores | Unprotected shared state |

## 9. Error handling and safe outputs

### 9.1 No exceptions

Embedded builds usually use `-fno-exceptions`. Every operation returns a `Result<T>`: either a value
or an `Error`. You must look at it.

| Error | Meaning | Typical caller reaction |
|---|---|---|
| `Ok` | Done | continue |
| `InvalidArg` | Bad argument | fix the code |
| `NotReady` | Hardware or driver not initialised | check board start-up |
| `Timeout` | A lock or transfer did not finish in the configured time | retry or degrade |
| `Io` | The bus reported NACK or a bus fault | the library already scheduled a resync |
| `IsrContext` | Sleeping operation called from an ISR | programming error: move the call to a task |
| `WouldBlock` | Operation would have to wait but waiting is not allowed here | try later |

### 9.2 Safe outputs

An expander loses its state when it resets (brown-out) and then usually comes up with all pins as
inputs, which means floating outputs. A relay or heater must not depend on that.

| Mechanism | What it does |
|---|---|
| `fail_safe` level per output | The level the pin must have at start and after a reset |
| Latch before direction | At init, write the output latch with the fail-safe level **first**, then switch the pin to output. The pin never shows a wrong level, not even for a microsecond |
| `resync()` | After any error, read the chip back; if it was reset, re-apply the fail-safe levels |
| `Faulted` state | After too many failed recoveries every call returns `Io`; the application decides (degrade, reset, alarm) |

The recovery state machine is in [`07_state_async_bus.puml`](diagrams/07_state_async_bus.puml).

## 10. The design patterns and the problem each one solves

Patterns are names for solutions that were found repeatedly. Learn each by the problem it removes.

| Pattern | Problem it solves here | Where |
|---|---|---|
| **Bridge** | "The pin API and the hardware vary independently. Without separation you need one class per (API variant × hardware variant)." | Pin → Port → Bus |
| **Adapter** | "Zephyr, FreeRTOS, Linux and the SDK offer different APIs for the same idea (a mutex, an I²C transfer). Our code wants one API." | OSAL, bus adapters |
| **Strategy** | "MCP23017 and PCA9555 differ only in framing and interrupt behaviour; the rest of the expander logic is identical." | `IExpanderChip` |
| **Facade** | "The application should not wire ports, buses and chips together itself." | `GpioSystem` |
| **Factory** | "Objects must be built from the configuration, not from constants in code." | `GpioSystem::init()` |
| **Observer** | "Several parts of the program want to react to the same input change." | `EventDispatcher` |
| **Deferred execution** | "The interrupt handler cannot do the real work." | `EventSignal` + event context |
| **RAII** | "A lock must be released on every return path, and there are no exceptions to unwind." | `ScopedLock`, `CriticalSection` |
| **Shadow state** | "The hardware register is write-only, slow or byte-wide." | `ExpanderPort` |

**Virtual functions or templates?** Virtual (run-time) dispatch is used at the few boundaries
where different kinds of objects must live in one array (`IGpioPort`, `IBus`, `IExpanderChip`).
One indirect call is negligible next to a bus transfer. Link-time selection (CMake choosing a
source directory) is used for OSAL and platform code, which costs nothing at run time. See
[ADR-0001](adr/0001-layering-and-polymorphism.md).

## 11. How to extend the library

| Task | What you add | What stays unchanged |
|---|---|---|
| New expander chip (for example another I²C chip with the same two-register layout) | A register-map JSON file under `config/chips/` and, if its framing or interrupt behaviour differs, one `IExpanderChip` class | Pin, Port logic, Bus, OSAL, the application |
| New bus controller | One `IBus` class | Everything above it |
| New silicon (for example another MCU family) | One native-port class and bus classes in `adapters/<silicon>/` | Core, application |
| New operating system | A new `osal/<os>/` directory (six small classes) | Core, application |
| New pin in the product | Lines in `gpio.json` | All C++ |

## 12. Self-check questions

Try to answer before reading the answers.

1. Why does `OutputPin` have no mutex?
2. Why is the shadow register updated only after the bus acknowledges?
3. Two tasks acquire the Bus lock and the Port lock. What single rule prevents deadlock?
4. A button interrupt arrives from an expander. Why can the ISR not read the input register?
5. A native `set()` on MCXN947 needs no lock. Which hardware feature makes this possible?
6. On bare-metal, why is `osal::Mutex` allowed to do nothing, and what is the one thing that is **not** allowed instead?
7. A pulse of 20 µs must be detected. Can it come through an expander?
8. Why must the output latch be written before the direction register at start-up?

<details>
<summary>Answers</summary>

1. It has no state to protect. All mutable state belongs to the Port; two copies of one fact is what caused finding C-1.
2. If the write fails, the chip did not change. A shadow that was updated early would claim a state the chip does not have, and later writes would be computed from a wrong base.
3. Always take the Port lock first and the Bus lock second, never in the other order, and never a third level.
4. Reading the register is an I²C transfer, which can wait. An ISR must not wait. It only signals; an event context does the read.
5. The `PSOR`, `PCOR` and `PTOR` registers change only the bits written as 1, so one store changes one pin without a read-modify-write.
6. There is only one non-ISR context, so tasks cannot contend. Instead, the design forbids sharing the shadow with an ISR; the ISR only writes an atomic status flag.
7. No. A pulse shorter than one bus read can be missed entirely. Use a native pin.
8. The pin is an input after reset. If direction were switched to output first, it would drive whatever the latch contains (often 1) until the fail-safe value is written. Writing the latch first means the first output level is already the safe one.

</details>

## 13. Glossary

| Term | Meaning |
|---|---|
| **ACK / NACK** | Acknowledge / not-acknowledge: the I²C receiver's answer to each byte. A NACK is reported as `Error::Io`. |
| **Atomic (hardware)** | An operation that cannot be interrupted half-way; here, a single register store. |
| **`atomic_write`** | Port capability: single-pin set/clear/toggle is one store, no read-modify-write. |
| **Bare-metal** | Firmware without an operating system or scheduler: a main loop plus ISRs. |
| **Bus** | The wires and controller for I²C or SPI. Several devices can share one. |
| **`can_sleep`** | Port capability: operations may wait, so they must not run in an ISR. |
| **Critical section** | A short region in which interrupts are disabled so nothing can interrupt it. Never waits. |
| **Deadlock** | Two or more tasks each hold something the other needs and wait forever. |
| **Event context** | The non-ISR place where deferred interrupt work runs: task, work-queue thread, Linux thread or `poll()`. |
| **Expander** | An external chip that provides extra GPIO pins over I²C or SPI. |
| **Fail-safe level** | The level an output must have at start-up and after a reset. |
| **ISR** | Interrupt service routine: the handler the CPU runs when a hardware interrupt fires. |
| **Latch (output)** | The register that holds the level a pin drives. |
| **Lost update** | Two read-modify-write sequences overlap and one result is overwritten. |
| **Mask** | A bit pattern selecting which pins an operation affects. |
| **Mutex** | A lock that makes other tasks wait. Cannot be used in an ISR. |
| **OSAL** | Operating-system abstraction layer: the same mutex, ISR check and event signal on every OS. |
| **Port** | One GPIO controller (native or expander); owner of its pins' state. |
| **Priority inheritance** | A mutex feature: the lock holder temporarily runs at the priority of the highest task waiting for it. |
| **Priority inversion** | A high-priority task waits for a low-priority task that a medium-priority task keeps from running. |
| **Result&lt;T&gt;** | A return type holding either a value or an `Error`; used instead of exceptions. |
| **Shadow register** | A RAM copy of a hardware register's value; the single source of truth for output state. |
| **Two-phase initialisation** | Constructors do nothing with hardware; a separate `init()` returns a `Result`. |

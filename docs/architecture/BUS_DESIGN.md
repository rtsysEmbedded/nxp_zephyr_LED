# Portable I2C / SPI Library: Design and Design Review

| Item | Value |
|---|---|
| Version | 1.0 (draft for review) |
| Date | 2026-10-17 |
| Scope | Portable I2C and SPI controller library (hardware peripheral and GPIO bit-bang), its register layer, and its integration with the GPIO library |
| Reference target | FRDM-MCXN947 (`frdm_mcxn947/mcxn947/cpu0`), Arm Cortex-M33 |
| Decisions | [ADR-0005](adr/0005-bus-layering-controller-device-register.md), [ADR-0006](adr/0006-hardware-vs-bitbang-selection.md), [ADR-0007](adr/0007-bus-transactions-locking-recovery.md), [ADR-0008](adr/0008-unified-hal-initialisation-order.md) |
| Tutorial | [BUS_GUIDE.md](BUS_GUIDE.md) (read this first if the protocols are new to you) |
| GPIO design | [DESIGN_REVIEW.md](DESIGN_REVIEW.md), [GUIDE.md](GUIDE.md) |

---

## 1. Purpose and scope

The GPIO design needed "a bus" only to reach expander chips. This document specifies a complete,
portable I2C and SPI library that any chip driver can use, with two kinds of implementation behind
one interface:

- **hardware**: the bus is driven by a peripheral (MCXN947: LPI2C or LPSPI inside an LP_FLEXCOMM instance), by the Zephyr driver, or by the Linux kernel;
- **bit-bang**: software creates the bus waveform by setting and reading GPIO pins.

It follows the same process as the GPIO design: explicit quality goals, layered architecture, ADRs for
every decision, C4 and UML views, hardware facts verified against vendor data, and a review section
listing every problem found while designing, with its resolution.

Out of scope for version 1: I2C target (slave) mode, 10-bit I2C addresses, I2C multiplexers,
dual/quad SPI, I3C, FlexIO-based emulation, DMA on Linux.

## 2. Method and tools

Identical to the GPIO design ([DESIGN_REVIEW.md](DESIGN_REVIEW.md) section 2), plus:

| Added method | Used for | Artefact |
|---|---|---|
| UML timing diagrams | Bit-level protocol behaviour that sequence diagrams cannot show | `19`, `20` |
| Failure-mode analysis (FMEA-style table) | Every way a bus transfer can fail, how it is detected and what the library does | section 7.2 |
| Protocol specification check | Bit-bang timing against NXP UM10204 (I2C) and the SMBus timeout rule | sections 5.2, 7.3 |

## 3. Requirements and quality goals

### 3.1 Functional requirements

| ID | Requirement |
|---|---|
| F-1 | I2C controller mode, 7-bit addresses, Standard-mode (100 kHz), Fast-mode (400 kHz), Fast-mode Plus (1 MHz) where the implementation allows it |
| F-2 | I2C transfers made of several messages joined by repeated START (write-then-read register access) |
| F-3 | SPI controller mode 0-3, MSB- or LSB-first, 1-32 bit words, full duplex |
| F-4 | SPI chip select by hardware (peripheral PCS line) or by any native GPIO pin, with configurable setup, hold and inactive times |
| F-5 | Several devices per bus with different SPI settings; one shared bus speed for I2C |
| F-6 | Register access helpers: I2C 8- or 16-bit register addresses; SPI read-flag framing and opcode framing |
| F-7 | Same interface for hardware and bit-bang implementations on all four platforms |
| F-8 | Bus recovery for a stuck I2C bus |
| F-9 | Everything above is selected and parameterised from JSON; nothing project-specific in source code |

### 3.2 Quality goals (in priority order)

| # | Goal | Measurable criterion |
|---|---|---|
| Q1 | Correct under concurrency | No interleaving of two transactions on one bus; no deadlock (two-level lock hierarchy); no blocking call in ISR |
| Q2 | Protocol conformance | Bit-bang I2C meets UM10204 minimum timings (tLOW, tHIGH, setup/hold); SPI sampling edges match the configured mode |
| Q3 | Recoverability | A stuck I2C bus is detected within the configured timeout and recovered or reported as `Faulted` |
| Q4 | No silent data corruption | No automatic retries; NACK and bus faults are distinct errors; SPI identity checks required in drivers |
| Q5 | Portability | Adding a platform, peripheral or chip requires no change in core or chip drivers |
| Q6 | Bounded blocking | Every wait (lock, transfer, clock stretching) has a configured timeout |
| Q7 | Configurability and validation | Invalid wiring, speeds or lock-hierarchy violations fail the build |
| Q8 | Testability | Core and bit-bang controllers testable on the host against simulated targets |

## 4. Architecture

### 4.1 Layers

| Layer | Elements | Responsibility | Owns |
|---|---|---|---|
| Register | `IRegisterDevice`, `I2cRegisterDevice`, `SpiRegisterDevice`, `ISpiFraming` | Turn "read register R, N bytes" into bus bytes | nothing (stateless) |
| Device | `I2cDevice`, `SpiDevice`, `I2cTransaction`, `SpiTransaction` | Address or chip select, per-device settings, transaction scope | nothing; Transaction holds the bus lock while alive |
| Controller | `II2cController`, `ISpiController` | One physical bus: waveforms, FIFOs, timeouts, recovery | bus mutex, peripheral or bit-bang pins |
| Implementation | hardware adapters, `BitBangI2c`, `BitBangSpi`, `IDelay` | Platform or software specifics | peripheral state |
| Shared | OSAL, `Result`/`Error`, configuration, board hooks | Same as GPIO | - |

Diagrams: overview `11`, components `12`, classes `13` and `14`.

### 4.2 Why three layers and not one

A single "bus" object would mix three concerns that change for different reasons (ADR-0005):

| Change | Layer affected |
|---|---|
| New microcontroller or bit-bang instead of peripheral | Controller only |
| Another chip on the same bus, other address or SPI mode | Device only (configuration) |
| New chip family with different register framing | Register layer only (strategy + chip JSON) |

### 4.3 Design patterns

| Pattern | Where | Problem solved |
|---|---|---|
| Bridge | Device → Controller | Devices and bus implementations vary independently |
| Strategy | `ISpiFraming`, `IDelay` | Chip framing and timing source vary without touching the protocol code |
| Adapter | Hardware adapters | Linux ioctl, Zephyr API and MCUXpresso SDK become one interface |
| RAII (scoped lock) | `I2cTransaction`, `SpiTransaction` | Bus lock and chip select released on every return path, without exceptions |
| Template Method (in the bit-bang controllers) | `BitBangI2c::transfer` composed of `start`, `write_byte`, `read_byte`, `stop` | Protocol sequence fixed, primitive steps testable separately |
| Facade + Factory | `hal::System` | One owner, built from configuration in dependency order (ADR-0008) |

## 5. Implementations: hardware peripheral and bit-bang

### 5.1 Selection per platform (ADR-0006)

| Platform | Hardware peripheral | Bit-bang |
|---|---|---|
| Linux | kernel I2C/SPI driver, used through `/dev/i2c-N` (`ioctl(I2C_RDWR)`) and `/dev/spidevB.C` (`ioctl(SPI_IOC_MESSAGE(n))`) | kernel `i2c-gpio` / `spi-gpio` (devicetree), used through the same two device files |
| Zephyr | Zephyr I2C/SPI driver (`i2c_transfer`, `spi_transceive`) | Zephyr `gpio-i2c` / `zephyr,spi-bitbang` driver, used through the same API |
| FreeRTOS | `McuxLpi2c` / `McuxLpspi`, RTOS mode (MCUXpresso `fsl_lpi2c_freertos` / `fsl_lpspi_freertos`: the task sleeps during the transfer) | library `BitBangI2c` / `BitBangSpi` |
| Bare-metal | `McuxLpi2c` / `McuxLpspi`, blocking or asynchronous (non-blocking SDK transfer + completion interrupt + `poll()`) | library `BitBangI2c` / `BitBangSpi` |

When to choose bit-bang:

| Choose hardware when | Choose bit-bang when |
|---|---|
| A free peripheral exists on the required pins | No peripheral instance is free, or the pins cannot be routed to one |
| Speed above what the CPU can toggle, or DMA needed | Low speed is enough (configuration, slow sensors, expanders) |
| CPU time must stay free during transfers | The CPU can afford busy-waiting during the transfer |
| Glitch filters or hardware stuck-bus detection are wanted | Full control of the waveform is needed (debugging, unusual devices) |

### 5.2 Bit-bang I2C

| Topic | Decision | Reason |
|---|---|---|
| Pin drive | Open-drain (`PORTn.PCRx.ODE = 1`), input buffer on (`IBE = 1`) | I2C is wired-AND: nobody may drive a line high. Release = write 1; the pull-up resistor makes it high. The real level is readable through `PDIR` |
| Pin writes | `PSOR` / `PCOR` single stores | Lock-free; no read-modify-write (direction switching would need `PDDR` RMW, which has no set/clear alias) |
| Timing | Separate `t_low_ns` and `t_high_ns`, each at least the UM10204 minimum for the selected mode | A symmetric clock at 400 kHz gives 1.25 µs low, below the Fast-mode minimum tLOW of 1.3 µs |
| Clock stretching | After releasing SCL, read SCL until high; give up after `stretch_timeout_us` → `BusStuck` | A target may hold SCL low to gain time |
| Arbitration | Optional (`multi_master: true`): when sending a 1, read SDA while SCL is high; 0 means another controller won → `ArbitrationLost` | Only needed with more than one controller on the bus |
| Preemption | Allowed; bus mutex only, interrupts stay enabled | The controller generates the clock, so a longer pause is legal (exception: section 7.3) |
| Recovery | Release SDA; up to 9 SCL pulses while SDA reads low; then STOP | Bus-clear procedure in UM10204 |

UM10204 minimum timings used for `t_low_ns` / `t_high_ns` and the START/STOP conditions:

| Parameter | Standard-mode | Fast-mode | Fast-mode Plus |
|---|---|---|---|
| fSCL max | 100 kHz | 400 kHz | 1 MHz |
| tLOW min | 4.7 µs | 1.3 µs | 0.5 µs |
| tHIGH min | 4.0 µs | 0.6 µs | 0.26 µs |
| tHD;STA, tSU;STA, tSU;STO min | 4.0 / 4.7 / 4.0 µs | 0.6 / 0.6 / 0.6 µs | 0.26 / 0.26 / 0.26 µs |
| tBUF min (STOP to next START) | 4.7 µs | 1.3 µs | 0.5 µs |

These values are copied into the code generator's data table (not into C++ source) and must be checked
against the UM10204 revision the project references.

### 5.3 Bit-bang SPI

| Topic | Decision |
|---|---|
| Pins | SCK and MOSI push-pull outputs, MISO input, chip selects as native outputs |
| Modes | Generic loop handling CPOL (idle level of SCK) and CPHA (sample on leading or trailing edge); timing diagram `20` |
| Word size, bit order | 1-32 bits, MSB or LSB first, per device |
| Speed | Half period from `max_hz`; the achievable maximum depends on CPU clock and `IDelay` resolution and is reported in `caps().max_hz` |
| Preemption | Allowed (the controller generates the clock) |

### 5.4 Hardware adapters on MCXN947

| Topic | Decision | Evidence (section 11) |
|---|---|---|
| Function selection | Each LP_FLEXCOMM instance runs one function (`PSELID.PERSEL`); I2C and UART may share one instance, SPI may not share | `PERSEL` values NONE, UART, SPI, I2C, UART+I2C |
| Interrupt | One IRQ per LP_FLEXCOMM instance, shared by its functions; the SDK's LP_FLEXCOMM layer dispatches it | `LP_FLEXCOMM0` IRQ 35 listed for LPI2C0, LPSPI0, LPUART0 |
| I2C error detection | NACK (`NDF`), arbitration lost (`ALF`), FIFO error (`FEF`), pin-low timeout (`PLTF`, threshold `MCFGR3.PINLOW`), bus busy (`BBF`) | LPI2C `MSR` fields |
| I2C noise | Glitch filters `MCFGR2.FILTSCL` / `FILTSDA`, from configuration | LPI2C `MCFGR2` fields |
| SPI chip select | Up to 4 hardware PCS lines per LPSPI (`PARAM.PCSNUM`); more devices need GPIO chip selects | LPSPI `PARAM`, `TCR.PCS` |
| SPI CS timing | `CCR.PCSSCK` (setup), `CCR.SCKPCS` (hold), `CCR.DBT` (inactive time) | LPSPI `CCR` fields |
| SPI frame | `TCR.CPOL`, `CPHA`, `LSBF`, `FRAMESZ`, `CONT` (keep PCS asserted across frames) | LPSPI `TCR` fields |
| DMA and caches | The Cortex-M33 has no integrated data cache, so DMA buffers in SRAM need no cache maintenance on this target. The interface still carries `caps().dma_requires_cache_maintenance` for cores that do (e.g. Cortex-M7) | Arm Cortex-M33 architecture |

## 6. Concurrency, transactions and configuration per device (ADR-0007)

### 6.1 Transactions

| Rule | Reason |
|---|---|
| The bus mutex is held for a whole Transaction | No other device's traffic may appear between the register-address write and the data read |
| I2C: messages are joined with repeated START; STOP only at the end | A STOP would free the bus for another controller (multi-master) and some chips reset their register pointer on STOP |
| SPI: device configuration applied once per Transaction, skipped if unchanged | Reconfiguration costs time; devices on one SPI bus often share settings |
| SPI: chip select asserted for the whole Transaction | Many chips treat a chip-select edge as the end of a command |

### 6.2 Global lock hierarchy (GPIO + buses)

| Level | Lock | May take while holding |
|---|---|---|
| 1 | Expander port lock (GPIO) | level 2 |
| 2 | Bus controller lock | nothing |

The generator rejects every configuration that would break this:

| Rejected configuration | Why |
|---|---|
| Bit-bang SCL/SDA/SCK/MOSI/MISO on an expander pin | The bus (level 2) would take an expander lock (level 1): inverted order, deadlock possible |
| SPI chip select on an expander pin | Same inversion: CS is switched while the bus lock is held |
| I2C multiplexer (bus behind a bus) | Level 2 inside level 2; not supported in version 1 |

### 6.3 Speed: per bus for I2C, per device for SPI

- **I2C:** every device sees every clock edge, so the speed is a bus property. The generator requires
  `bus.speed_hz <= min(device.max_hz)`. UM10204 states that Standard-mode devices are not upward compatible and
  must not be placed on a Fast-mode bus.
- **SPI:** only the selected device listens; speed and mode are per device and applied per Transaction.

### 6.4 Timeouts (configuration)

| Key | Bounds |
|---|---|
| `timeouts.lock_ms` | Waiting for the bus mutex |
| `timeouts.transfer_ms` | One Transaction (hardware completion or bit-bang total) |
| `stretch_timeout_us` | Bit-bang I2C waiting for SCL to go high |
| `recovery.max_attempts` | Recovery attempts before `Faulted` |

## 7. Errors, recovery and failure modes

### 7.1 Error classes

| Error | Meaning | Bus state afterwards | Library action |
|---|---|---|---|
| `AddrNack` | No device acknowledged the address | Healthy (STOP sent) | None |
| `DataNack` | Device refused a data byte | Healthy (STOP sent) | None |
| `ArbitrationLost` | Another controller won the bus | Owned by the other controller | Abort; no recovery |
| `Timeout` | Lock or transfer did not finish in time | Unknown | Abort; recovery if an I2C line is low |
| `BusStuck` | SDA or SCL low and does not release | Stuck | Recovery up to `max_attempts`, then `Faulted` |
| `Io` | Peripheral FIFO or DMA error | Unknown | Abort; reinitialise peripheral |

Never retried automatically (ADR-0007): a repeated read of a clear-on-read status register, a FIFO, or an
auto-increment sequence returns different data or loses data. The chip driver decides.

### 7.2 Failure-mode analysis

| # | Failure | Typical cause | Detection | Library reaction | Residual risk / mitigation |
|---|---|---|---|---|---|
| 1 | SDA held low | Target reset or interrupted mid-byte | Lines checked at init and after abort; hardware `PLTF`/`BBF`; bit-bang SDA reads 0 when released | Recovery: 9 SCL pulses + STOP | Target hardware fault: `Faulted`; board may power-cycle the target |
| 2 | SCL held low permanently | Target crashed while stretching | Stretch timeout; `PLTF` | `BusStuck` → `Faulted` | Recovery cannot help (SCL cannot be driven high); target power-cycle |
| 3 | Address NACK | Device absent, wrong address, busy (EEPROM write cycle) | Protocol | `AddrNack` | Driver may poll (EEPROM acknowledge polling) |
| 4 | Data NACK mid-write | Device rejects byte, buffer full | Protocol | `DataNack` | Partial write possible: the driver must know the chip's behaviour |
| 5 | Arbitration lost | Multi-master bus | `ALF` / bit-bang read-back | `ArbitrationLost` | Driver retries after bus free |
| 6 | Glitches on SCL/SDA | Noise, long wires | Hardware filters (`FILTSCL`/`FILTSDA`); bit-bang has none | Misread bits possible | Hardware review: pull-up value, rise time, wiring |
| 7 | Slow rise time | Pull-up too weak, bus capacitance | Bit-bang reads SCL back (tolerates it); hardware may violate timing silently | None | Board design rule: check rise time against UM10204 |
| 8 | SPI device absent | Not populated, CS wiring | **Not detectable by the protocol**: MISO reads 0x00 or 0xFF | None | Driver identity check at init (mandatory) |
| 9 | SPI mode mismatch | Wrong `spi_mode` | **Not detectable**: bits shifted | None | Identity check at init |
| 10 | CS active during boot | CS pin floating before init | - | CS driven inactive before controller init (ADR-0008) | Board pull-up on active-low CS lines |
| 11 | Two functions on one LP_FLEXCOMM | Configuration error | Generator | Build fails | - |
| 12 | Bus lock held too long | Large transfer, slow device | `lock_ms` timeouts in other tasks | `Timeout` to the waiting task | Split large transfers; size `lock_ms` |
| 13 | SMBus target times out | Bit-bang task preempted > 25 ms | Usually NACK or garbage afterwards | Error to the driver | Section 7.3 |

### 7.3 Known limitation: SMBus clock-low timeout with bit-bang

SMBus devices (many battery gauges, some sensors) reset their interface when SCL stays low longer than
tTIMEOUT (minimum 25 ms in the SMBus specification). Plain I2C has no such limit. A bit-bang transfer can be
preempted by higher-priority tasks for longer than 25 ms in the middle of a byte.

Mitigation: devices carry `smbus: true` in the configuration; the generator **warns** when such a device is on a
bit-bang bus. The project then either uses a hardware controller, or shows by timing analysis that the bit-bang task
cannot be preempted for 25 ms.

## 8. Initialisation and configuration (ADR-0008)

Initialisation order is computed at build time from the dependency graph (diagram `18`):

```
1 native GPIO ports → 2 native pins (CS inactive, I2C lines released) → 3 bus controllers
→ 4 devices → 5 expander ports (fail-safe first) → 6 expander pins → start()
```

Configuration shape (authoritative schema: `config/gpio.schema.json`, to be written in phase 1):

```json
{
  "schema_version": 2,
  "buses": [
    { "id": "i2c_main", "type": "i2c", "impl": "hardware", "driver": "lib",
      "peripheral": { "kind": "lp_flexcomm", "instance": 2 },
      "mode": "rtos", "speed_hz": 400000, "owner_core": 0,
      "filters": { "scl_cycles": 2, "sda_cycles": 2 },
      "timeouts": { "lock_ms": 20, "transfer_ms": 10 },
      "recovery": { "max_attempts": 3, "hook": "board_i2c_main_recovery" } },
    { "id": "i2c_soft", "type": "i2c", "impl": "bitbang", "driver": "lib",
      "pins": { "scl": "soft_scl", "sda": "soft_sda" },
      "speed_hz": 100000, "delay": "cycle_counter",
      "stretch_timeout_us": 1000, "multi_master": false, "owner_core": 0,
      "timeouts": { "lock_ms": 20, "transfer_ms": 50 },
      "recovery": { "max_attempts": 3 } },
    { "id": "spi_ext", "type": "spi", "impl": "hardware", "driver": "lib",
      "peripheral": { "kind": "lp_flexcomm", "instance": 1 },
      "mode": "rtos", "owner_core": 0,
      "timeouts": { "lock_ms": 20, "transfer_ms": 10 } }
  ],
  "devices": [
    { "id": "exp_a", "bus": "i2c_main", "address": 32, "max_hz": 400000,
      "registers": { "framing": "i2c_reg8" } },
    { "id": "exp_b", "bus": "spi_ext", "max_hz": 10000000, "spi_mode": 0,
      "bit_order": "msb", "word_bits": 8,
      "cs": { "kind": "gpio", "pin": "exp_b_cs", "active_high": false },
      "cs_setup_ns": 50, "cs_hold_ns": 50, "cs_inactive_ns": 50,
      "registers": { "framing": "opcode", "chip": "mcp23s17", "hw_address": 0 } }
  ]
}
```

Values in this example are illustrative. `pins` and `cs.pin` refer to pin names defined in the GPIO part of
the same file (single source of truth). `hook` names a board function the generator declares and the board implements.

Generator checks added for buses:

| Check | Prevents |
|---|---|
| Pin referenced by a bus or CS exists, is native, `atomic_write`, not used twice | Lock inversion, double use |
| I2C bit-bang pins have `drive: open_drain` | Driving a line high against another device |
| `bus.speed_hz <= min(device.max_hz)` on I2C | Standard-mode device on a Fast-mode bus |
| Unique I2C addresses per bus | Two devices answering the same address |
| Hardware CS index < `PCSNUM` of the peripheral | Non-existent chip select |
| One function per LP_FLEXCOMM instance (UART + I2C allowed) | Peripheral conflict |
| Requested bit-bang speed within the platform's documented maximum | Silent slower clock |
| `smbus: true` device on a bit-bang bus | Warning (section 7.3) |
| Dependency graph acyclic | Undefined init order |

## 9. Impact on the GPIO design

| ID | Change | Where documented |
|---|---|---|
| A-1 | GPIO gains `drive: "open_drain"` and an `OpenDrainPin` handle (`release()`, `drive_low()`, `read()`) | ADR-0006 |
| A-2 | `gpio::IBus` is replaced by `hal::IRegisterDevice`; no `mutex()` in it | ADR-0005 |
| A-3 | `IExpanderChip::frame()` removed; framing is `ISpiFraming` / I2C register width | ADR-0005 |
| A-4 | Common `hal` namespace; `Error` gains `AddrNack`, `DataNack`, `ArbitrationLost`, `BusStuck`, `Unsupported` | ADR-0005 |
| A-5 | Configuration: new `buses` and `devices` arrays; expander port refers to a `device` id | ADR-0008 |
| A-6 | `GpioSystem` becomes `hal::System` with generated init order | ADR-0008 |
| A-7 | Lock hierarchy extended to buses; bit-bang pins and GPIO chip selects must be native | ADR-0007 |

The GPIO documents and diagrams `01`-`10` keep the earlier names; read `IBus` as `IRegisterDevice` and
`GpioSystem` as `hal::System`.

## 10. Design review: problems found while designing

Same severity scale as the GPIO review (critical: corruption or deadlock; major: defect on at least one target or blocked portability; minor: clarity or efficiency).

| ID | Severity | Problem found | Resolution |
|---|---|---|---|
| B-1 | Critical | A GPIO chip select or bit-bang pin on an expander makes the bus (level 2) take an expander lock (level 1); with an expander on the same bus this deadlocks | Forbidden by the generator; hierarchy documented (ADR-0007) |
| B-2 | Critical | An automatic retry inside the library would repeat reads of clear-on-read or FIFO registers and lose data | No library retries; drivers decide (ADR-0007) |
| B-3 | Major | `gpio::IBus` mixed bus, device and framing concerns, and exposed its mutex | Three layers; mutex internal (ADR-0005) |
| B-4 | Major | A symmetric clock at 400 kHz violates the Fast-mode tLOW minimum (1.25 µs < 1.3 µs) | Separate `t_low_ns` / `t_high_ns` from the specification table (section 5.2); diagram 14 corrected |
| B-5 | Major | Bit-bang I2C by switching pin direction needs read-modify-write of `PDDR`, which has no set/clear alias on MCXN947 | Open-drain mode via `PCR.ODE`; writes through `PSOR`/`PCOR` only |
| B-6 | Major | Per-device I2C speed is physically wrong: all devices see the clock | Speed per bus; generator checks the slowest device |
| B-7 | Major | Treating NACK as a bus fault would trigger recovery for every absent device | Separate error classes (section 7.1) |
| B-8 | Major | Hardware bus recovery needs the I2C pins as GPIO, but the library must not change pin multiplexing (GPIO review M-11) | Board-supplied `IBusRecoveryHook`; without it, `Faulted` |
| B-9 | Major | LP_FLEXCOMM functions are mutually exclusive (except UART + I2C) and share one IRQ | Generator check; IRQ dispatch through the SDK LP_FLEXCOMM layer |
| B-10 | Major | GPIO and bus initialisation depend on each other in both directions | One facade, generated topological order (ADR-0008) |
| B-11 | Major | SPI chip selects floating or active before init can select devices during boot | CS driven inactive before controllers start; board pull-ups recommended |
| B-12 | Minor | SPI absence or wrong mode is invisible to the protocol | Identity check at driver init (documented requirement) |
| B-13 | Minor | SMBus devices time out if a bit-bang transfer is preempted > 25 ms | Configuration flag + generator warning (section 7.3) |
| B-14 | Minor | Bit-bang on Linux user space would be slow and jittery | Kernel `i2c-gpio`/`spi-gpio` used instead (ADR-0006) |

Quality-goal check:

| Goal | How the design meets it | Residual risk |
|---|---|---|
| Q1 | Transaction-scoped mutex, two-level hierarchy, generator enforcement | none known |
| Q2 | Spec-table timings, mode-generic SPI loop, timing diagrams as reference | rise time and noise are board properties (FMEA 6, 7) |
| Q3 | Line check, timeouts, recovery state machine, `Faulted` state | SCL stuck low needs target power-cycle (FMEA 2) |
| Q4 | Distinct errors, no retries, identity checks | depends on driver discipline |
| Q5 | Controller interface + adapters; OS drivers preferred | FlexIO adapter not yet designed |
| Q6 | All waits configured | bit-bang total time depends on preemption |
| Q7 | Generator checks in section 8 | checks depend on correct board data in JSON |
| Q8 | Simulated open-drain lines and simulated targets on the host (phase 1) | timing correctness needs a logic analyser on hardware |

## 11. Verified MCXN947 facts

Source: `svd/MCXN947_cm33_core0.xml` in this repository.

| Fact | SVD evidence |
|---|---|
| LP_FLEXCOMM0, LPUART0 and LPSPI0 share base `0x40092000`; LPI2C0 is at `0x40092800` | `baseAddress` of the four peripherals |
| `PSELID.PERSEL` values: 0 NONE, 1 UART, 2 SPI, 3 I2C, 7 UART+I2C; `PSELID.LOCK` bit present | LP_FLEXCOMM0 `PSELID` (offset 0xFF8) |
| LPI2C0, LPSPI0 and LPUART0 all list interrupt `LP_FLEXCOMM0` = 35 | `<interrupt>` entries |
| LPI2C `PARAM` reset `0x303`: `MTXFIFO = MRXFIFO = 3` (log2 encoding, i.e. 8 entries; confirm in the reference manual) | LPI2C0 `PARAM` |
| LPI2C `MSR` flags `NDF`, `ALF`, `FEF`, `PLTF`, `MBF`, `BBF` | LPI2C0 `MSR` fields |
| LPI2C `MCFGR1.PINCFG` (0 = two-pin open drain), `MCFGR2.BUSIDLE`, `FILTSCL`, `FILTSDA`, `MCFGR3.PINLOW` | LPI2C0 configuration registers |
| LPSPI `PARAM` reset `0x40303`: `TXFIFO = RXFIFO = 3`, `PCSNUM = 4` | LPSPI0 `PARAM` |
| LPSPI `TCR`: `CPOL`, `CPHA`, `LSBF`, `PCS[1:0]`, `CONT`, `FRAMESZ[11:0]`, `WIDTH`, `PRESCALE` | LPSPI0 `TCR` |
| LPSPI `CCR`: `SCKDIV`, `DBT`, `PCSSCK`, `SCKPCS` | LPSPI0 `CCR` |
| `PORTn.PCRx`: `PS`, `PE`, `SRE`, `ODE`, `DSE`, `MUX[3:0]` (0 = GPIO), `IBE`, `INV`, `LK` | PORT0 `PCR0` |
| GPIO has `PDDR` but no set/clear alias for direction | GPIO register list (DESIGN_REVIEW.md section 7) |
| `FLEXIO0`, `I3C0`, `I3C1` present | peripheral list |

Not verifiable from the SVD and therefore to be confirmed on hardware: read-back of an open-drain GPIO pin through
`PDIR` with `IBE = 1`; availability of the DWT cycle counter (`DWT_CTRL.NOCYCCNT`); which LP_FLEXCOMM instances are
routed to usable pins on FRDM-MCXN947.

## 12. Platform API mapping (to be confirmed against the versions used)

| Operation | Linux | Zephyr | MCUXpresso SDK |
|---|---|---|---|
| I2C multi-message transfer | `ioctl(fd, I2C_RDWR, &i2c_rdwr_ioctl_data)` with `struct i2c_msg` (`I2C_M_RD`) | `i2c_transfer(dev, msgs, n, addr)` with `I2C_MSG_WRITE/READ/RESTART/STOP` | `LPI2C_MasterTransferBlocking`, `LPI2C_MasterTransferNonBlocking`, `LPI2C_RTOS_Transfer` |
| I2C bus recovery | kernel driver (if supported) | `i2c_recover_bus(dev)` (if implemented by the driver) | library procedure + board hook |
| SPI transfer | `ioctl(fd, SPI_IOC_MESSAGE(n), struct spi_ioc_transfer[])`, `cs_change` | `spi_transceive(dev, &spi_config, tx, rx)`; `SPI_HOLD_ON_CS`, `SPI_LOCK_ON`, `spi_release()` | `LPSPI_MasterTransferBlocking`, `LPSPI_MasterTransferNonBlocking`, `LPSPI_RTOS_Transfer` |
| GPIO chip select | spidev `cs-gpios` in devicetree | `spi_cs_control` (GPIO + delay) | library (`ChipSelect::Gpio`) |
| Function select | devicetree | devicetree (`nxp,lp-flexcomm` parent node) | `LP_FLEXCOMM_Init` |

## 13. Open questions and risks

| ID | Question | Impact |
|---|---|---|
| Q-E | Which I2C/SPI devices are on the real board (part numbers, maximum speeds, SMBus or not)? | Bus speeds, framing data, B-13 relevance |
| Q-F | Is any I2C bus multi-master? | Arbitration handling on bit-bang buses |
| Q-G | Is DMA needed (large SPI transfers: displays, flash)? | Adapter scope in phase 3 |
| Q-H | Are I2C multiplexers (e.g. TCA9548A) needed? | Lock hierarchy extension |
| Q-I | Which LP_FLEXCOMM instances are free (one is likely used by the console UART)? | Hardware vs bit-bang choice per bus |
| Q-A..Q-D | Still open from the GPIO review | - |

## 14. Updated implementation plan

| Phase | Content | Verifiable in this environment |
|---|---|---|
| 1 | Config schema v2 + generator (graph, checks); `hal` common types; GPIO core; bus core (`I2cDevice`, `SpiDevice`, transactions, register layer, framing); `BitBangI2c` / `BitBangSpi`; host OSAL; **simulated open-drain lines and simulated targets** (PCA9555 model on I2C, MCP23S17 model on SPI); tests incl. ThreadSanitizer | Yes |
| 2 | Zephyr adapters (GPIO, I2C, SPI) for FRDM-MCXN947 | No: build with `west` on your machine |
| 3 | MCUXpresso adapters: FreeRTOS (RTOS mode), then bare-metal (blocking, async) | No |
| 4 | Linux adapters (libgpiod v2, i2c-dev, spidev) | Partly |
| 5 | Hardware validation: logic-analyser captures for bit-bang timing, recovery tests with a deliberately stuck SDA | On your hardware |

## 15. Diagram index

| File | View | Notation |
|---|---|---|
| [`11_bus_overview_simple.puml`](diagrams/11_bus_overview_simple.puml) | The boxes to understand first | UML (simplified) |
| [`12_bus_c4_component.puml`](diagrams/12_bus_c4_component.puml) | Components and link to GPIO | C4 level 3 |
| [`13_bus_class_core.puml`](diagrams/13_bus_class_core.puml) | Controller, device, transaction, register layer | UML class |
| [`14_bus_class_impl.puml`](diagrams/14_bus_class_impl.puml) | Hardware and bit-bang implementations | UML class |
| [`15_seq_i2c_register_read.puml`](diagrams/15_seq_i2c_register_read.puml) | I2C register read with NACK and stuck-bus paths | UML sequence |
| [`16_seq_spi_transaction.puml`](diagrams/16_seq_spi_transaction.puml) | SPI register read with chip-select timing | UML sequence |
| [`17_state_bus_controller.puml`](diagrams/17_state_bus_controller.puml) | Controller lifecycle and recovery | UML state machine |
| [`18_activity_hal_init_order.puml`](diagrams/18_activity_hal_init_order.puml) | Build-time graph and run-time init order | UML activity |
| [`19_timing_i2c_bitbang.puml`](diagrams/19_timing_i2c_bitbang.puml) | I2C START, bits, stretching, ACK, STOP | UML timing |
| [`20_timing_spi_modes.puml`](diagrams/20_timing_spi_modes.puml) | SPI CPOL / CPHA | UML timing |

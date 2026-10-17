# ADR-0006: Hardware peripheral vs GPIO bit-bang implementation

| Field | Value |
|---|---|
| Status | Accepted (bus design review 2026-10-17) |
| Related | [BUS_DESIGN.md](../BUS_DESIGN.md) sections 4-5; diagrams `14`, `19`, `20`; [ADR-0004](0004-expander-driver-selection.md) (same rule for expanders) |

## Context

Each I2C or SPI bus can be driven by:

- a **hardware peripheral** (MCXN947: LPI2C or LPSPI inside an LP_FLEXCOMM instance), which generates the bus
  timing itself, has FIFOs, interrupts and DMA, and detects errors (NACK, arbitration loss, pin-low timeout);
- **bit-banging**: software sets and reads GPIO pins and creates the timing with delays.

Linux and Zephyr already ship both kinds as drivers (Linux: `i2c-gpio`, `spi-gpio`; Zephyr: `gpio-i2c`,
`zephyr,spi-bitbang`). FreeRTOS and bare-metal have none.

## Decision

1. Per bus, the configuration selects `impl: "hardware"` or `impl: "bitbang"`, and `driver: "os"` or `"lib"`,
   with the same preference order as for expanders:
   **OS driver** (Linux, Zephyr) **>** **library hardware adapter** (FreeRTOS, bare-metal) **>** **library bit-bang controller** (FreeRTOS, bare-metal).
2. On Linux and Zephyr, bit-banged buses are created by the OS (devicetree) and used through the same hardware
   adapter (`LinuxI2cDev`, `ZephyrI2c`, ...). The library's `BitBangI2c` / `BitBangSpi` are compiled only for FreeRTOS and bare-metal.
   Reason: on Linux every pin change from user space is a system call, so timing is slow and unbounded.
3. **Bit-bang pins must be native GPIO pins with `atomic_write` and `can_sleep = false`.** Expander pins are rejected
   by the configuration generator (lock hierarchy, ADR-0007; and one bus transfer per edge would be absurdly slow).
4. **I2C bit-bang uses open-drain pins** (`drive: "open_drain"` in the GPIO configuration; on MCXN947 the `PORTn.PCRx.ODE`
   bit) with the input buffer enabled (`IBE`), so the real line level can be read back for clock stretching,
   arbitration and bus-clear. Writing 1 releases the line; the pull-up resistor makes it high.
5. **Timing comes from an `IDelay` strategy** (cycle counter, hardware timer, calibrated loop). Delays are minimums:
   preemption only lengthens them. This is safe because both I2C and SPI are clocked by the controller,
   with one exception documented in section 7.3 of BUS_DESIGN.md (SMBus timeout).
6. **Bit-banging does not disable interrupts.** It takes only the bus mutex.

## Alternatives considered

| Alternative | Reason rejected |
|---|---|
| Library bit-bang on Linux via libgpiod | One ioctl per edge; tens of kHz at best with large jitter; the kernel drivers exist. |
| Disable interrupts during bit-bang bytes | Adds interrupt latency for the whole system; unnecessary because the protocols tolerate a slower clock. |
| Direction switching (input = released) instead of open-drain | On MCXN947 the direction register `PDDR` has no set/clear alias, so every edge would be a read-modify-write needing a critical section. Open-drain needs only `PSOR`/`PCOR`. |
| FlexIO-based emulation | Valid third option on MCXN947 (FLEXIO0 is present); deferred to a later adapter. |

## Consequences

- The GPIO library gains `drive: open_drain` and an `OpenDrainPin` handle with `release()`, `drive_low()`, `read()`.
- Achievable bit-bang clock depends on CPU frequency and delay resolution; the controller reports the achieved
  frequency in `caps().max_hz`, and the generator warns if a device requires a higher one.

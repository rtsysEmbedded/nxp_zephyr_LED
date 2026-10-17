# ADR-0007: Bus transactions, global lock hierarchy, timeouts and recovery

| Field | Value |
|---|---|
| Status | Accepted (bus design review 2026-10-17) |
| Extends | [ADR-0002](0002-concurrency-model.md) (concurrency model) |
| Related | [BUS_DESIGN.md](../BUS_DESIGN.md) section 6; diagrams `15`, `16`, `17` |

## Context

Several devices share one bus; several tasks use several devices; expander ports call buses while holding their
own lock. I2C can hang (a target holding SDA low); SPI cannot hang at protocol level but its peripheral or DMA can
time out. Some register reads have side effects (clear-on-read, FIFO pop, auto-increment).

## Decision

1. **Transaction = unit of locking.** The bus mutex is held from `begin()` until the Transaction is destroyed.
   One call such as `write_read()` is an implicit Transaction. During a Transaction:
   - I2C: no other device's START can interleave; a repeated START keeps the bus.
   - SPI: the device configuration is applied once, chip select stays asserted, segments are full duplex.
2. **Global lock hierarchy** (applies to GPIO, I2C and SPI together):

   | Level | Lock | May be held while taking |
   |---|---|---|
   | 1 | Expander port lock | a level-2 lock |
   | 2 | Bus controller lock | **nothing** |

   Consequences enforced by the configuration generator: a bit-bang pin and a GPIO chip select must be native,
   lock-free pins (never expander pins). An I2C multiplexer (a bus behind a bus) is not supported in this version,
   because it would require level-2-in-level-2 nesting.
3. **Bus speed for I2C is a property of the bus**, not of the device. The generator requires
   `bus.speed_hz <= min(device.max_hz)` over all devices on the bus. A Standard-mode device must not be placed on a bus
   clocked in Fast-mode (NXP UM10204: Standard-mode devices are not upward compatible).
   **For SPI the speed and mode are per device** and applied at the start of each Transaction (skipped when unchanged).
4. **Timeouts are configuration**: `timeouts.lock_ms` (waiting for the bus mutex), `timeouts.transfer_ms`
   (one Transaction), `stretch_timeout_us` (bit-bang I2C waiting for SCL high).
5. **Error classes are distinct**:
   - `AddrNack` / `DataNack`: protocol answer; STOP is sent; the bus is healthy; **no recovery**.
   - `Timeout`, `BusStuck`, `ArbitrationLost`, FIFO error: abort the peripheral; if an I2C line is still low,
     run **bus recovery** (up to 9 SCL pulses, then STOP; NXP UM10204 "bus clear"), at most `recovery.max_attempts`
     times, then enter `Faulted` (diagram `17`).
6. **No automatic retry of transfers** by the library. Re-reading a clear-on-read or FIFO register, or repeating a
   non-idempotent write, can corrupt data. Retries belong to the chip driver, which knows the register semantics.
7. **Hardware bus recovery needs the pins as GPIO.** The library never changes pin multiplexing (review finding M-11);
   the board supplies an `IBusRecoveryHook` (`to_gpio()`, `to_peripheral()`). Without a hook, the hardware
   controller reports `BusStuck` and enters `Faulted`.
8. **SPI chip-select timing** (`cs_setup_ns`, `cs_hold_ns`, `cs_inactive_ns`) is configuration. Hardware chip select
   uses the peripheral delays (MCXN947 LPSPI `CCR.PCSSCK`, `CCR.SCKPCS`, `CCR.DBT`); GPIO chip select uses `IDelay` and `osal::Clock`.

## Consequences

- Deadlock freedom extends to buses: two levels, fixed direction, no callbacks under any lock.
- A slow device on a shared I2C bus slows every device on it; the generator makes this visible.
- Chip drivers must check identity at init for SPI devices, because SPI has no acknowledge.

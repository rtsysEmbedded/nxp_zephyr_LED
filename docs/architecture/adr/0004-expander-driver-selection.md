# ADR-0004: Expander driver selection

| Field | Value |
|---|---|
| Status | Accepted (design review 2026-10-16) |
| Related | [DESIGN_REVIEW.md](../DESIGN_REVIEW.md) findings M-7, m-5, m-7; diagrams `05`, `06`, `07` |

## Context

GPIO expanders (PCA95xx/TCA95xx, MCP23017 on I2C; MCP23S17, 74HC595 on SPI) are supported by
Linux kernel drivers (`pca953x`, `mcp23s08`, `74x164`) and by upstream Zephyr drivers for several
families. On FreeRTOS and bare-metal no OS driver exists.

## Decision

1. **Per port, the configuration field `driver` selects** `"os"` (Linux kernel or Zephyr driver,
   exposed as an ordinary controller through `LinuxGpiodPort` / `ZephyrGpioPort`) or `"lib"`
   (library `ExpanderPort` over `IBus`).
2. **Default is `"os"` where a driver exists.** OS drivers already provide bus locking, shadow
   registers and interrupt handling, and allow other kernel consumers (e.g. LED triggers).
3. **`"lib"` is mandatory on FreeRTOS and bare-metal**, and allowed on Linux/Zephyr only for
   chips without a driver or when port-wide atomic writes across several pins are required.
4. **Every expander port declares `can_sleep: true`.** The generator rejects expander pins used
   for timing-critical roles (chip select, bit-banged protocols, PWM).
5. **`ExpanderPort` rules**: shadow is committed only after the bus transfer succeeds; on error
   `resync_needed` is set; fail-safe levels are written to the output latch **before** the
   direction register, at init and after a detected chip reset.
6. **Expander interrupts**: the native INT pin is edge-triggered; the event context re-reads the
   INT level after each input read and loops (bounded by `irq_max_reads`) to close the lost-edge window.

## Consequences

- Linux and Zephyr builds usually need no library expander code at all.
- The register map for each chip family is data (`config/chips/*.json`); chip behaviour
  (framing, interrupt-clear semantics) is code in `IExpanderChip` implementations.
- Glitches shorter than one bus read cannot be observed through an expander; this is a physical
  limit and is documented, not worked around.

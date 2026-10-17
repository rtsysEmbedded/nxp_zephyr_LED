# ADR-0008: One HAL facade with dependency-ordered initialisation

| Field | Value |
|---|---|
| Status | Accepted (bus design review 2026-10-17) |
| Amends | [ADR-0001](0001-layering-and-polymorphism.md) (`GpioSystem` becomes `hal::System`), [ADR-0003](0003-configuration-single-source-json.md) (schema gains `buses` and `devices`) |
| Related | [BUS_DESIGN.md](../BUS_DESIGN.md) section 8; diagram `18` |

## Context

GPIO, I2C and SPI now depend on each other in both directions:

```
native GPIO pins  ──►  bit-bang bus  ──►  device  ──►  expander port  ──►  expander pins
native GPIO pin   ──►  SPI chip select of a device
```

A separate `GpioSystem` and `BusSystem` would each need the other to be initialised first.

## Decision

1. One facade, `hal::System`, owns all GPIO ports, bus controllers, devices and expander ports.
2. The configuration generator builds the **dependency graph** from JSON, rejects **cycles** and the forbidden
   edges of ADR-0007 (bit-bang pin or chip select on an expander), and emits the init table in **topological order**.
3. Run-time `init()` walks that table and stops at the first failing element, returning its id. Elements already
   initialised remain in their safe state (outputs at fail-safe level, chip selects inactive, I2C lines released).
4. Safe defaults are applied as early as possible: SPI chip selects are driven **inactive before** their controller is
   initialised; open-drain I2C lines are released before the controller checks the bus.
5. `start()` (unchanged from the GPIO design): freeze subscriptions, enable interrupts, start event contexts.
6. The configuration schema gains two arrays, `buses` and `devices`. Expander ports reference a `device` id instead of
   `bus` + `address`.

## Consequences

- Initialisation order is a build artefact that can be reviewed, not an accident of code order.
- A wrong configuration (missing pin, cycle, forbidden edge) is a build error.
- Shutdown (Linux) runs the same table in reverse order.

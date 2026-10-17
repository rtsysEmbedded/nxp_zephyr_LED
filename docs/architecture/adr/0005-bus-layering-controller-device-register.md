# ADR-0005: Bus library layering: controller, device, register device

| Field | Value |
|---|---|
| Status | Accepted (bus design review 2026-10-17) |
| Amends | [ADR-0001](0001-layering-and-polymorphism.md) (`gpio::IBus` replaced), [ADR-0004](0004-expander-driver-selection.md) (framing moves out of `IExpanderChip`) |
| Related | [BUS_DESIGN.md](../BUS_DESIGN.md); diagrams `11`-`16` |

## Context

The GPIO design contained a small `IBus` interface (`write_reg`, `read_reg`, `mutex()`, `recover()`) only to
serve expander chips. The project now needs a complete, portable I2C and SPI library that:

- serves any chip driver (expanders, sensors, EEPROMs), not only GPIO expanders;
- runs on Linux, Zephyr, FreeRTOS and bare-metal;
- has two kinds of implementation behind the same interface: a hardware peripheral, or software that toggles GPIO pins (bit-banging).

Three different concerns were mixed in `gpio::IBus`:

| Concern | Belongs to |
|---|---|
| Moving bits on one physical bus (timing, START/STOP, FIFOs, chip select hardware) | the bus |
| Who is addressed and with which settings (I2C address; SPI chip select, mode, speed) | one device on the bus |
| How a register number is encoded into bytes (I2C 8/16-bit register address; SPI read flag; SPI opcode) | the chip family |

## Decision

1. **Controller** (`II2cController`, `ISpiController`): one object per physical bus. Owns the bus mutex,
   the peripheral or the bit-bang pins, timeouts and bus recovery. All implementations are interchangeable.
2. **Device** (`I2cDevice`, `SpiDevice`): a small value object per chip: controller pointer plus address, or plus
   `SpiConfig` (chip select, mode 0-3, maximum clock, bit order, word size, chip-select timing).
   `begin()` returns an RAII **Transaction** that holds the bus lock, applies the device configuration and,
   for SPI, keeps chip select asserted until the Transaction is destroyed.
3. **Register device** (`IRegisterDevice` with `I2cRegisterDevice`, `SpiRegisterDevice`): register access on top of
   one device. SPI framing is a strategy (`ISpiFraming`: `RwBitFraming`, `OpcodeFraming`) whose parameters come
   from the chip JSON file.
4. `gpio::IBus` is **replaced** by `hal::IRegisterDevice`. `ExpanderPort` depends on `IRegisterDevice` only.
   `IExpanderChip` keeps register map and interrupt semantics; its `frame()` method is removed.
5. The bus mutex is **not** exposed to chip drivers. It is taken inside a single call or inside an explicit Transaction.
6. Common types (`Result`, `Error`, OSAL, configuration access) move into the shared `hal` namespace used by GPIO, I2C and SPI.
   `Error` gains `AddrNack`, `DataNack`, `ArbitrationLost`, `BusStuck`, `Unsupported`.

## Alternatives considered

| Alternative | Reason rejected |
|---|---|
| Keep `gpio::IBus` and add SPI/I2C specifics to it | Mixes three concerns; every new chip family would change the bus interface. |
| One combined `IBus` for I2C and SPI | The protocols differ in essential ways (addressing vs chip select, ACK vs no ACK, half vs full duplex). A common interface would be either too weak or full of "unsupported" methods. |
| Expose `mutex()` to chip drivers | Lets drivers build their own lock orders; makes the global lock hierarchy (ADR-0007) unenforceable. |

## Consequences

- Sensor and EEPROM drivers reuse the same register layer as the expanders.
- The GPIO code (not yet written) uses `IRegisterDevice` from the start; the GPIO diagrams `04`/`04b` still show the older name `IBus`, and that name in them means `IRegisterDevice`.
- MCP23017 (I2C) and MCP23S17 (SPI) share one `IExpanderChip`; only the register device differs.

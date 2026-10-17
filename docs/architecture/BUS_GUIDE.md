# Portable I2C / SPI Library: A Step-by-Step Guide

This guide teaches the I2C/SPI part of the HAL from first principles. Sections 1-3 explain the two protocols
themselves; sections 4-10 explain how the library is built and why. If you already know I2C and SPI well,
start at section 4.

Prerequisite: the GPIO guide ([GUIDE.md](GUIDE.md)), at least sections 2 (building blocks) and 5 (concurrency rules).
Full specification and review: [BUS_DESIGN.md](BUS_DESIGN.md). Decisions: [ADR-0005 to ADR-0008](adr/).

| Section | You will learn |
|---|---|
| 1 | What I2C and SPI are, and how they differ |
| 2 | The I2C protocol, bit by bit |
| 3 | The SPI protocol and the four SPI modes |
| 4 | Hardware peripheral vs bit-banging |
| 5 | The building blocks of the library, with two complete examples |
| 6 | How bit-banged I2C works inside |
| 7 | How bit-banged SPI works inside |
| 8 | Sharing a bus between tasks and devices safely |
| 9 | When things go wrong: errors and bus recovery |
| 10 | Configuration and what the build checks for you |
| 11 | Self-check questions with answers |
| 12 | Glossary |

---

## 1. I2C and SPI in one page

Both are **serial buses**: a controller (the microcontroller) exchanges bytes with one or more chips over a few wires,
one bit at a time. The **controller always generates the clock**.

| | I2C | SPI |
|---|---|---|
| Wires | 2: SCL (clock), SDA (data), plus ground | 4: SCK (clock), MOSI (controller out), MISO (controller in), CS (chip select, one per device) |
| How a device is chosen | 7-bit **address** sent as the first byte | A dedicated **chip-select** wire per device is pulled low |
| Electrical drive | **Open-drain**: devices only pull lines low; resistors pull them high | **Push-pull**: outputs drive both high and low |
| Direction | Half duplex: one direction at a time | Full duplex: one bit out and one bit in on every clock |
| Confirmation | Receiver answers every byte with **ACK** (0) or **NACK** (1) | **None**: a missing device is silent |
| Typical speed | 100 kHz, 400 kHz, 1 MHz | 1-50 MHz |
| Typical devices | Sensors, expanders, EEPROMs, power monitors | Displays, flash memory, fast ADCs, expanders |

**Open-drain** is the key to understanding I2C. A device can connect a line to ground ("pull low") or let go of it
("release"). Nobody ever drives the line high; a pull-up resistor does that. Consequence: if any device pulls low,
the line is low. This is called **wired-AND**, and it is what makes ACK, clock stretching and arbitration possible
without electrical conflicts.

## 2. The I2C protocol, bit by bit

Timing diagram: [`19_timing_i2c_bitbang.puml`](diagrams/19_timing_i2c_bitbang.puml).

### 2.1 Four rules

| Rule | Meaning |
|---|---|
| Data changes only while SCL is low | While SCL is high, SDA must be stable: that is when the receiver samples it |
| **START**: SDA falls while SCL is high | The only events where SDA may change during SCL high are START and STOP |
| **STOP**: SDA rises while SCL is high | Frees the bus |
| Every byte is followed by a 9th clock for ACK/NACK | The receiver pulls SDA low (ACK) or leaves it high (NACK) |

### 2.2 Reading two registers from a PCA9555 (address 0x20)

```
START | 0x40 = 0x20<<1 | W | ACK | 0x00 (register) | ACK |
REPEATED START | 0x41 = 0x20<<1 | R | ACK | data0 | ACK by controller | data1 | NACK by controller | STOP
```

| Part | Who drives SDA | Meaning |
|---|---|---|
| `0x20<<1 | 0` | controller | "device 0x20, I will write" |
| ACK | device | "I am here" |
| `0x00` | controller | register pointer = 0 (INPUT0) |
| repeated START | controller | change direction without giving up the bus |
| `0x20<<1 | 1` | controller | "device 0x20, I will read" |
| data0, data1 | device | register 0 and register 1 (auto-increment) |
| ACK after data0 | controller | "send more" |
| NACK after data1 | controller | "that was the last byte" |
| STOP | controller | bus free |

Why a repeated START and not STOP + START? Between a STOP and the next START, another controller could take the bus,
and some chips reset their register pointer on STOP. The repeated START keeps the bus for the whole operation.

### 2.3 Clock stretching

The controller releases SCL, but a slow target may keep pulling it low until it is ready. Because SCL is open-drain,
nothing breaks: the controller must simply **read SCL back** and wait until it is really high. A controller that
does not read SCL back would sample data too early.

### 2.4 Arbitration (only with several controllers)

Two controllers can start at the same time. Each one reads SDA back while sending. A controller that sends a 1
(releases SDA) but reads a 0 knows that another controller is sending a 0; it has lost and stops. The other
controller never notices: its message continues undisturbed.

### 2.5 Speed is a property of the whole bus

Every device sees every clock edge. A Standard-mode (100 kHz) device must not be put on a bus clocked at 400 kHz:
NXP UM10204 states that Standard-mode devices are not upward compatible. So the library sets I2C speed **per bus**
and the build checks that it does not exceed the slowest device.

## 3. The SPI protocol and the four modes

Timing diagram: [`20_timing_spi_modes.puml`](diagrams/20_timing_spi_modes.puml).

SPI is two shift registers connected in a ring: on every clock, the controller shifts one bit out on MOSI and the
device shifts one bit out on MISO. After 8 clocks, the two have exchanged one byte. Writing and reading are the same
operation. To "only read", the controller sends filler bytes; to "only write", it ignores what comes back.

**Chip select (CS)** is usually active low. Pulling it low selects the device and often marks the start of a command;
releasing it ends the command.

Two settings must match the device datasheet:

| Setting | Meaning |
|---|---|
| **CPOL** (clock polarity) | Level of SCK while idle: 0 = low, 1 = high |
| **CPHA** (clock phase) | 0 = data is sampled on the **leading** (first) edge; 1 = sampled on the **trailing** (second) edge |

| Mode | CPOL | CPHA | Idle SCK | Sample on | Data changes on |
|---|---|---|---|---|---|
| 0 | 0 | 0 | low | rising edge | falling edge |
| 1 | 0 | 1 | low | falling edge | rising edge |
| 2 | 1 | 0 | high | falling edge | rising edge |
| 3 | 1 | 1 | high | rising edge | falling edge |

With CPHA = 0 the first bit must already be on the wire **before** the first edge (it appears when CS goes low).
With CPHA = 1 the first bit appears **on** the first edge.

**SPI has no acknowledge.** If the device is missing, or the mode is wrong, the controller still reads bytes:
usually all 0x00 or all 0xFF, or bits shifted by one position. Nothing reports an error. This is why every SPI chip
driver must read an identity or a known register at start-up and compare it.

## 4. Hardware peripheral vs bit-banging

| | Hardware peripheral | Bit-banging |
|---|---|---|
| What creates the waveform | Dedicated logic (MCXN947: LPI2C, LPSPI) | Software: write pin, wait, write pin, read pin... |
| CPU load during a transfer | Low (FIFO, interrupts, DMA) | 100 % of one CPU for the duration |
| Maximum speed | Specified by the peripheral | Limited by CPU clock and delay resolution |
| Error detection | In hardware (NACK, arbitration, stuck-bus timeout, glitch filters) | In software (read back lines) |
| Pins | Only pins that can be multiplexed to that peripheral | Any native GPIO pin |
| Typical reason to use | Default choice | No free peripheral, pins not routable, low-speed side bus |

On MCXN947 each **LP_FLEXCOMM** instance can be a UART, an SPI or an I2C controller (I2C and UART can be combined,
SPI cannot). One instance used for the console UART is therefore not available for SPI.

**Where each implementation comes from**

| Platform | Hardware | Bit-bang |
|---|---|---|
| Linux | kernel driver | kernel `i2c-gpio` / `spi-gpio` driver |
| Zephyr | Zephyr driver | Zephyr `gpio-i2c` / `spi-bitbang` driver |
| FreeRTOS, bare-metal | library adapter over the MCUXpresso SDK | **library** `BitBangI2c` / `BitBangSpi` |

The rule is the same as for GPIO expanders: use the operating system's driver when one exists.

## 5. The building blocks

Diagram: [`11_bus_overview_simple.puml`](diagrams/11_bus_overview_simple.puml). Details: diagrams `13` and `14`.

```
Chip driver        "read register 0x00, 2 bytes"
    │
RegisterDevice     turns registers into bus bytes (framing)
    │
Device             who: I2C address, or SPI chip select + mode + speed
    │              begin() → Transaction (holds the bus lock)
Controller         one physical bus; owns the lock; hardware OR bit-bang
```

| Block | One-sentence job | State | Lock |
|---|---|---|---|
| `IRegisterDevice` | Encode register numbers into bytes for one chip family | none | none |
| `I2cDevice` / `SpiDevice` | Remember how to talk to one chip | address / `SpiConfig` | none |
| `I2cTransaction` / `SpiTransaction` | Keep the bus (and the SPI chip select) for a sequence of transfers | - | holds the bus lock while it exists |
| `II2cController` / `ISpiController` | Drive one physical bus | peripheral or pins | owns the bus lock |

### 5.1 Example A: I2C register read

```cpp
// Illustrative usage, not final code.
I2cRegisterDevice exp{ i2c_main_device_for_exp_a };   // built by hal::System from JSON
uint8_t in[2];
Result<void> r = exp.read_reg(0x00, in);              // INPUT0, INPUT1
```

What happens (diagram [`15`](diagrams/15_seq_i2c_register_read.puml)):

| Step | Layer | Action |
|---|---|---|
| 1 | Register | build the write part `[0x00]` and the read part (2 bytes) |
| 2 | Device | take the bus lock (timeout from configuration) |
| 3 | Controller | START, address+W, `0x00`, repeated START, address+R, 2 bytes, NACK, STOP |
| 4 | Device | release the lock, return `Ok`, `AddrNack`, `Timeout` or `BusStuck` |

### 5.2 Example B: SPI register read from MCP23S17

The MCP23S17 expects: an **opcode** byte `0100 A2 A1 A0 R/W`, then the register address, then data.
For hardware address 0 and a read: `0x41`. Register GPIOA is `0x12`.

```
CS low | MOSI: 0x41 0x12 0x00(filler) | MISO: xx xx value | CS high
```

| Step | Layer | Action |
|---|---|---|
| 1 | Register (`OpcodeFraming`) | header `[0x41, 0x12]` |
| 2 | Device | take bus lock; apply mode 0, 10 MHz if the previous device used other settings |
| 3 | Controller | assert CS, wait `cs_setup_ns`, send header, clock 1 more byte while reading, wait `cs_hold_ns`, release CS |
| 4 | Device | release the lock |

Diagram: [`16_seq_spi_transaction.puml`](diagrams/16_seq_spi_transaction.puml).

## 6. Bit-banged I2C inside

### 6.1 Open-drain pins

On MCXN947 a GPIO pin becomes open-drain with `PORTn.PCRx.ODE = 1`. Then:

| Software action | GPIO register | Line |
|---|---|---|
| `release()` | `PSOR` (write 1) | high through the pull-up, unless a device pulls it low |
| `drive_low()` | `PCOR` (write 0) | low |
| `read()` | `PDIR` (needs input buffer `IBE = 1`) | the real level on the wire |

Each action is one register store or load: no read-modify-write, no lock. (Switching the pin direction instead would
need read-modify-write of `PDDR`, which has no set/clear alias on this chip.)

### 6.2 Timing comes from the specification, not from "half the period"

| Mode | tLOW min | tHIGH min | Period at full speed |
|---|---|---|---|
| Standard (100 kHz) | 4.7 µs | 4.0 µs | 10 µs |
| Fast (400 kHz) | 1.3 µs | 0.6 µs | 2.5 µs |
| Fast-mode Plus (1 MHz) | 0.5 µs | 0.26 µs | 1 µs |

At 400 kHz, "half the period" is 1.25 µs, which is **shorter** than the required tLOW of 1.3 µs. The library therefore
computes `t_low_ns` and `t_high_ns` separately. Waits are **minimums**: if the task is preempted, the clock simply gets
slower, which I2C allows (exception: SMBus devices, section 9.4).

### 6.3 The primitive steps (pseudo-code)

```cpp
// Illustrative pseudo-code. delay() waits at least the given time.

Result<void> wait_scl_high() {               // also handles clock stretching
    scl.release();
    deadline = now() + stretch_timeout;
    while (!scl.read())                      // a slow target may hold SCL low
        if (now() > deadline) return Error::BusStuck;
    return Ok;
}

void start() {                               // before: SCL high, SDA high
    sda.drive_low();  delay(t_hd_sta);       // SDA falls while SCL is high = START
    scl.drive_low();  delay(t_low);
}

Result<void> write_bit(bool b) {             // before: SCL low
    b ? sda.release() : sda.drive_low();     // change data only while SCL is low
    delay(t_low);
    TRY(wait_scl_high());                    // target samples now
    if (b && multi_master && !sda.read())
        return Error::ArbitrationLost;       // I sent 1, someone else sends 0
    delay(t_high);
    scl.drive_low();
    return Ok;
}

Result<bool> write_byte(uint8_t v) {         // returns true = ACK
    for (int i = 7; i >= 0; --i) TRY(write_bit((v >> i) & 1));
    sda.release();                           // let the target answer
    delay(t_low);
    TRY(wait_scl_high());
    bool ack = !sda.read();                  // target pulls low = ACK
    delay(t_high);
    scl.drive_low();
    return ack;
}

void stop() {                                // before: SCL low
    sda.drive_low();  delay(t_low);
    wait_scl_high();  delay(t_su_sto);
    sda.release();    delay(t_buf);          // SDA rises while SCL is high = STOP
}

Result<void> recover() {                     // bus clear (UM10204)
    sda.release();
    for (int i = 0; i < 9 && !sda.read(); ++i) {
        scl.drive_low();  delay(t_low);
        TRY(wait_scl_high());  delay(t_high);
    }
    if (!sda.read()) return Error::BusStuck;
    scl.drive_low();  delay(t_low);          // stop() expects SCL low; pulling SDA
    stop();                                  // low while SCL is high would be a START
    return Ok;
}
```

Why up to 9 pulses? A target that was interrupted while sending a byte still owns SDA. Each clock lets it shift out
one more bit; after at most 8 data bits and 1 ACK clock it is finished and releases SDA.

## 7. Bit-banged SPI inside

```cpp
// Illustrative pseudo-code for one word. Works for all four modes.
uint32_t shift_word(uint32_t tx, int bits) {
    uint32_t rx = 0;
    for (int i = 0; i < bits; ++i) {
        bool out = next_bit(tx, i);           // MSB or LSB first, per device
        if (cpha == 0) {
            mosi.set(out);                    // data ready BEFORE the leading edge
            delay(half);
            sck.set(!cpol);                   // leading edge: both sides sample
            rx = (rx << 1) | miso.read();
            delay(half);
            sck.set(cpol);                    // trailing edge: both sides change data
        } else {
            sck.set(!cpol);                   // leading edge: both sides change data
            mosi.set(out);
            delay(half);
            sck.set(cpol);                    // trailing edge: both sides sample
            rx = (rx << 1) | miso.read();
            delay(half);
        }
    }
    return rx;                                // reorder if LSB first
}
```

The chip select is not part of this loop. It is asserted when the Transaction begins and released when it ends,
with `cs_setup_ns` before the first edge and `cs_hold_ns` after the last.

## 8. Sharing a bus safely

### 8.1 One Transaction, one lock

```cpp
// Illustrative: several transfers that must not be interleaved with other devices.
auto t = flash.begin(timeout);        // takes the bus lock, applies mode/speed, asserts CS
if (!t) return t.error();
t->transfer(cmd_read_page);           // CS stays low between the two transfers
t->transfer(page_buffer);
// t destroyed here: CS released, bus lock released
```

| Rule | What it prevents |
|---|---|
| Lock for the whole Transaction | Another device's bytes appearing between a command and its data |
| SPI settings applied once per Transaction | A device receiving traffic in the wrong mode |
| Chip select held for the Transaction | The device aborting a multi-part command |

### 8.2 The global lock hierarchy

From the GPIO guide: an expander port takes its own lock (level 1) and then the bus lock (level 2). The bus lock is
**the last lock**: while holding it, nothing else may be locked.

Imagine an SPI chip select placed on an expander pin:

```
Task A: expander X lock (L1) ──► wants SPI bus lock (L2)     (writes a pin of X over SPI)
Task B: SPI bus lock (L2)    ──► wants expander X lock (L1)  (needs X to toggle its chip select)
→ deadlock
```

The configuration generator therefore refuses chip selects and bit-bang lines on expander pins. Native pins are
written with single lock-free stores (`PSOR`/`PCOR`), so using them while holding the bus lock takes no further lock.

### 8.3 Timeouts

Every wait has a configured limit: waiting for the bus lock (`lock_ms`), a whole transfer (`transfer_ms`), and a
stretched clock (`stretch_timeout_us`). Nothing can wait forever.

## 9. When things go wrong

### 9.1 A NACK is an answer, not a fault

| Situation | Error | Is the bus broken? | What the library does |
|---|---|---|---|
| No device at this address | `AddrNack` | No | Sends STOP, returns the error |
| Device busy (EEPROM writing internally) | `AddrNack` | No | Same; the driver may poll until ACK |
| Device rejects a data byte | `DataNack` | No | Sends STOP, returns the error |
| SDA or SCL stays low | `BusStuck` | **Yes** | Recovery (section 6.3), then `Faulted` if it fails |
| Transfer did not finish in time | `Timeout` | Maybe | Abort; recovery if a line is low |
| Another controller won | `ArbitrationLost` | No | Returns; the driver may retry |

State machine: [`17_state_bus_controller.puml`](diagrams/17_state_bus_controller.puml).

### 9.2 Why the library never retries by itself

Many chips have registers whose **read changes them**: interrupt status that clears when read, FIFOs that pop a
value, counters that reset. If the library silently repeated a failed read, the first read might already have taken
effect, and the second read would return different data. Only the chip driver knows whether repeating is safe.

### 9.3 Hardware recovery needs help from the board

To send the 9 recovery pulses, SCL and SDA must temporarily become GPIO pins instead of LPI2C pins. Changing pin
multiplexing is board knowledge, not library knowledge, so the board supplies a small hook object with `to_gpio()`
and `to_peripheral()`. Without a hook, a stuck hardware bus is reported as `Faulted`.

### 9.4 One known limitation: SMBus

SMBus devices reset their interface if SCL stays low for more than 25 ms. A bit-banged transfer that is preempted for
that long breaks the transfer. Mark such devices with `"smbus": true`; the build warns if they sit on a bit-banged bus.

## 10. Configuration and build checks

Buses and devices are described in the same JSON file as the GPIO pins (example in [BUS_DESIGN.md](BUS_DESIGN.md)
section 8). Bit-bang pins and GPIO chip selects refer to GPIO pin **names**, so each pin is defined exactly once.

The generator checks, among others:

| Check | Prevents |
|---|---|
| Bus pins and chip selects are native, lock-free pins | Deadlock (section 8.2) |
| I2C bit-bang pins are open-drain | Two devices driving a line in opposite directions |
| I2C bus speed ≤ the slowest device's maximum | Standard-mode device on a Fast-mode bus |
| No two devices with the same I2C address on one bus | Two answers to one address |
| Hardware chip-select index exists (LPSPI: 0-3) | Configuring a non-existent signal |
| One function per LP_FLEXCOMM (UART + I2C allowed) | Peripheral conflict |
| The dependency graph has no cycle | Undefined start-up order |

Start-up order is computed from the dependencies (diagram [`18`](diagrams/18_activity_hal_init_order.puml)).
Chip selects are driven inactive **before** any SPI controller starts, so no device is selected by accident during boot.

## 11. Self-check questions

1. Why can a device stretch the I2C clock without an electrical short circuit?
2. Why does reading a register use a repeated START instead of STOP followed by START?
3. Why is the I2C speed configured per bus but the SPI speed per device?
4. A bit-banged I2C bus runs at 400 kHz with a symmetric clock. What is wrong?
5. Why must an SPI chip driver check a known register at start-up?
6. In SPI mode 3, on which edge is data sampled, and what is the idle level of SCK?
7. Why is an SPI chip select on an expander pin rejected by the build?
8. A read returns `AddrNack`. Should the library run bus recovery?
9. Why does the library not retry a failed read automatically?
10. Why is bit-banged I2C implemented with open-drain pins instead of switching the pin direction?

<details>
<summary>Answers</summary>

1. I2C lines are open-drain: devices only pull low and never drive high. The controller releases SCL; if the target keeps it low, the line is simply low. The controller reads SCL back and waits.
2. Between STOP and START another controller may take the bus, and some chips reset their register pointer on STOP. A repeated START keeps the bus for the whole operation.
3. All I2C devices see every clock edge, so the clock must suit the slowest device. In SPI only the selected device listens.
4. Each half is 1.25 µs, shorter than the Fast-mode minimum tLOW of 1.3 µs. tLOW and tHIGH must be computed separately.
5. SPI has no acknowledge. A missing or wrongly configured device returns plausible-looking bytes without any error.
6. Mode 3: CPOL = 1 (SCK idles high), CPHA = 1 (sample on the trailing edge, which is the rising edge).
7. The chip select is switched while the bus lock (level 2) is held; switching an expander pin needs the expander lock (level 1). Taking level 1 while holding level 2 inverts the order and can deadlock.
8. No. A NACK means the bus worked and the device answered "no". STOP is sent and the bus is healthy.
9. Reads can have side effects (clear-on-read, FIFO pop). A repeated read can return different data or lose data. Only the chip driver knows.
10. On MCXN947 the direction register `PDDR` has no set/clear alias, so every direction change would be a read-modify-write needing a lock. With open-drain, every edge is a single lock-free store to `PSOR` or `PCOR`.

</details>

## 12. Glossary

| Term | Meaning |
|---|---|
| **ACK / NACK** | I2C answer to every byte: ACK = receiver pulls SDA low on the 9th clock; NACK = leaves it high. |
| **Arbitration** | How two I2C controllers that start together decide who continues: the one reading back a different bit than it sent stops. |
| **Bit-banging** | Creating a bus waveform in software by setting and reading GPIO pins. |
| **Bus recovery (bus clear)** | Up to 9 clock pulses plus STOP to free an I2C bus whose SDA is held low. |
| **Chip select (CS)** | SPI wire that selects one device; usually active low. |
| **Clock stretching** | An I2C target holding SCL low to delay the controller. |
| **Controller / target** | The device that generates the clock and starts transfers / the device that answers. (Older documents: master / slave.) |
| **CPOL / CPHA** | SPI clock polarity (idle level) and clock phase (sampling edge). Together they form modes 0-3. |
| **Framing** | How a register number and the read/write direction are encoded into the first bytes of a transfer. |
| **Full duplex** | Sending and receiving at the same time (SPI). |
| **LP_FLEXCOMM** | MCXN947 serial block that works as UART, SPI or I2C (or UART + I2C). |
| **LPI2C / LPSPI** | The I2C and SPI controller peripherals inside LP_FLEXCOMM on MCXN947. |
| **Open-drain** | Output that can only pull low or release; a resistor provides the high level. |
| **Repeated START** | A START without a preceding STOP; keeps the bus between the parts of one operation. |
| **SMBus** | A protocol based on I2C with stricter rules, including a 25 ms clock-low timeout. |
| **START / STOP** | I2C bus conditions: SDA falls / rises while SCL is high. |
| **tLOW / tHIGH** | Minimum time SCL must stay low / high (UM10204). |
| **Transaction** | A sequence of transfers done while holding the bus lock (and, for SPI, the chip select). |
| **Wired-AND** | Property of open-drain lines: the line is high only if nobody pulls it low. |

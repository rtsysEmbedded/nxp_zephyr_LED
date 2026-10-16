# ADR-0003: Configuration as a single JSON source with build-time generation

| Field | Value |
|---|---|
| Status | Accepted (design review 2026-10-16) |
| Related | [DESIGN_REVIEW.md](../DESIGN_REVIEW.md) findings M-10, m-8, R-3, R-5; diagram `09` |

## Context

Project rule: no setting is hard-coded in source; all configuration lives in JSON.
Microcontroller targets have no file system at boot and no budget for a JSON parser.
Zephyr's native configuration mechanisms are devicetree and Kconfig.

## Decision

1. `config/gpio.json` (project) and `config/chips/*.json` (expander register maps) are the
   **only** human-edited configuration. Both carry `schema_version`.
2. `config/gpio.schema.json` (JSON Schema) defines structure; `tools/gpio_codegen.py` performs
   schema validation **and** semantic validation, and fails the build on any error:
   duplicate pin, index out of port width, `can_sleep` source used at ISR time, IRQ priority
   outside the RTOS-allowed range, undefined bus, missing `fail_safe` on an output, port or bus
   with more than one owning core.
3. Outputs per target:
   - MCU (Zephyr, FreeRTOS, bare-metal): `gpio_config.hpp` with `constexpr` tables (flash only).
   - Zephyr additionally: `gpio.overlay` containing **only** new expander nodes and references to
     existing labels (never redefining board nodes), and a `gpio.conf` Kconfig fragment.
   - Linux: `gpio.json` is installed and parsed at start-up by `JsonConfigSource`.
4. IRQ priorities are written in **logical levels** (0 to 2^nvicPrioBits - 1; 0 to 7 on MCXN947)
   and converted by the generator into the shifted register form the RTOS expects.
5. Machine-specific paths (SDK, Zephyr base) belong in `CMakeUserPresets.json`, which is not
   version-controlled, not in shared preset files.

## Alternatives considered

| Alternative | Reason rejected |
|---|---|
| Kconfig + devicetree only | Not available on Linux, FreeRTOS, bare-metal; two sources of truth. |
| Run-time JSON parsing on the MCU | Flash/RAM cost, late error detection on the device. |
| Hand-written C++ config headers | Violates the JSON rule; no validation. |

## Consequences

- Configuration errors are build errors, not field failures.
- The generator becomes a tested component (unit tests with valid and invalid fixtures).
- Developers edit one file per board variant; generated files are build artefacts and are not committed.

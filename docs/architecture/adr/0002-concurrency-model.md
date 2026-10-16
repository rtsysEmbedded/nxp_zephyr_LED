# ADR-0002: Concurrency model

| Field | Value |
|---|---|
| Status | Accepted (design review 2026-10-16) |
| Related | [DESIGN_REVIEW.md](../DESIGN_REVIEW.md) findings C-1, C-2, M-3, M-4, M-5, M-6, M-9; diagrams `05`, `06`, `07` |

## Context

Execution contexts differ per platform: threads (Linux), threads + ISRs (Zephyr, FreeRTOS),
super-loop + ISRs (bare-metal). Expander operations can sleep (bus transfer of tens to hundreds
of microseconds); native writes cannot. The MCXN947 target is dual-core.

## Decision

1. **Two primitives with distinct meaning**:
   - `osal::Mutex`: task-to-task; may block; must have priority inheritance
     (Zephyr `k_mutex`, FreeRTOS mutex type, not a binary semaphore). On bare-metal it is a no-op
     because only one non-ISR context exists.
   - `osal::CriticalSection`: task-to-ISR; never blocks; short; never encloses a bus transfer.
2. **`ScopedLock` reports failure**: `[[nodiscard]] bool owns()`. Every timeout becomes
   `Error::Timeout` to the caller. No code path continues without the lock.
3. **Fixed lock order**: `ExpanderPort::mtx_` then `IBus::mutex()`. Pins hold no lock. Maximum
   nesting depth is 2, which keeps the FreeRTOS priority-inheritance limits with nested mutexes
   out of play.
4. **No sleeping call from an ISR**: a port with `caps().can_sleep` returns `Error::IsrContext` when
   `osal::Context::in_isr()` is true.
5. **ISR deferral through `osal::EventSignal`**: the ISR clears the hardware flag and raises a bit;
   the event context (FreeRTOS task, Zephyr dedicated work queue, bare-metal `poll()`, Linux
   edge-event thread) performs bus I/O and dispatch.
6. **Observers**: subscription only before `start()`; the table is immutable afterwards; callbacks
   are invoked with no library lock held and never from an ISR.
7. **Shared-state ownership is explicit per field** (table in DESIGN_REVIEW.md section 6.3).
   In particular the expander shadow registers are never accessed from an ISR.
8. **Multi-core**: each port and each bus has exactly one owning core (`owner_core` in the
   configuration, validated by the code generator). Cross-core access goes through IPC, not shared state.
9. **Timeouts are configuration**, not literals (`timeouts.port_lock_ms`, `timeouts.bus_lock_ms`).

## Alternatives considered

| Alternative | Reason rejected |
|---|---|
| Per-pin mutex | Extra lock level, no protection for shared port registers. |
| Global library lock | Serialises independent buses; priority inversion across unrelated pins. |
| Lock-free shadow with CAS | Still needs serialisation of the bus transfer itself; no benefit. |
| Running user callbacks in the ISR | Forbids blocking calls in callbacks; expander reads impossible. |

## Consequences

- Deadlock freedom follows from the fixed two-level order and no callback under lock.
- The contract table (context × operation) must be part of the public documentation.
- On bare-metal, expander writes are asynchronous (`poll()` required) or blocking with timeout; selected by configuration.

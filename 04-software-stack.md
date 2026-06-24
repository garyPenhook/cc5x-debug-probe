# Software Stack

The system is **three separate codebases** plus one shared protocol definition. This
doc picks the toolchain/framework for each and — the important decision — how to keep
the CDL protocol from drifting across them. Probe MCU = **STM32 (ST preferred)**.

```
  ┌─ TARGET STUB (PIC) ────────┐   ┌─ PROBE FIRMWARE (STM32) ───┐   ┌─ PC TOOL ──────────┐
  │ CC5X C, generated          │   │ C (CubeMX+HAL) or Rust     │   │ Python (cc5x-helper)│
  │ cdl_monitor_<dev>.c/.h     │   │ USB-CDC + UART/DMA + timer  │   │ debug-monitor cmd   │
  └────────────┬───────────────┘   └─────────────┬──────────────┘   └──────────┬─────────┘
               │                                  │                              │
               └──────────────► shared CDL protocol definition ◄────────────────┘
                                (one spec → generated codec for all three)
```

---

## 1. Component A — target stub (PIC, CC5X)

- **Language/toolchain:** plain C compiled by **CC5X** — no framework, static
  allocation, ISR-safe ring buffer (this is `02`/`03` territory).
- **Produced by:** the helper's `debuggen.py` + `debug-stub` command, per device.
- **Software scope:** tiny — UART/bit-bang TX, optional RX command handler (Tier A),
  the `CDL_TRACE/LOG/BP/POLL` macros, and the generated protocol constants (see §4).
- **Testing:** the *generation* is unit-testable in CI (golden-file the emitted C);
  the compiled stub is validated on hardware.

## 2. Component B — probe firmware (STM32)

This is the new firmware to write. It's **fixed and target-independent**, so it's
written once.

### Recommended part
For a USB trace probe you want: native USB device, a timer with **input-capture +
DMA** (for Tier-C edge timestamping and precise timing), a UART (ideally DMA) to the
target, enough RAM to buffer bursts, a cheap dev board.

| Option | Notes (verify exact RAM vs the chosen part datasheet before committing) | When to pick |
|--------|------------------------------------------------------------------------|--------------|
| **STM32F042K6** ✅ chosen | **USB *Device* core** (same family as F072, simpler than OTG). LQFP-32, prototype on **NUCLEO-F042K6** — the available dev board (deciding constraint). Datasheet-confirmed (DocID025832 R5, in repo): Cortex-M0 @48 MHz, 32 KB flash, **6 KB SRAM**, crystal-less USB FS, 5-ch DMA, 2 USART, one 32-bit timer. 6 KB SRAM is the buffer budget — adequate for a single-link bridge. | **Locked** (2026-06-23) — board on hand, in stock, USB device core |
| **STM32F072** *(was default; fallback)* | Same USB *Device* core; the **DMA-accelerated multi-UART↔USB-CDC reference design** (`majbthrd/stm32cdcuart`) targets it (~90% of this probe). More RAM/flash than F042. Nucleo/Discovery-F072 boards. | Fallback if F042K6 is too tight |
| **STM32F411 "Black Pill"** | USB **OTG-FS** core, more RAM + speed (F4 class), huge community, ~cheap board. OTG slightly more involved than the device core. | Most headroom if both F0 parts fall short |
| **STM32G431 (Nucleo-G431)** | Newer G4, USB FS, rich **timer/comparator interconnect** (great for advanced edge capture). | Fanciest capture fabric |

ST splits USB into a **Device core on F0–F3** and **OTG core on F4+** — both do USB
CDC; the device core is a touch simpler to bring up. **Locked choice: STM32F042K6**
(LQFP-32, USB device core) — the deciding factor is dev-board availability (NUCLEO-F042K6
is the board on hand). The `majbthrd/stm32cdcuart` reference targets the F072 USB core;
porting to the F042 USB IP is minor (the probe needs only 1 USART + RX/TX DMA + one
32-bit timer, all present). Design buffers to the **6 KB SRAM** budget. If a *future* rev
outgrows it, **F072** (16 KB SRAM, the reference's native part) is the upgrade path.
Datasheet (`stm32f042k6.pdf`, DocID025832 R5) + RM0091 + pm0215 are in the repo and
citable via the pdf-creator MCP.

### Firmware framework — pick one
| Stack | Pros | Cons | Verdict |
|-------|------|------|---------|
| **STM32CubeMX + HAL/LL (C)** | ST-official; CubeMX generates USB-CDC middleware + UART/DMA/timer init; most docs/examples | HAL is heavy/verbose | **Recommended** given ST preference |
| **libopencm3 (C)** | lean, open-source, clean USB stack | less hand-holding, fewer examples | good if you dislike HAL |
| **TinyUSB** (USB layer) on HAL/LL or libopencm3 | vendor-neutral, solid CDC, portable if you ever change MCU | extra integration | nice if portability matters |
| **Rust + `embassy`** | memory-safe, great async USB | different toolchain/skillset | only if you want Rust |

**Recommendation:** **STM32CubeMX + HAL/LL in C**, USB-CDC device class, **UART RX via
DMA** (ring buffer) toward the target, a **free-running 32-bit timer (TIM2)** for
timestamps, and **timer input-capture + DMA** on the toggle pin for Tier-C targets.
Model the UART↔USB path on `majbthrd/stm32cdcuart`.

**Implementation note (P5a vs P5b).** The first bench build (P5a,
`firmware/probe-f042/`) deliberately uses **direct-register LL, interrupt-driven RX,
and a buffered polled TX** (no HAL, no DMA, no USB) for a minimal, dependency-light
bring-up over the ST-LINK VCP. **DMA on the USARTs, the USB-CDC stack (PA11/PA12), and
Tier-C input-capture are P5b** — that is where the CubeMX/HAL + `majbthrd` path above
applies. The portable relay/framing core is transport- and HAL-independent and is
covered by host unit tests (`tests/`).

### Testing
Probe firmware is hardware-bound, but the **CDL codec it uses is the generated C
from §4**, which is exercised by the PC-side codec tests — so the framing logic is
covered even though the USB/DMA plumbing needs a board.

## 3. Component C — PC tool (Python, in `cc5x-helper`)

- **Language:** Python — it *is* the existing project; reuse its CLI/JSON conventions
  and test harness.
- **New command:** `cc5x-helper debug-monitor --map cdl_map_<dev>.json --port <cdc>`.
- **Deps:** `pyserial` for the companion's USB-CDC VCP; everything else stdlib.
- **Does:** decode CDL frames, apply the symbol/channel map (names + addresses),
  render timestamped trace, drive watch/read-mem/breakpoint commands (Tier A).
- **Testing:** the decoder + codec are pure Python → unit-tested in CI exactly like
  the rest of the suite (no hardware needed). This is also the reference
  implementation of the protocol.

## 4. The shared protocol — one source of truth (key decision)

CDL framing/message-types are implemented in **three** places (CC5X-C, STM32-C,
Python). Hand-maintaining three copies guarantees drift. Instead:

> Define the CDL protocol **once** as a Python spec (message types, field layouts,
> CRC/framing params) inside the helper, and **generate** the rest.

The helper emits, from that single spec:
1. **Python codec** — used directly by `debug-monitor`, and the CI test target.
2. **Target C header** (`cdl_proto.h`) — `#define`s + pack/unpack for the CC5X stub.
3. **Probe C header** (same `cdl_proto.h`, MCU-neutral) — constants + codec for STM32.

This is the same codegen pattern the project already uses (`headergen`, `debuggen`),
and it means a protocol change is a one-file edit + regenerate, with the Python codec
test catching mismatches before they reach firmware. The probe firmware is *not*
generated per target — only this shared header is shared with it.

---

## 5. End-to-end toolchains at a glance

| Piece | Language | Build tool | USB/Link | Tested in CI? |
|-------|----------|-----------|----------|---------------|
| Target stub | C | CC5X (via helper) | UART / bit-bang / toggle | codegen: yes; binary: on HW |
| Probe FW | C | STM32CubeMX + arm-none-eabi-gcc (Makefile/CMake) | USB-CDC + UART-DMA | codec: yes; USB/DMA: on HW |
| PC tool | Python | uv / pip (in `cc5x-helper`) | pyserial (VCP) | yes (pure Python) |
| CDL protocol | Python spec → C/Py | helper codegen | — | yes |

---

## 6. Suggested build order (re-stated for software)

> **Status:** step 3 (`debuggen.py` Tier-A stub + map) is **DONE** and compile-verified;
> the SPBRG/baud value is computed from the manifest `fosc`+`baud` using the EUSART
> formula fetched from the **Microchip MCP** (cited provenance), not hand-entered. The
> authoritative phased roadmap is in `00-master-plan.md` §5.

1. **CDL protocol spec + Python codec + tests** (CI, no hardware). ← start here
2. **PC `debug-monitor`** decoding canned frames (still no hardware).
3. **`debuggen.py`** emitting Tier-A stub for one verified full-monitor PIC + the map.
   **✅ DONE** (Tier-A full/min + Tier-C fixed-site; CC5X compile verified).
4. **Compile-and-measure gate (CC5X only, no probe/target)** — compile the generated
   stub for a roomy A-full, a 512 B A-full, and a 256 B A-min part, and assert the
   `.occ`/`.var`/`.asm`/`.fcs` budget fits the promised tier (`02` §6, `03` §7 step 3).
5. **STM32 probe FW** (CubeMX + HAL, USB-CDC + UART-DMA), pass-through + timestamp.
6. Wire end-to-end on hardware; add commands/breakpoints.
7. Tier-B (bit-bang) and Tier-C (toggle + STM32 input-capture) paths.

Steps 1–4 are pure software and CI-testable (step 4 needs only the CC5X compiler);
hardware only enters at step 5.

---

## 7. Decisions still open

- ~~**STM32F072 vs F411**~~ — **resolved: STM32F042K6** (2026-06-23, T-001). Datasheet
  (`stm32f042k6.pdf`, DocID025832 Rev 5) + RM0091 + pm0215 now in repo. Confirmed budget:
  **6 KB SRAM, 32 KB flash, crystal-less USB FS** (internal 48 MHz RC + CRS), 5-ch DMA,
  two USARTs. Remaining verification: USB-CDC + UART-DMA + timestamp buffers must fit in
  6 KB SRAM, and the F072-targeted `majbthrd/stm32cdcuart` USB stack must port to the F042
  USB IP. Fall back to F072/F411 if it won't fit.
- **C/HAL vs Rust/embassy** — recommend C/HAL for ST-ecosystem support; flag Rust as
  viable if preferred.
- **Probe↔target link:** 2-wire UART (enables host→target commands/breakpoints) vs
  1-wire (trace only, fewer pins). Affects which CDL tier the *probe* exposes.
- **Probe build system:** CubeMX-generated Makefile vs CMake (CMake integrates more
  cleanly if you ever script firmware builds from the helper).

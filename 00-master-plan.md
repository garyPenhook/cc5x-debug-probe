# CC5X Debug Probe — Master Design Plan

Status: **living plan**. This is the top-level design and roadmap for the whole
system; docs [01](01-debug-link-protocol.md)–[04](04-software-stack.md) remain the
detailed references for each area. Read this first.

## 1. Why this exists

No working interactive source-level debugger exists for **CC5X** on modern
enhanced-midrange PIC: the MPLAB X + CC5X plugin only partially works and rides a
deprecated IDE, gpsim doesn't cover the Five-Digit (F15xxx/F18xxx) parts, and a true
GDB path is infeasible for this silicon. Instead we build a **cooperative trace/
monitor probe**: the target reserves a link and runs a small *generated* stub; a fixed
companion MCU timestamps/buffers/forwards to a PC. It depends on **nothing** from
Microchip's proprietary debug stack, so it works on every modern target.

Honest scope: named trace + timestamps, on-demand memory **read** (write off by
default), software breakpoints (halt-in-monitor). **Not** transparent hardware
breakpoints, silicon single-step, or zero overhead (the probe effect is real).

## 2. Architecture (the five parts + one protocol)

```
 TARGET PIC (CC5X)          PROBE MCU (STM32F042K6)            PC (Python: cc5x-helper)
 cdl_monitor_<dev>.c/.h ── CDL link (UART) ─► timestamp+buffer ─ USB-CDC ─► debug-monitor
   generated per device        fixed firmware, written once         decode + map + commands
        ▲                                                                    ▲
        └───────────────── shared CDL protocol: cdl_proto spec ──────────────┘
                            (one source → Python codec + C headers)
        ▲
   Microchip MCP ── EUSART baud formula/table → SPBRG (with datasheet provenance)
```

1. **Target stub** — generated CC5X C; *all* device-specific knowledge lives here +
   `cdl_map_<dev>.json`. ([03](03-helper-codegen-and-companion.md))
2. **Probe firmware** — fixed, device/target-family independent, **STM32F042K6**
   (LQFP-32, USB device core; prototype on **NUCLEO-F042K6** — the available dev board).
   Start from the `majbthrd/stm32cdcuart` DMA UART↔USB-CDC design (F072-native; port to
   F042 is minor — the probe needs 1 USART + RX/TX DMA + one 32-bit timer). Size buffers
   to the **6 KB SRAM** budget. ([04](04-software-stack.md))
3. **PC tool** — `cc5x-helper debug-monitor`; pure Python, the reference protocol impl.
4. **Shared protocol** — one Python spec → generated Python codec + MCU-neutral C
   header, so the three codebases never drift. ([04 §4](04-software-stack.md))
5. **Baud source** — the **Microchip MCP** (`search_microchip_product_documents`)
   returns the EUSART BRG formula + SPBRG table + a citable URL; codegen computes the
   divisor from Fosc with provenance. No new MCP server is needed.

## 3. Decisions locked

| Decision | Choice | Rationale |
|---|---|---|
| Probe MCU | **STM32F042K6** (LQFP-32; NUCLEO-F042K6) (locked 2026-06-23, T-001) | Dev-board availability is the deciding constraint (NUCLEO-F042K6 is the board on hand). Adequate for v0.1 single-link bridge; **6 KB SRAM** is the design budget. F072 = upgrade path if a future rev needs more |
| Baud (SPBRG) | **Derive via Microchip MCP** from Fosc+baud, cited; manifest `brg` = optional override, `fosc` = input | Honors fact-from-source policy; removes hand-entry footgun |
| Tier source of truth | **Pack metadata** → *provisional* tier; **measured `.occ`** confirms/demotes | Codegen reads pack at build time; measurement is the arbiter ([02 §6](02-target-footprint.md)) |
| WRITE_MEM | Off by default; opt-in + codegen write blacklist | Safety on a live target ([01 §6](01-debug-link-protocol.md)) |
| Protocol | **Single `cdl_proto` spec** → generated Python codec + C header | Kills 3-way drift |
| Tier-A link default | **2-wire** (TX+RX → commands/breakpoints); 1-wire opt-in (omit `rx_pin`) | Full features out of the box |
| Probe build system | **CMake** | Scriptable from the helper |

## 4. Status — implemented vs. remaining

**Built & compile-verified this session** (`cc5x-helper`):
- `debuggen.py` + `debug-stub` CLI: config parse/validate, **metadata-driven**
  capability detection (EUSART regs/bits, RCIF/TRMT containing registers, free timer,
  RAM dedup, flash), tier selection (full/min/trace/toggle; provisional; force-down-only),
  `cdl_map` JSON, manifest `debug` passthrough.
- Stub emission: **Tier A full/min** (CRC8 + SLIP + EUSART TX + RX command loop:
  PING/READ_MEM/SET_BP/CLR_BP/CONTINUE + software breakpoints via FSR0/INDF0) and
  **Tier C fixed-site** toggle — both compile clean under real CC5X.
- Measured budget, PIC16F15244 Tier-A-full: **60 B RAM / 465 code words (11%)**.
- 283 tests pass; flake8 clean; code-reviewed (5 real bugs fixed).

**Remaining** (→ roadmap phase): single-source `cdl_proto`/codec (P1); compile-and-
measure gate + symbol-map population (P2); baud-via-MCP wiring (P3); `debug-monitor`
(P4); STM32 firmware (**P5a + P5b both build clean** — relay+init+native USB-CDC,
zero warnings; P5a VCP 2336 B flash / 2152 B RAM, P5b USB-CDC 3964 B flash / 2240 B
of 6 KB RAM; all T-001 checks verified, descriptors host-tested, pending bench);
end-to-end (P6); Tier-B bit-bang, Tier-C pulse-width/-train, target-tick,
WRITE_MEM+blacklist, bank-crossing read, READ_PGM, compression (P7).

## 5. Roadmap (CI-gated; P0–P4 need no hardware)

| Phase | Deliverable | Acceptance gate |
|---|---|---|
| **P0** | Master doc + reconcile 01–04 (this) | Docs internally consistent |
| **P1** | `cdl_proto.py` spec + `cdl_codec.py` (Python) + tests; debuggen imports the spec | Codec round-trips every message; matches stub's emitted C on golden vectors |
| **P2** | Compile-and-measure gate (CC5X only): build stubs for roomy/512 B/256 B parts, parse `.occ/.var/.asm/.fcs`, populate `cdl_map.symbols`, confirm/demote tier | CI green via CrossOver runner; map symbols filled; [02 §6](02-target-footprint.md) closed |
| **P3** | Baud via Microchip MCP: codegen computes SPBRG from `fosc`+baud, cites the datasheet URL; `brg` override kept | Unit test pins SPBRG to a known Fosc/baud table row |
| **P4** | `debug-monitor` PC command (decode + map + commands) | Decodes canned frames + renders named trace in CI |
| **P5a** | STM32F042K6 firmware bench bring-up (CMake+HAL/LL): target USART1 (PA9/PA10) → TIM2 timestamp → relay to **ST-LINK VCP** (USART2 PA2/PA15). No native-USB wiring needed | Relays + timestamps target frames to a PC terminal over the ST-LINK Virtual COM; LED (PB3) heartbeat |
| **P5b** | Swap PC transport to **native USB-CDC** on PA11/PA12; wire USB breakout to CN3-D10/D2. **Built:** bare-metal F042 USB FS device + CDC-ACM driver (`usb_cdc.c`/`usb_desc.c`), RM0091-cited, descriptor-unit-tested, compile-time `PROBE_PC_TRANSPORT` switch; cross-builds clean (3964 B flash / 2240 B of 6 KB SRAM). **Pending:** bench enumeration. | Enumerates as CDC; forwards + timestamps frames; buffers fit 6 KB SRAM |
| **P6** | End-to-end slice on PIC16F15244: trace → READ_MEM → breakpoints | Live timestamped trace; mem peek; bp stop/continue |
| **P7** | Deferred: Tier-B bit-bang, Tier-C pulse-width/-train + STM32 input-capture, target-tick, WRITE_MEM+blacklist, bank-crossing read, READ_PGM, compression | Each its own gate |

## 6. Baud sourcing (confirmed)

`search_microchip_product_documents` returns, for the PIC16F15213/14/23/24/43/44
family (covers 16F15244), the §"EUSART Baud Rate Generator (BRG)" content verbatim,
including the formula table and a citable `onlinedocs.microchip.com` URL:

| SYNC | BRG16 | BRGH | Mode | Baud |
|---|---|---|---|---|
| 0 | 0 | 0 | 8-bit async | Fosc / [64 (n+1)] |
| 0 | 0 | 1 | 8-bit async | Fosc / [16 (n+1)] |
| 0 | 1 | 1 | 16-bit async | Fosc / [4 (n+1)] |

`n` = SPBRGH:SPBRGL. Codegen picks the mode that minimizes error for the requested
Fosc/baud, emits `CDL_SPBRG_VALUE` (and BRGH/BRG16), and records the source URL in a
comment + the `cdl_map`. **The formula/value always comes from the MCP, never memory.**

## 7. Open items (track to closure)

- 1-wire vs 2-wire **default** — locked to 2-wire; revisit if pin pressure demands.
- STM32 **probe MCU** — **locked to F042K6** (2026-06-23, T-001). Deciding constraint:
  the available dev board is the **NUCLEO-F042K6**. Adequate for the v0.1 scope (one
  cooperative UART link, trace + timestamp + mem-read + breakpoints; compression deferred)
  — **6 KB SRAM is the binding design budget**, sufficient for a single-channel
  USB-CDC↔UART streaming bridge. Sources in repo (pdf-creator MCP / Read):
  `stm32f042k6.pdf` (DS DocID025832 R5), `rm0091-…pdf` (RM0091), `pm0215-…pdf` (Cortex-M0
  PM). Confirmed: 6 KB SRAM (HW parity), 32 KB flash, crystal-less USB FS (internal 48 MHz
  RC, no crystal), 5-ch DMA, 2 USART, one 32-bit timer. Upgrade path if a future rev
  outgrows it: F072 (16 KB SRAM, the reference design's native part) — not v0.1.
- Trace **compression** (delta/RLE) — deferred past v0.1.
- PIC16F15244 **flash discrepancy**: pack metadata 4096 w vs product feed 3584 w —
  measured `.occ` (465 w, fits A-full) is the arbiter; see [02 §4](02-target-footprint.md).

## 8. Repos & key paths

- Design docs: `cc5x-debug-probe/` (this repo).
- Generator + tools: `cc5x-helper/tools/cc5x_setcc_native_lib/debuggen.py`,
  `tools/cc5x_setcc_native.py` (`debug-stub`; future `debug-monitor`), `tests/`.
- CC5X compile gate: `cc5x-helper/tools/validate_generated_headers.py` (CrossOver runner).
- Baud/datasheet facts: **Microchip MCP** (`mcp__microchip__*`).
- Probe firmware: `cc5x-debug-probe/firmware/probe-f042/` (P5). Direct-register LL
  (not CubeMX/HAL). Relay core + RM0091-verified peripheral init + bare-metal USB FS
  CDC-ACM driver (`usb_cdc.c`/`usb_desc.c`) **cross-build clean** (arm-none-eabi-gcc,
  zero warnings) for both PC-link transports via the `PROBE_PC_TRANSPORT` switch
  (P5a VCP 2336 B flash / 2152 B RAM; P5b USB-CDC 3964 B flash / 2240 B of 6 KB RAM),
  host-tested (relay + USB descriptors), all T-001 checks verified — pending hardware
  bench. USART DMA stays deferred — see [04 §"Firmware framework"](04-software-stack.md).

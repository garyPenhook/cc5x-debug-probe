# Target-Side Footprint — per-device tier classification

Which CDL tier (`01-debug-link-protocol.md` §2) each CC5X-validated device can host,
based on **verified** Microchip resource specs. The target runs *cooperative* stub
code, so the question is purely "how much flash/RAM/peripheral can it spare."

> **Provenance:** all numbers below come from Microchip's `get_full_product_profile`
> MCP tool + the parametric feed it references (`microchipdirect.com/feed/json/<PART>.json`),
> fetched per device. They are **not** from memory. Two cells are inferred, not
> field-verified — flagged with † / ‡ and explained under "Caveats."

---

## 1. Verified resource table

| Device | Class | Flash (words) | RAM (B) | Pins | EUSART | SPI/I2C |
|---|---|---:|---:|---:|:--:|:--:|
| PIC10F200 | Baseline (12-bit) | 256 | 16 | 6/8 | No | No |
| PIC10F320 | Enh. mid-range | 256 | 64 † | 6/8 | No | No |
| PIC10F322 | Enh. mid-range | 512 | 64 | 6/8/14 | No | No |
| PIC12F1501 | Enh. mid-range | 1024 | 64 | 8 | No | No |
| PIC12F1840 | Enh. mid-range | 4096 | 256 | 8 | Yes (1) | MSSP |
| PIC16F1509 | Enh. mid-range | 8192 | 512 | 20 | Yes (1) | MSSP |
| PIC16F15244 | Enh. mid-range ‡ | 3584 | 512 | 20 | Yes (1) | SPI+I2C |
| PIC16F15313 | Enh. mid-range ‡ | 2048 | 256 | 8 | Yes (1) | SPI+I2C |
| PIC16F1789 | Enh. mid-range | 16384 | 2048 | 40/44 | Yes (1) | MSSP |
| PIC16F18325 | Enh. mid-range ‡ | 8192 | 1024 | 14/16 | Yes (1) | SPI×2+I2C×2 |
| PIC16F18446 | Enh. mid-range ‡ | 16384 | 2048 | 20 | Yes (1) | SPI×2+I2C×2 |
| PIC16F18857 | Enh. mid-range ‡ | 32768 | 4096 | 28 | Yes (1) | SPI×2+I2C×2 |
| PIC16F19195 | Enh. mid-range ‡ | 8192 | 1024 | 64 | Yes (2) | SPI+I2C |

---

## 2. Tier thresholds (design rule)

> **All RAM/flash floors below are provisional design rules, not measured facts.**
> They must be confirmed against real CC5X output before any device is *promised* a
> tier — see §6. The thresholds are starting estimates for the codegen, deliberately
> conservative, and the build-time validator downgrades a device whose **measured**
> budget (`.occ`/`.var`/`.fcs`) does not actually fit the tier it was provisionally
> assigned.

Tier A is split into two capability sub-tiers because a 256 B part and a 1 KB+ part
should not be promised the same feature set:

| Tier | Requires (provisional) | Gives |
|------|------------------------|-------|
| **A-full — full monitor** | EUSART + **RAM ≥ ~512 B** + **flash ≥ ~4 KW** | trace ring + mem read + (opt) mem write + software breakpoints + RX commands + (opt) target tick — all at once |
| **A-min — reduced monitor** | EUSART + **RAM ~256 B** + **flash ≥ ~2 KW** | RX commands + mem read + a **small** trace ring + breakpoints; the optional target tick and `WRITE_MEM` are dropped unless the measured budget proves they fit. **Never promised without a measured budget.** |
| **B — bit-bang trace** | **flash ≥ ~1 KW** + **RAM ≥ ~48 B** (no/!spare EUSART ok) | one-way trace + log; no commands, no breakpoints |
| **C — toggle/edge** | anything smaller, or baseline core | timestamped pin pulses only; companion does all timing |

Rationale for the RAM/flash floors: a full Tier-A stub needs a TX ring buffer
(tens of bytes) **plus** a command/breakpoint handler, the symbol-addressed
`READ_MEM` path, and optionally a target-tick timer. At ~512 B+ those coexist with
real app headroom (**A-full**). At ~256 B they contend hard — so **A-min** keeps the
RX/command/breakpoint core but drops the trace ring to a few entries and omits the
target tick / `WRITE_MEM` until measurement says otherwise; below that it crowds out
the application and the part drops to Tier B. Tier B drops the RX/command/breakpoint
machinery, so it fits a small bit-bang TX + tiny ring. Tier C needs essentially
nothing on the target — just a GPIO pulse — and pushes all timestamping to the
companion (which, on an RP2040/STM32, does it in hardware; see `03` §5).

---

## 3. Classification

### Tier A — monitor-capable (9 devices)
`PIC12F1840`, `PIC16F1509`, `PIC16F15244`, `PIC16F15313`, `PIC16F1789`,
`PIC16F18325`, `PIC16F18446`, `PIC16F18857`, `PIC16F19195`

All have a hardware EUSART. The **A-full / A-min** sub-tier (§2) follows from RAM:

- **A-full — roomy** (≥1 KB RAM): 16F1789, 16F18325, 16F18446, 16F18857, 16F19195 —
  full monitor with generous trace buffering; 19195 even has **2 EUSARTs**, so the
  debug link can take a dedicated UART and leave one for the application.
- **A-full — comfortable** (512 B): 16F1509.
- **A-full — measured-confirmed** (512 B): 16F15244. **Measured** with real CC5X
  (`debug-stub` Tier-A-full + the device header): **60 B RAM / 465 code words (11%)** —
  fits comfortably with ~90% flash and ~452 B RAM free for the app, so **A-full is
  confirmed**, not merely provisional. (Flash source note: pack metadata reports 4096
  words for this part, the product feed 3584; either way the measured 465-word stub
  fits — see §4. Codegen uses pack metadata.)
- **A-min — provisional, pending measurement** (256 B): 12F1840, 16F15313. A reduced
  monitor (RX + mem-read + small ring + breakpoints) is *expected* to fit, but this
  is **not promised** until a measured CC5X budget (§6) confirms it. The optional
  target tick and `WRITE_MEM` are **off** here unless the budget proves headroom. If
  the measured stub does not fit with usable app RAM, the device falls back to Tier B.

### Tier B — bit-bang trace (1 device)
`PIC12F1501` — 1 KW flash, **no EUSART**, only 64 B RAM. Fits a one-way bit-banged
trace + log; **no** commands/breakpoints (no RAM budget for the handler, no RX). The
64 B RAM is shared with the app, so trace channels must be few and narrow.

### Tier C — toggle/edge only (3 devices)
`PIC10F200`, `PIC10F320`, `PIC10F322` — too small and/or no EUSART:
- `PIC10F200`: 256 W flash, **16 B RAM**, baseline 12-bit core → pulse markers only.
- `PIC10F320`: 256 W flash, 64 B RAM → pulse markers only.
- `PIC10F322`: 512 W flash, 64 B RAM → pulse markers; *optionally* a minimal
  bit-bang single-channel trace could be squeezed in, but treat as Tier C by default.

On Tier C the target just calls `CDL_MARK(id)`; the **companion** timestamps the
edges in hardware and the PC tool maps edge-id → source location via a
codegen-emitted edge map.

#### Tier C edge-ID encoding (must be chosen at codegen time)

One GPIO edge carries no id by itself, so an id has to be coded into *time*. The
codegen picks one of three schemes per build (manifest `toggle.encoding`), trading
target cost against how many distinct sites you can mark:

| Scheme | How an id is sent | Sites | Target cost | When |
|--------|-------------------|------:|-------------|------|
| **fixed-site** (default) | each marked site gets its **own GPIO pin**; a single pulse on pin *n* = site *n* | = number of free pins | 1 pin per site, ~2 instr/mark | very few sites, lowest CPU/timing skew |
| **pulse-width** | one pin; high-time encodes the id in *k* quantized width buckets (e.g. 1×/2×/…/k× a base width `Tq`) | ~k (k≤8 practical) | 1 pin, a calibrated busy-delay per mark (blocks for up to k·Tq) | a handful of sites, 1 pin to spare |
| **pulse-train** | one pin; the id is sent as *n* short pulses (a count), framed by an inter-id gap ≥ `Tg` | many (id = pulse count) | 1 pin, blocks for n·Tp; longest probe effect | more sites than pins, can tolerate longer marks |

Rules the codegen and companion must agree on (all emitted into `cdl_map_<dev>.json`):

- **Timebase.** `Tq`/`Tp`/`Tg` are derived from the target `Fosc` (so the mark is a
  known instruction-cycle count) and emitted in the map in **microseconds** so the
  companion's hardware capture can classify widths/counts without knowing `Fosc`.
- **Quantization margin.** Width buckets are spaced ≥ 2× apart and `Tg` ≥ 2× the max
  intra-id spacing, so jitter/probe-effect can't alias one id into another; the
  companion rejects (and the PC tool reports) any edge it can't classify.
- **Probe effect is explicit.** pulse-width and pulse-train **block** the target for
  the encode duration — the map records the worst-case mark time per scheme so the
  user can see the timing cost. fixed-site is the cheapest and is the default.
- **id space.** `CDL_MARK(id)` ids are assigned densely from the edge map; the
  codegen refuses a build whose id count exceeds the chosen scheme's capacity (too
  many sites for the pins/buckets available) rather than silently truncating.

---

## 4. Caveats (from the verification pass)

- **† PIC10F320 RAM:** the parametric feed says **64 B**; one marketing description
  string read "32 B". Took the feed value (64) as authoritative; either way it's
  Tier C.
- **‡ Class inferred, not field-verified:** for
  `16F15244/15313/18325/18446/18857/19195` the feed's `InstructionWord` field was `0`
  (a data gap), so "enhanced mid-range" is inferred from part family (corroborated by
  PPS on 16F15244), not an explicit field. High confidence (these are
  well-known enhanced-midrange parts) but flagged for honesty. `PIC10F200` (12-bit)
  and `PIC12F1840` (description explicitly says "Enh Mid Range") are field-confirmed.
- **RAM bank count:** not exposed by these MCP tools for any device → the stub
  generator must read bank layout from the **pack metadata** (picmeta) at codegen
  time, not from this table.
- **PIC16F1789 "100-pin":** an ICE/emulator silicon artifact in the profile; real
  packages are 40-pin PDIP/UQFN and 44-pin QFN/TQFP.
- **Data EEPROM** (not needed here, recorded for completeness): 256 B on 12F1840,
  16F1789/18325/18446/18857/19195; 0 on the rest.
- **Flash word count, PIC16F15244 — source discrepancy:** the pack `.ini` `ROMSIZE`
  reports **4096 words**, the product feed's marketing string **3584 words (7 KB)**.
  Codegen uses the **pack metadata** (4096) since that is the toolchain's source of
  truth; the table here keeps the feed value for cross-reference. The discrepancy is
  moot for tiering: the measured Tier-A-full stub is **465 words / 60 B RAM** (§3, §6),
  which fits under either figure.
- **RAM byte count — mirrored common block:** the `.ini` lists the access/common RAM
  once per bank (e.g. 0x70-0x7F mirrored as 0xF0-0xFF, 0x170-0x17F, …). Summing those
  per-bank over-counts RAM by `(banks-1) × block`; the generator de-duplicates by
  address and counts the common block once (so 16F15244 = **512 B**, matching the
  feed). The table's RAM column is the de-duplicated GPR size.

---

## 5. Implication for codegen

`debuggen.py` (`03` §3) picks a **provisional** tier with `EUSART? + RAM + flash`
from pack metadata, cross-checked against the floors in §2, then **confirms it
against the measured CC5X budget** (§6) before the map promises a feature set.
Provisional mapping for this device set: **7 → A-full, 2 → A-min, 1 → Tier B,
3 → Tier C** (the two A-min parts, 12F1840 / 16F15313, plus the flash-on-the-line
A-full part 16F15244, are confirmed or demoted by measurement). The manifest may force a *lower* tier (e.g. Tier B trace on a Tier-A
part to save flash) but never a higher one than the resources allow — validation
rejects that, consistent with the project's "never persist validator-rejected
state" rule.

## 6. Tier confirmation by measured CC5X budget (acceptance gate)

A provisional tier is a hypothesis; the **measured** stub is the proof. Before any
device is promised a tier, the codegen build for it must be compiled and its CC5X
reports parsed (this is a zero-hardware gate — see `03` §7 / `04` §6):

- **`.occ`** — code + RAM occupation: does the stub's RAM + the reserved app
  headroom fit the device, and does code fit flash?
- **`.var`** — that every symbol the map references actually exists at the address
  claimed.
- **`.asm`** — that bank/`BSR`/`FSR` handling around `READ_MEM` is what the address
  model (`01` §6) assumes.
- **`.fcs`** — call-depth/stack use of the monitor path stays within the device's
  hardware stack.

The validator promotes the device to its provisional tier **only if** the measured
budget fits; otherwise it downgrades (A-full→A-min→B→C) and records why. This is the
gate that keeps the A-min row (§3) honest — a 256 B part is promised the reduced
monitor only after its `.occ` shows it fits with usable app RAM.

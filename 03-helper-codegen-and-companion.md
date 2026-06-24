# Helper Codegen + Companion Firmware — Architecture

How `cc5x-helper` generates a per-device target stub, what the fixed companion MCU
(the probe MCU — STM32 here, but the role is MCU-agnostic; §5) runs, and how the
PC-side tool decodes it. Pairs with `01-debug-link-protocol.md`
(the wire format) and `02-target-footprint.md` (which tier each device gets).

The design principle: **all device-specific knowledge lives in the generated target
stub + a build-time symbol/channel map. The companion firmware and the wire format
are device-independent.** That is what makes "one probe, every PIC I program" real.

```
   pack metadata (.PIC/.ini/.cfgdata)        setcc-native.json  (debug: {...})
            │  (picmeta.py)                          │ (project.py)
            ▼                                         ▼
        device model ───────────► debug-stub generator (new: debuggen.py)
                                          │
              ┌───────────────────────────┼───────────────────────────┐
              ▼                            ▼                            ▼
     cdl_monitor_<dev>.c/.h        cdl_map_<dev>.json          (build_options += stub)
     (CC5X-compiled into target)   (symbol+channel map → PC)
                                          │
                                          ▼
                                  cc5x-helper debug-monitor  ◄── USB-serial ── COMPANION MCU
                                  (decode + display + send commands)            (STM32, fixed firmware)
```

---

## 1. Where this slots into the existing helper

The helper already has the pieces this reuses:

- **`cc5x_setcc_native_lib/picmeta.py`** — normalized device model (SFRs, RAM ranges,
  banks, arch). The stub generator reads UART SFR names/addresses, free RAM, timers,
  and arch class from here — *no hardcoded register addresses*.
- **`cc5x_setcc_native_lib/headergen.py`** — precedent for emitting CC5X-safe C from
  device metadata (the `_safe_identifier`/`_safe_comment` discipline applies to any
  pack-derived names the stub embeds).
- **`cc5x_setcc_native_lib/project.py`** — manifest model; add a `debug` section.
- **`intellisense` / `vscode-tasks` commands** — precedent for "a CLI subcommand that
  generates a file from the project + device model and emits `{ok:..}` JSON."
- **`.var` / symbol output** — CC5X already emits variable→address info (`-V`); the
  generator parses it to build the symbol map (CDL §6).

New code: a `debuggen.py` library module + a `debug-stub` CLI command, mirroring how
`headergen.py` backs header generation. No change to the wire protocol per device.

---

## 2. Manifest additions (`setcc-native.json`)

```jsonc
"debug": {
  "enabled": true,
  "tier": "auto",            // auto | full | trace | toggle (auto = pick from 02-footprint)
  "transport": {
    "uart": "auto",          // auto = use device EUSART if present; else bitbang
    "tx_pin": "RC6",         // used for bitbang / to pin the EUSART TX
    "rx_pin": "RC7",         // optional; omit → one-way (no host→target commands)
    "fosc": 32000000,        // required for monitor tiers: SPBRG is computed from this
    "baud": 115200,          // target baud; SPBRG derived from fosc+baud via Microchip MCP
    "brg": null              // optional explicit SPBRG override (skips the computation)
  },
  "target_timestamp": "auto",// auto = use a free 16-bit timer if available (CDL §5 layer 2)
  "toggle": {                // Tier-C only (ignored otherwise); see 02 §3 edge-ID encoding
    "encoding": "fixed-site",// fixed-site | pulse-width | pulse-train
    "pins": ["RA0"]          // fixed-site: one pin per marked site
  },
  "channels": [              // named trace channels → ids (CDL §4 TRACE.ch)
    { "name": "state",  "width": 1 },
    { "name": "adc",    "width": 2 },
    { "name": "errflag","width": 1 }
  ],
  "breakpoints": "software"  // software | none
}
```

Validation (reuses the audit-hardened `validate_project_file` pattern): reject a tier
the device can't support **or that the measured CC5X budget doesn't actually fit**
(`02` §6), an `rx_pin` with no spare pin, a baud the `Fosc` can't hit within UART
error tolerance, more channels than the chosen tier allows, more Tier-C marked sites
than the chosen `toggle.encoding`/pins can carry (`02` §3), and `WRITE_MEM` requested
without the explicit per-build opt-in (off by default, `01` §6). The mutation
commands must refuse to persist an impossible debug config (same "don't write
validator-rejected state" rule already enforced for editions/config).

---

## 3. The target stub generator (`debuggen.py`)

Input: device model (picmeta) + `debug` manifest section. Output: a CC5X-includable
`cdl_monitor_<dev>.c` + `.h` and a `cdl_map_<dev>.json`.

Decision flow:

1. **Pick tier** (if `auto`): look the device up in the `02-target-footprint.md`
   class table (full / trace / toggle) derived from *verified* flash/RAM/peripherals.
2. **Pick transport:** EUSART present and free → Tier A 2-wire (or 1-wire if no
   `rx_pin`). No EUSART → bit-banged TX (Tier B). Toggle class → emit only the
   GPIO-toggle macros + the edge map (no framing).
3. **Allocate RAM:** size the TX ring buffer and command scratch from the device's
   free RAM (leave a configurable headroom, e.g. keep ≥50% RAM for the app). Refuse
   if it can't fit the minimum.
4. **Wire registers from metadata:** TXREG/RCREG/SPBRG/baud config (register names and
   control-bit positions detected from pack metadata; the **SPBRG value is computed from
   the manifest `fosc`+`baud` using the EUSART baud formula fetched from the Microchip
   MCP**, with the datasheet URL recorded as provenance — never guessed), timer for the
   optional target tick, the chosen pins' TRIS/LAT/PPS (enhanced-midrange PPS is
   read from pack metadata, not assumed).
5. **Emit the API the user calls in their app:**
   - `CDL_TRACE(ch_name, value)` → push a `TRACE` frame (compile-time channel id).
   - `CDL_LOG("msg")` → `LOG` frame.
   - `CDL_BP(id)` → breakpoint marker (Tier A); when enabled at runtime, emits
     `BP_HIT` and spins in the monitor poll loop until `CONTINUE`.
   - `CDL_POLL()` → service inbound commands (call in main loop or from RX ISR).
   - `CDL_INIT()` → bring up the link.
   For toggle class these collapse to `CDL_MARK(id)`, emitted per the chosen
   `toggle.encoding` (fixed-site pin / pulse-width / pulse-train; `02` §3), with the
   id→site map and timebase written into `cdl_map_<dev>.json`.
6. **Emit `cdl_map_<dev>.json`:** protocol version, device id, tier, channel
   name→id+width, and `symbol→file-register address` (from `.var`) for `READ_MEM`.
   This is the file the PC tool loads to turn raw frames into named, addressed data.

CC5X-specific care: keep the stub small and predictable — no recursion, static
allocation only, ISR-safe ring buffer (single-producer/single-consumer), and honor
the `_safe_identifier` rules for any pack-derived names embedded in comments/IDs.

---

## 4. Build integration

A new subcommand, consistent with the existing ones:

```bash
cc5x-helper debug-stub --project setcc-native.json   # writes cdl_monitor_<dev>.{c,h} + cdl_map_<dev>.json
```

- `build` (when `debug.enabled`) adds the generated `.c` to the compile and defines
  e.g. `-DCDL_ENABLED`; a non-debug build omits it entirely (zero footprint when off).
- Regenerate is idempotent and atomic (reuse `atomic_write_text`); the map is
  rewritten whenever channels/symbols change so the PC decoder never drifts from the
  firmware.
- `--json` contract honored like the other commands (wrap in `json_error_boundary`).

---

## 5. Companion firmware (the probe MCU)

**Fixed, device-independent, and target-family-independent.** It knows the CDL wire
format and nothing about the target's symbols. Responsibilities:

1. **Link RX/TX** to the target (UART, the negotiated baud) — or **edge-capture** a
   Tier-C target's toggle pin.
2. **Timestamp** every received frame / captured edge from a free-running timer
   (CDL §5 layer 1).
3. **Reframe to the PC** over USB-CDC, adding the wall-clock stamp; CDL-in,
   CDL-plus-timestamp-out.
4. **Relay commands** PC→target (READ_MEM/SET_BP/CONTINUE…), pass ACK/NAK back.
5. **Buffer** to absorb bursts so trace isn't lost during PC hiccups.

### Probe MCU choice — the companion is NOT constrained to PIC

The companion can be any MCU; nothing about the CDL protocol or the per-device target
stub depends on it (that's the whole point — see the payoff note below). Picking the
best-fit part for a *smart USB trace probe*, with verified specs:

| Probe MCU | Why / why not | Verdict |
|-----------|---------------|---------|
| **RP2040** (recommended) | **264 KB SRAM** (huge trace buffer), **8 PIO state machines** (implement the CDL UART at any baud, run multiple links at once, and hardware-timestamp Tier-C toggle edges with zero CPU), **native USB 1.1 FS device**, dual Cortex-M0+ (one core links the target, one talks USB). Cheap, ubiquitous, great open toolchain (Pico C SDK / MicroPython). Effectively purpose-built for this. | **Best fit** |
| **STM32** (e.g. F4/G0/F1) | **Timer input-capture + DMA timestamps edges in hardware at µs accuracy** (excellent for Tier-C edge capture and precise target-timing), native USB on many parts, multiple UARTs, ample RAM on mid/high parts. More config overhead (HAL/CubeMX), ST ecosystem. | Strong alternative, esp. if you want hardware input-capture precision |
| **AVR** (e.g. 32U4 / AVR-DU) | USB on select parts, but comparatively small RAM (limited buffering) and **no programmable-IO fabric** — weaker for high-rate / multi-channel trace. (Exact AVR RAM/USB figures not verified here.) | OK for a minimal one-link probe; not the best |
| **PIC with USB** (PIC16F1-USB / PIC18 USB) | Keeps it all-in-family ("a second PIC"), but heavier USB stack, less RAM than RP2040, and no PIO/PIO-equivalent. | Only if staying all-PIC is a hard requirement |

**Chosen probe: STM32 (per project preference for ST products).** STM32 covers every
probe requirement — native USB-CDC, a UART (with DMA) to the target, and **timer
input-capture + DMA** that timestamps Tier-C toggle edges in hardware (µs accuracy),
so even a baseline PIC10F gets precise, low-overhead trace. The concrete STM32 part,
firmware framework, and full software stack are specified in
**[04-software-stack.md](04-software-stack.md)** (locked: STM32F042K6, LQFP-32 /
NUCLEO-F042K6, USB device core; port the F072-targeted DMA UART↔USB-CDC reference and
verify RAM/flash fit — F072/F411 are fallbacks; CubeMX + HAL/LL).

(RP2040 remains the technically strongest part for this role if you were
PIC/AVR/ARM-agnostic — its PIO fabric is ideal — but ST is the chosen path here, and
STM32 meets every requirement.)

Because the companion is **fixed and target-independent**, you build it **once**.
Every new target you program just gets a freshly generated stub + map — no companion
changes, regardless of which probe MCU you chose. That is the payoff, and it's exactly
why the probe MCU can be PIC/AVR/ARM without touching the rest of the system.

---

## 6. PC-side tool (`cc5x-helper debug-monitor`)

```bash
cc5x-helper debug-monitor --map cdl_map_16F1509.json --port /dev/ttyACM0
```

- Opens the companion's serial port, decodes CDL frames.
- Uses `cdl_map_*.json` to render `TRACE ch0=0x2A` as `state = RUNNING @ 12.412ms`
  and to resolve `read myVar` → `READ_MEM 0x0020`.
- Live view: timestamped trace log, watch expressions (periodic `READ_MEM`),
  breakpoint set/continue, memory peek/poke (Tier A).
- Reuses the helper's existing JSON/CLI conventions; could later feed a VS Code view,
  but the CLI is the MVP and is IDE-independent (the whole point).

---

## 7. Build/skeleton order (suggested)

> **Status (see `00-master-plan.md` §5 for the authoritative roadmap):** step 2 is
> **DONE** — `debuggen.py` + `debug-stub` emit Tier-A full/min and Tier-C fixed-site
> stubs, compile-verified with real CC5X (PIC16F15244: 60 B RAM / 465 words). Step 1
> (single-source `cdl_proto`/codec) and step 3 (the CI compile-and-measure gate) are the
> next work. Steps 4–6 (probe firmware, `debug-monitor`, Tier-B/extra Tier-C) follow.

1. **Protocol lib, host-side** (pure Python encode/decode + unit tests) — no hardware
   needed; lets the wire format be tested in CI like the rest of the suite. *(next)*
2. **`debuggen.py` + `debug-stub` command** emitting Tier-A C for one device
   (a verified full-monitor part from `02`), plus the map. **✅ DONE** (Tier-A full/min
   + Tier-C fixed-site; one-shot manual CC5X compile verified).
3. **Compile-and-measure acceptance gate (zero hardware) — do this before any
   firmware/probe work.** Generate *and actually compile with CC5X* the stub for
   **three** representative parts — one **roomy A-full** (e.g. 16F1789), one **512 B
   A-full** (16F1509), and one **256 B A-min** (12F1840 or 16F15313) — then parse the
   CC5X reports and assert the budget fits the promised tier (`02` §6):
   - `.occ` → RAM + code occupation vs device + reserved app headroom,
   - `.var` → every mapped symbol exists at the claimed address,
   - `.asm` → bank/`BSR`/`FSR` handling around `READ_MEM` matches `01` §6,
   - `.fcs` → monitor call depth fits the hardware stack.
   This proves the footprint *and the codegen correctness* with no probe and no
   target wired up, and it is the gate that confirms/demotes the A-min row in `02`.
   Make it a CI job (CC5X is the only dependency).
4. **Companion firmware** (start as a dumb pass-through + timestamp; add command relay).
5. **`debug-monitor`** PC tool against a real target.
6. Tier B (bit-bang) and Tier C (toggle) generators.
7. Manifest `debug` section + validation, wired into `build`.

Each layer is independently testable; layers 1, 2, 3, 7 are CI-testable without
hardware (layer 3 needs only CC5X, not a programmer or probe), matching the
project's existing unit-test discipline. Real hardware only enters at layer 4.

---

## 8. Risks / honest caveats

- **Probe effect** (CDL §1): the stub changes timing and uses a pin/RAM/flash. Bugs
  that depend on exact cycles may move. Document per-tier overhead in the map.
- **Software breakpoints only** — no silicon watchpoints; `CDL_BP` must be compiled
  in at the points you might stop.
- **Baseline parts get Tier C only** — toggle/edge trace, not a monitor (see `02`).
- **You own this stack.** It's bounded and Microchip-independent, but it is real
  firmware + tooling to maintain. The trade vs. the flaky MPLAB plugin is: more to
  build, but it actually works on every modern target and never regresses under you.

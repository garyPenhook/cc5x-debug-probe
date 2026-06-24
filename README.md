# CC5X Debug Probe — design docs

A cooperative debug/trace system for CC5X-built firmware on modern PIC, built because
**no fully-working interactive source-level debugger exists for CC5X on enhanced-midrange
PIC**: the MPLAB X + CC5X plugin only "works partially" on the current MPLAB X (6.30) and
rides the deprecated NetBeans IDE; gpsim doesn't cover the modern (Five-Digit F15xxx/F18xxx)
parts; open-source sims are core-only; and a true GDB path is infeasible for this silicon
(proprietary ICSP debug-exec, no 8-bit-PIC gdbserver, COFF/COD not DWARF).

## The idea

A **second micro** can't be a *universal* hardware debugger (that's a PICkit/SNAP and needs
Microchip's proprietary debug protocol). But it makes an excellent **cooperative trace/
monitor probe**: the target reserves a link and runs a small generated stub; a fixed
companion MCU timestamps, buffers, and forwards to a PC. It depends on **nothing** from
Microchip's debug stack, so it works on every modern target — which is exactly why it's
worth building here.

The **companion (probe) MCU is not constrained to PIC** — it can be PIC/AVR/ARM. The
chosen part here is **STM32** (project preference for ST): native USB-CDC, UART+DMA to
the target, and timer input-capture+DMA for hardware edge-timestamping. The choice
doesn't affect the protocol or the per-device target stub — see doc 03 §5. The full
software stack (STM32 part, firmware framework, PC tool, shared-protocol codegen) is in
doc 04.

## Documents

**Start here: [00-master-plan.md](00-master-plan.md)** — the whole-system design,
locked decisions, current status (what's built), and the phased CI-gated roadmap.
Docs 01–04 below are the detailed references.

1. **[01-debug-link-protocol.md](01-debug-link-protocol.md)** — the CDL wire protocol:
   SLIP/CRC8 framing, message types (trace / mem read-write / software breakpoints / log),
   two-layer timestamps, the Harvard/banked address model, reliability/resync.
2. **[02-target-footprint.md](02-target-footprint.md)** — per-device resource budget
   (verified flash/RAM/peripherals from Microchip docs) and the tier each device gets:
   **full monitor** (enhanced-midrange) vs **trace** vs **toggle-only** (baseline).
3. **[03-helper-codegen-and-companion.md](03-helper-codegen-and-companion.md)** — how
   `cc5x-helper` generates the per-device target stub from pack metadata + manifest, the
   fixed companion firmware, and the PC-side `debug-monitor` decoder.
4. **[04-software-stack.md](04-software-stack.md)** — the three codebases (PIC stub /
   STM32 probe / Python PC tool), STM32 part + firmware framework choice, and the
   single-source CDL protocol codegen that keeps all three in sync.

## Capability summary (honest)

| You get | You don't get |
|---------|---------------|
| Named trace events with timestamps | Transparent hardware breakpoints/watchpoints |
| On-demand memory **read** (Tier A); write off by default | Silicon single-step |
| Software breakpoints (halt-in-monitor, Tier A-full/A-min) | Zero overhead (probe effect is real) |
| Works on every target with spare resources | A monitor on tiny baseline parts (toggle-only there) |
| Microchip-debug-stack-independent, never regresses under you | A free lunch — it's firmware+tooling you build and own |

## Status

**In progress.** The target-stub generator is built: `cc5x-helper`'s `debuggen.py` +
`debug-stub` command emit Tier-A (full/min) and Tier-C (fixed-site) CDL stubs + the
`cdl_map`, compile-verified with real CC5X (PIC16F15244: 60 B RAM / 465 code words).
The **STM32F042K6 probe firmware** cross-compiles clean (`firmware/probe-f042/`,
arm-none-eabi-gcc, zero warnings, host-tested) for **both** PC-link transports:
**P5a** ST-LINK VCP (2336 B flash / 2152 B RAM) and **P5b** native USB-CDC (3964 B
flash / 2240 B of 6 KB RAM) — the bare-metal F042 USB FS device + CDC-ACM driver is
RM0091-cited and descriptor-unit-tested, pending hardware bench. Remaining work
(single-source protocol/codec, the CI compile-and-measure gate, the `debug-monitor`
PC tool) is sequenced in [00-master-plan.md](00-master-plan.md) §5. Host-side pieces
are CI-testable without hardware, matching the `cc5x-helper` project's unit-test
discipline.

> Scope note: the target families here are the CC5X-validated set in the project's
> `SUPPORT_MATRIX.md` (PIC10F/12F/16F). Device resource facts in doc 02 are taken from
> Microchip's published specs, not from memory.

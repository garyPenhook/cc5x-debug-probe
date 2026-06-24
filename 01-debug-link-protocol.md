# CC5X Debug Link (CDL) — Protocol Specification v0.1

Status: draft / design. This defines the wire protocol between a **target PIC**
(running a small generated monitor/trace stub built with CC5X) and a **companion
MCU** (the *probe MCU* — a fixed debug probe that timestamps, buffers, and forwards
to a PC). The probe MCU is not constrained to PIC (STM32 is the chosen part; see
doc 03 §5); the protocol below is identical regardless of which MCU runs the probe.

It deliberately depends on **nothing** from Microchip's ICD/ICSP debug-executive
stack. The target *cooperates*: it reserves a link and runs stub code. That is the
whole reason this works on modern enhanced-midrange parts where the MPLAB plugin
debugger is only partially functional.

---

## 1. Goals and non-goals

**Goals**
- Stream named **trace** events (a value on a channel) from target → companion.
- Let the host **dump/read/write target memory** on demand.
- Support **software breakpoints**: target halts in the monitor and waits for release.
- Attach **timestamps** so trace can be correlated in time.
- Be cheap enough to fit an enhanced-midrange PIC; degrade gracefully on tiny parts.

**Non-goals (be honest about these)**
- No transparent **hardware** breakpoints/watchpoints or silicon single-step — that
  needs the proprietary debug hardware. CDL offers *software* breakpoints + trace.
- Not zero-overhead. The stub consumes flash/RAM, a pin or peripheral, and CPU
  cycles — the classic **probe effect**. Timing-critical bugs can shift.
- Not lossless for trace by default (see §7 flow control). Commands *are* reliable.

---

## 2. Topology and transport

```
  +-----------------+   CDL link    +------------------+   USB-CDC / UART   +-------+
  |  TARGET PIC     | <===========> |  COMPANION MCU   | <================> |  PC   |
  |  (CC5X + stub)  |  half-duplex  | (STM32, fixed FW)|                    | tool  |
  +-----------------+               +------------------+                    +-------+
       cooperative                    timestamps + buffers              cc5x-helper
       monitor/trace                  + reframes to host                debug-monitor
```

Three transport tiers, chosen per device class (see `02-target-footprint.md`):

| Tier | Target requirement | Link | Capability |
|------|--------------------|------|------------|
| **A — Full monitor** | Hardware EUSART + spare RAM/flash | 2-wire UART (TX/RX) | trace + mem read (write off by default) + software breakpoints — A-full/A-min capability split per RAM, see `02` §2 |
| **B — Trace** | Spare flash/RAM, no/!spare EUSART | 1-wire bit-banged UART (TX only) | trace + one-way log (no commands) |
| **C — Toggle** | Almost no resources (baseline) | 1 GPIO, raw edges | timestamped event markers only (no framing) |

Tiers A/B speak the framed protocol below. Tier C is **out of band**: the target
just toggles a pin at instrumented points and the companion timestamps edges and
maps them to source locations via a codegen-emitted edge map. Tier C is documented
in `02-target-footprint.md`, not here.

**Link electricals/baud** are negotiated at build time (manifest), not on the wire.
The companion is told the baud by the PC tool; the target stub's baud divisor (SPBRG)
is **computed from a manifest-supplied `Fosc`** using the EUSART baud formula obtained
from the **Microchip MCP** (`search_microchip_product_documents`), with the datasheet
URL recorded as provenance, so both ends agree. `Fosc` is *not* in pack metadata (it is
a runtime oscillator setting) so it must be supplied; a manifest `brg` value overrides
the computed one. See `00-master-plan.md` §6.

---

## 3. Frame format (Tiers A/B)

Byte-oriented, SLIP-style framing so the delimiter is unambiguous and a desynced
receiver resynchronizes on the next delimiter.

```
  FLAG  payload (stuffed)                                   FLAG
  0x7E  [TYPE] [SEQ] [LEN] [ ARG bytes ... ]  [CRC8]         0x7E
        \_______________ covered by CRC8 ______________/
```

- `FLAG = 0x7E` delimits frames. A frame is the bytes between two FLAGs.
- **Byte stuffing (SLIP):** inside a frame, `0x7E → 0x7D 0x5E`, `0x7D → 0x7D 0x5D`.
  Unstuff on receive before CRC.
- `TYPE` (1 byte): the full message type (see §4). There is **no** version nibble in
  the frame header — protocol version is carried **only** in `HELLO.ver` (§9), so
  every other frame spends one fewer byte and the header is fixed-shape. (Earlier
  drafts proposed a `VER|TYPE` split; v0.1 deliberately drops it.)
- `SEQ` (1 byte): per-direction sequence counter, wraps. Lets the host detect
  dropped trace frames.
- `LEN` (1 byte): number of ARG bytes (0–255). Frames are small by design.
- `CRC8` (1 byte): poly `0x07` (CRC-8/SMBUS-style), init `0x00`, over
  `TYPE..last ARG`. Cheap table-free on PIC.

Rationale: SLIP + CRC8 is the cheapest framing that survives a noisy half-duplex
line and a target that may reset mid-frame. No length-prefixed sync ambiguity.

---

## 4. Message types

`TYPE` byte. Direction T→H = target→host(companion/PC), H→T = host→target.

### Target → Host
| TYPE | Name | ARG payload | Notes |
|------|------|-------------|-------|
| 0x01 | `HELLO` | ver, device-id(2), caps(1), tier(1), trace-ch-count(1) | sent on boot/attach; caps bitfield: bit0 mem-read, bit1 mem-write, bit2 sw-breakpoints |
| 0x02 | `TRACE` | ch(1), tstamp-lo(opt), value(1–4) | one trace event; value width is per-channel (fixed at codegen) |
| 0x03 | `LOG` | ascii bytes | free-form text (printf-style) |
| 0x04 | `MEM_DATA` | addr(2), bytes... | reply to `READ_MEM` |
| 0x05 | `BP_HIT` | bp-id(1), pc(2, opt), nframe(opt) | target has stopped in monitor; now polling for commands |
| 0x06 | `ACK` / 0x07 `NAK` | ref-seq(1)[, code] | reliability for H→T commands; NAK `code`: 1=BAD_ADDR, 2=SIDE_EFFECT, 3=WRITE_DENIED, 4=BAD_LEN, 5=UNKNOWN_TYPE |

### Host → Target  (Tier A only)
| TYPE | Name | ARG payload | Notes |
|------|------|-------------|-------|
| 0x81 | `PING` | — | liveness; target replies HELLO or ACK |
| 0x82 | `READ_MEM` | addr(2), len(1) | bank-resolved file-register address (see §6) |
| 0x83 | `WRITE_MEM` | addr(2), bytes... | **off by default** (cap bit1; see §6 write blacklist) |
| 0x84 | `SET_BP` | bp-id(1) | enable a codegen-placed breakpoint marker |
| 0x85 | `CLR_BP` | bp-id(1) | |
| 0x86 | `CONTINUE` | bp-id(1) | release target from a `BP_HIT` spin loop |
| 0x87 | `SET_TRACE` | ch(1), on(1) | enable/disable a trace channel at runtime |

All H→T commands are ACK/NAK'd by `ref-seq`. T→H trace/log are fire-and-forget.

### Probe → Host  (companion-originated envelopes)

The companion/probe (not the target) may originate its own frames toward the PC,
using the same FLAG/SLIP/CRC8 framing. **`TYPE` range `0xF0–0xFF` is reserved for
probe-origin envelopes** so it never collides with target (`0x01–0x0F`) or host
(`0x81–0x8F`) types.

| TYPE | Name | ARG payload | Notes |
|------|------|-------------|-------|
| 0xF0 | `RELAY` | tstamp(4, LE µs), data(1–N) | probe-timestamped target bytes (burst-stamped); the firmware P5a envelope |
| 0xF1 | `STATUS` | dropped(4, LE) | cumulative bytes the probe lost (ring/USART overrun) — keeps captures from being silently lossy |

These are implemented by `firmware/probe-f042/src/relay.c` and decoded by
`tools/relay_decode.py`; they fold into the single-source `cdl_proto` codec at P1.

---

## 5. Timestamps (two layers)

The tiny target should not be burdened with a real-time clock, so timestamping is
**primarily the companion's job**, with an optional target-side fine counter:

1. **Companion wall-clock (authoritative ordering):** the companion stamps every
   received frame from a free-running timer (e.g. 1 µs tick). This is what the PC
   tool uses to order/space events. Always present; costs the target nothing.
2. **Target cycle counter (optional, Tier A):** if the stub can spare a 16-bit
   timer (e.g. TMR1), `TRACE` may carry a 2-byte target tick so intra-target timing
   between two events is exact regardless of link/buffer latency. Advertised in
   `HELLO` caps; the codegen decides based on free timers.

The PC tool reconciles: companion stamp = global order; target tick (when present)
= precise delta within a burst. Link latency therefore never corrupts *relative*
target timing when layer 2 is available.

---

## 6. Address model (Harvard / banked)

PIC data memory is banked file registers; program memory is word-addressed and
separate. CDL `READ_MEM`/`WRITE_MEM`/`MEM_DATA` operate on a **flat file-register
address** that the stub maps to the correct bank select before access. The codegen
knows each symbol's absolute file-register address (from the CC5X `.var`/symbol
output) and emits a **symbol → address map** consumed by the PC tool, so the user
reads `myVar` and the tool sends the resolved address. Program-memory reads are a
separate optional `READ_PGM` (reserved TYPE 0x88) and are not in v0.1.

The flat-address encoding and bank rules are **per arch class** and are taken from
**pack metadata** (`picmeta`) at codegen time, never assumed:

- **Address encoding.** The flat address is the device's physical file-register
  address. The stub derives the bank-select value and in-bank offset from the
  arch's bank size (read from pack metadata), and reloads `BSR`/bank bits before
  each access. The PC tool only ever deals in symbol names → flat addresses; it
  does not compute bank arithmetic.
- **Bank crossing.** A multi-byte `READ_MEM`/`WRITE_MEM` that would cross a bank
  boundary is split by the stub into per-bank accesses (re-selecting the bank for
  each run); the host sees one contiguous `MEM_DATA`. A range whose start is not a
  valid file register is NAK'd.
- **Common (access) RAM.** Addresses in the device's mirrored/common region (the
  same bytes visible in every bank, per pack metadata) are read without bank
  switching. The codegen marks these in the map so the host knows they are
  bank-invariant.
- **SFR side effects.** Reading some SFRs has side effects (e.g. clear-on-read
  flags, FIFO/buffer registers, ADC result latches). The codegen emits a
  **read-sensitive SFR list** from pack metadata; by default `READ_MEM` of those
  addresses is **refused** (NAK `code=SIDE_EFFECT`) unless the manifest opts in
  per address. Plain GPR/symbol reads are always allowed.
- **Volatile symbols.** Symbols the application marks volatile (or that alias an
  SFR) are tagged in the map so the host shows them as "live" and does not cache a
  stale value between polls.
- **Indirect access.** v0.1 reads only **direct** file-register addresses. It does
  **not** dereference `FSR`/`INDF` or follow pointers; reading a pointer variable
  returns the pointer's bytes, not its target. Pointer-following is a host-side
  concern (issue a second `READ_MEM`), keeping the stub trivial.
- **Write safety / blacklist.** `WRITE_MEM` is **disabled by default** (cap bit1
  clear) and must be explicitly enabled in the manifest. Even when enabled, the
  codegen emits a **write blacklist** — config/calibration words, the stub's own
  state (ring buffer, SEQ, link SFRs), the stack, and read-sensitive SFRs — and the
  stub NAKs (`code=WRITE_DENIED`) any write that targets a blacklisted address. The
  host cannot widen the blacklist past what the build allows.

---

## 7. Reliability, flow control, framing recovery

- **Trace/log (T→H):** unacknowledged. CRC8 lets the companion drop a corrupt
  frame; `SEQ` gaps let the PC tool report "N events lost" rather than silently
  miss them. A full target TX buffer drops oldest trace (ring buffer) and sets an
  `overrun` flag reported in the next `LOG`/`HELLO`.
- **Commands (H→T):** stop-and-wait with ACK/NAK keyed on `SEQ`; host retransmits
  on timeout. Simple and tiny.
- **Resync:** any receiver that sees a malformed/CRC-failed frame discards bytes
  until the next `FLAG`. A mid-frame target reset is invisible beyond one dropped
  frame.

---

## 8. Sessions / lifecycle

1. Target boots → emits `HELLO` (device-id, tier, caps, channel count).
2. Companion forwards `HELLO`; PC tool loads the matching **symbol/channel map**
   (emitted by the helper for that build) and begins decoding.
3. Steady state: target streams `TRACE`/`LOG`; host may issue `READ_MEM`, set
   breakpoints (Tier A).
4. Breakpoint: target hits a `SET_BP`-enabled marker → sends `BP_HIT` → spins in
   the monitor servicing `READ_MEM`/`WRITE_MEM` until `CONTINUE`.

---

## 9. Versioning

`HELLO.ver` carries protocol major.minor. The PC tool refuses mismatched majors.
v0.1 reserves: TYPE 0x88 (`READ_PGM`), 0x08–0x0F (T→H), 0x88–0x8F (H→T), and
**0xF0–0xFF (probe→host envelopes, §4)** for future use. Unknown TYPEs are NAK'd
(H→T) or ignored with a logged warning (T→H).

---

## 10. Worked example (Tier A trace + breakpoint)

```
T→H  7E 01 00 06  01 16 09 07 01 03  CRC 7E      HELLO ver1, dev 0x1609(=16F1509-ish id),
                                                  caps=0b111, tier=A, 3 channels
T→H  7E 02 01 03  00 2A 11        CRC 7E         TRACE ch0 value=0x2A (target tick 0x11.. if layer2)
H→T  7E 84 00 01  05              CRC 7E         SET_BP bp-id 5
T→H  7E 06 00 01  00              CRC 7E         ACK ref-seq 0
... target reaches marker 5 ...
T→H  7E 05 02 03  05 1C 80        CRC 7E         BP_HIT bp5 at pc=0x801C
H→T  7E 82 01 03  20 00 04        CRC 7E         READ_MEM addr 0x0020 len 4
T→H  7E 04 03 06  20 00 DE AD BE EF CRC 7E       MEM_DATA addr 0x0020 = DE AD BE EF
H→T  7E 86 02 01  05              CRC 7E         CONTINUE bp5
```

(CRC bytes shown as `CRC`; SEQ/stuffing simplified for readability.)

---

## 11. Open questions to resolve before v1.0

- Single-wire vs 2-wire default for Tier A (RX costs a pin; many bugs need only T→H).
- ~~Whether `WRITE_MEM` is worth the risk on a live target~~ — **resolved:** off by
  default, gated by a per-build opt-in + codegen write blacklist (§6).
- Baud ceiling per device class given `Fosc` and bit-bang vs EUSART (decide in `02`).
- Compress trace (delta/RLE on a channel) for high-rate signals? (post-v0.1.)

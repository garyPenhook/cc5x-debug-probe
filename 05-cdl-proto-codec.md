# P1 — Single-Source CDL Protocol Codec (Design)

Status: design / ready to implement. Master-plan phase **P1** (`00-master-plan.md` §5).
This doc is grounded in the real code as it stands on 2026-06-24:

- **Wire spec:** `01-debug-link-protocol.md` (frame format, message types, CRC).
- **Single-source decision:** `04-software-stack.md` §4.
- **Target codegen (today):** `cc5x-helper` →
  `tools/cc5x_setcc_native_lib/debuggen.py` (constants `:41–105`; C-header emit
  `_render_header` `:604–665`; CRC/framer `_CRC_LINES`/`_FRAMER_LINES` `:668–728`).
- **Probe codec (today):** this repo → `firmware/probe-f042/src/relay.c` (encoder)
  and `firmware/probe-f042/tools/relay_decode.py` (decoder).

> **P1 is a refactor to a single source of truth, NOT a wire-format change.** The
> framing already shipped in two independent implementations that agree on the
> wire (see §2). P1 must reproduce the existing bytes exactly — the existing
> golden artifacts (`debuggen` golden headers, the probe golden frame in
> `tests/test_relay.c`) are the regression floor.

---

## 1. Scope

**P1 delivers**, all inside `cc5x-helper` (the home of the existing codegen):

1. `cdl_proto.py` — one declarative spec: framing constants, CRC params, the
   three message-type groups, and per-message ARG field layouts.
2. `cdl_codec.py` — a pure-Python encoder/decoder generated-from / driven-by the
   spec, used by the PC tool (P4 `debug-monitor`) and as the protocol's reference
   implementation.
3. `cdl_proto.h` — a C header (constants + optional pack/unpack helpers) emitted
   from the spec for the CC5X **target stub**. The STM32 **probe firmware** does
   **not** consume it in v0.1: under the recommended flow (C) (§6) `relay.c` stays
   hand-written and is pinned to the same constants by the `0xF0` golden vector,
   so no generated file enters the firmware build. Vendoring the header into the
   probe (flow A) is a documented future option, not a P1 deliverable.
4. `debuggen.py` refactored to **import the spec** instead of defining
   `:41–105` inline, so the emitted target header is unchanged but no longer a
   second source.
5. Tests: codec round-trip, byte-exact golden vectors (the existing probe `0xF0`
   frame, plus full frames *computed* by the codec for each `01` §4 message —
   **not** transcribed from `01` §10, whose frames carry a literal `CRC`
   placeholder and "SEQ/stuffing simplified for readability"), and a
   generated-header golden-file check on the `test_golden.py` pattern.

**P1 does NOT cover** (tracked elsewhere, do not pull in):

- SPBRG/baud computation from `Fosc` via the Microchip MCP → **P3** (`00` §6).
  P1 treats `brg`/baud as opaque map fields; it states no EUSART formula.
- The CC5X compile-and-measure budget gate → **P2**.
- The `debug-monitor` PC command (symbol-map application, REPL) → **P4**. P1
  provides the codec it will import; it does not open serial ports or render.
- Tier-C toggle edge-map encoding (out of band, no framed protocol) → P7.

---

## 2. The drift problem (why P1 exists)

CDL framing is currently implemented **three** times, and a fourth consumer
(`relay_decode.py`) re-derives it again:

| Where | Lang | Constants source | Role |
|---|---|---|---|
| `debuggen.py:41–105` | Python→C | inline module vars | emits target-stub C |
| `relay.c` | C | `relay.h` `#define`s | probe encoder (`0xF0`/`0xF1`) |
| `relay_decode.py` | Python | module top constants | probe decoder + `_encode` ref |
| (future P4 `debug-monitor`) | Python | — | would re-decode again |

They agree **today**, which is exactly why drift is the risk: nothing enforces
it. Two subtleties P1 must preserve, not "fix":

- **Stuffing representation differs but the bytes are identical.** The stub emits
  `cdl_putc(b ^ CDL_ESC_XOR)` with `CDL_ESC_XOR = 0x20` (`debuggen.py:45,684`);
  `relay.c`/`relay_decode.py` use the literal pairs `0x7E→0x7D 0x5E`,
  `0x7D→0x7D 0x5D`. Since `0x7E ^ 0x20 = 0x5E` and `0x7D ^ 0x20 = 0x5D`, both put
  the same bytes on the wire (`01` §3). The spec should define stuffing **once**
  (the XOR form is canonical) and let each emitter render its idiom.
- **CRC and header shape already match byte-for-byte.** Stub `cdl_send`
  (`debuggen.py:690–707`) and `relay.c` `emit()` both do: `FLAG`, then
  CRC-accumulate-and-stuff `TYPE,SEQ,LEN,ARG...`, then stuff `CRC`, then `FLAG`,
  with CRC-8 poly `0x07` init `0x00`. A single spec can drive both with no wire
  change.

**Type-space gap.** `debuggen.py` only knows the target/host types
(`MSG_TYPES_T2H` `0x01–0x07`, `MSG_TYPES_H2T` `0x81–0x87`). The **probe-origin
envelopes** `0xF0 RELAY` / `0xF1 STATUS` (`01` §4, implemented in `relay.c`) exist
only in the probe repo. The unified spec must add this third group so the probe
header and `relay_decode.py` derive from the same place.

---

## 3. `cdl_proto.py` — the single source

Follow the repo's conventions (verified in `debuggen.py`/`headergen.py`):
`from __future__ import annotations`, `@dataclass(frozen=True)` value objects,
`UPPER_SNAKE` constants, design-doc cite comments (`# 01 §3`), ≤127-col lines
(`.flake8`), public-API docstrings.

### 3.1 Framing + CRC (one definition)

```python
CDL_PROTOCOL_VERSION = (0, 1)        # carried only in HELLO.ver (01 §9)
CDL_FLAG    = 0x7E                    # 01 §3
CDL_ESC     = 0x7D
CDL_ESC_XOR = 0x20                    # 0x7E^0x20=0x5E, 0x7D^0x20=0x5D
CDL_CRC = CrcSpec(poly=0x07, init=0x00, width=8)   # CRC-8/SMBUS, over TYPE..last ARG
```

### 3.2 Message catalogue as data

Each message is one frozen `Msg(type, name, group, fields=[...])`. `group` ∈
`{T2H, H2T, PROBE}` and pins the type-code range invariant (`01` §4/§9:
`0x01–0x0F`, `0x81–0x8F`, `0xF0–0xFF`). `fields` is an ordered list of
`Field(name, kind)` where `kind` is one of a small closed set:

| Field kind | Meaning | Example |
|---|---|---|
| `U8` / `U16LE` / `U32LE` | fixed-width little-endian int | `addr:U16LE`, `RELAY.tstamp:U32LE` |
| `BYTES` | variable tail, length = `LEN − fixed prefix` | `MEM_DATA.bytes`, `RELAY.data` |
| `VARVAL(min,max)` | per-instance width fixed at codegen | `TRACE.value` (1–4 B, per channel) |

The full catalogue is the union of `01` §4's three tables — e.g.

```
T2H   0x01 HELLO     ver:U8 devid:U16LE caps:U8 tier:U8 ch_count:U8
T2H   0x02 TRACE     ch:U8 [tick:U16LE?] value:VARVAL(1,4)
T2H   0x04 MEM_DATA  addr:U16LE bytes:BYTES
H2T   0x82 READ_MEM  addr:U16LE len:U8
PROBE 0xF0 RELAY     tstamp:U32LE data:BYTES
PROBE 0xF1 STATUS    dropped:U32LE
```

Optional fields (`TRACE.tick`, `BP_HIT.pc/nframe`) are gated by a build/caps flag
recorded per generated instance, mirroring how `debuggen.py` already decides
`tier`/`caps` (`_capabilities`). The spec declares the option; the generator
binds it.

### 3.3 What stays out of the spec

Bank/address arithmetic (`01` §6) lives in the stub, not the codec — the codec
moves opaque `addr:U16LE` + `bytes`. The symbol→address map is `debuggen`'s
`build_map()` output, unchanged. Keep `cdl_proto.py` purely about *bytes on the
wire*.

---

## 4. Generated artifacts

### 4.1 `cdl_codec.py` (Python)

No I/O (P4 owns serial). `encode` is a pure function; **decoding is stateful** —
a serial/CDC read can split a frame, or even a single escape pair, across two
chunks, so the deframer must keep `_buf/_in/_esc/_bad` between calls. Mirror
`relay_decode.py`'s existing object instead of a stateless function:

```python
def encode(msg_name: str, seq: int, **fields) -> bytes      # pure → full stuffed frame incl. FLAGs

class Deframer:                                             # SLIP deframer, resync-on-FLAG
    def feed(self, chunk: bytes) -> Iterator[Frame]: ...    # carries _buf/_in/_esc/_bad across calls
# Frame: {name, type, seq, **decoded_fields} | {error, raw}
```

`Deframer.feed` must reproduce `relay_decode.py`'s `Deframer` behaviour exactly:
CRC/len errors and dangling escapes are yielded as `{"error": ...}` and do not
desync the next frame (`relay_decode.py:43–87`, already tested). P1 can lift that
state machine wholesale — it is the proven reference, and it is a class precisely
because the partial-frame state must survive across reads.

### 4.2 `cdl_proto.h` (C, target stub; probe pinned by golden vector)

Must reproduce the bytes `debuggen.py:_render_header` (`:619–637`) emits today, so
the target stub is unchanged:

```c
#define CDL_FLAG       0x7E
#define CDL_ESC        0x7D
#define CDL_ESC_XOR    0x20
#define CDL_CRC_POLY   0x07
#define CDL_T_HELLO    0x01
... CDL_T_*, CDL_NAK_* ...
#define CDL_P_RELAY    0xF0     // NEW: probe-origin group
#define CDL_P_STATUS   0xF1
```

Two consumers, two constraints:
- **CC5X target stub:** `#define`-only + the existing `cdl_crc8`/`cdl_send`
  helpers (`debuggen.py:668–728`), which must stay CC5X-safe (no `~(1<<n)`, no
  variable-count shifts — `debuggen.py:9–18`). P1 sources these helper bodies
  from the spec but keeps them in `debuggen`'s emit path.
- **STM32 probe:** the probe already has hand-written `relay.c`; P1's job is to
  let it consume the *same constants*. See §6 for the cross-repo flow.

### 4.3 JSON map

`build_map()` (`debuggen.py:538–544`) already embeds the protocol constants into
`cdl_map_<dev>.json`. After P1 it embeds them **from the spec**, so the map and
the headers cannot disagree. No schema change.

---

## 5. Migration of `debuggen.py` (no output change)

1. Add `cdl_proto.py` next to `debuggen.py` in
   `tools/cc5x_setcc_native_lib/`.
2. Replace the inline block `debuggen.py:41–105` with `from . import cdl_proto`
   and reference `cdl_proto.CDL_FLAG`, `cdl_proto.MSG_TYPES_*`, etc.
3. Re-run `tests/test_golden.py` (and `--update` **only** if a diff is reviewed
   and intentional — there should be none).
4. Drive `_render_header` / `_CRC_LINES` / `_FRAMER_LINES` from the spec
   catalogue rather than literals, keeping output byte-identical.

The acceptance bar for the migration: `git diff` on every generated
`cdl_monitor_<dev>.h` and `cdl_map_<dev>.json` is **empty**.

---

## 6. Cross-repo artifact flow (the load-bearing decision)

The spec lives in `cc5x-helper`; the probe firmware lives **here**. Three ways to
keep the probe in sync; recommend **(C)**:

- **(A) Vendor the generated `cdl_proto.h` into this repo** and have `relay.c`
  `#include` it, with a CI check that the committed copy matches a fresh
  generation. Tightest coupling; the probe build now depends on a generated file.
- **(B) Generate a probe-side `relay_proto.h`** and replace the hand `#define`s in
  `relay.h`. Same as A with a probe-specific filename.
- **(C) Keep `relay.c`/`relay_decode.py` hand-written, add a golden-vector
  cross-check as the drift tripwire.** Already prototyped: `tests/test_relay.c`
  and `relay_decode.py`'s selftest both assert the byte-identical 15-byte
  `0xF0 RELAY` golden frame (this branch). P1 adds the *same* frame as a vector
  in `cdl_codec`'s test suite, so all three implementations are pinned to one
  constant. Lowest coupling; no generated file enters the firmware build.

**Recommendation: (C) for v0.1.** The probe firmware is tiny, already shipped, and
golden-pinned; a generated-header dependency buys little and complicates the
cross-compile. Revisit (A) if the probe grows more message types. Either way, the
`0xF0 RELAY` golden frame is the shared anchor across repos.

---

## 7. Testing

Mirror `tests/test_debuggen.py` / `tests/test_golden.py` (synthetic fixtures, no
packs, no hardware):

1. **Round-trip:** for every message in the catalogue,
   `Deframer().feed(encode(...))` returns the original fields; assert exact byte
   length and stuffing. Include a split-read case (feed the frame in two chunks,
   the split landing mid-escape) to prove the deframer state survives across
   calls.
2. **Golden vectors (byte-exact):**
   - one *computed* full frame per `01` §4 message (HELLO/TRACE/SET_BP/ACK/
     BP_HIT/READ_MEM/MEM_DATA/CONTINUE): build the bytes with the codec's own
     CRC + stuffing, freeze them as the fixture, and assert `Deframer().feed`
     decodes back to the declared fields. **Do not transcribe `01` §10** — it
     shows `CRC` as a placeholder and states "SEQ/stuffing simplified for
     readability", so its bytes are illustrative, not wire-exact. Cross-check the
     decoded *fields* against §10's annotations (e.g. ACK `ref-seq 0`,
     `READ_MEM addr 0x0020 len 4`) to keep the spec and codec honest;
   - the `0xF0 RELAY` frame `7E F0 00 07 00 01 00 00 7D 5E 7D 5D 42 E2 7E`
     (seq 0, ts 256, data `7E 7D 42`) — identical to the constant in this repo's
     `tests/test_relay.c` and `tools/relay_decode.py`.
3. **Stuffing/recovery:** CRC error, bad escape, dangling-escape-then-FLAG all
   surface as errors without desyncing the next frame (port the
   `relay_decode.py` selftest assertions).
4. **Spec ↔ emitter equality:** assert the C `#define`s `debuggen` emits equal the
   spec values (catches a future hand-edit to the emitter).
5. **Generated-header golden file** for `cdl_proto.h` on the `test_golden.py`
   mechanism, plus optional `validate_generated_headers.py` compile-gate once it
   accepts protocol headers (today it only validates device headers).

---

## 8. Work breakdown

| Step | Deliverable | Size |
|---|---|---|
| 1 | `cdl_proto.py` — constants + `Msg`/`Field` catalogue (incl. PROBE group) | S (~150 LOC) |
| 2 | `cdl_codec.py` — `encode` + stateful `Deframer` (port from `relay_decode.py`) | M (~250 LOC) |
| 3 | `cdl_codec` tests — round-trip + golden vectors (§7.1–7.3) | M (~250 LOC) |
| 4 | `cdl_proto.h` emitter + golden file (§4.2, §7.5) | S (~120 LOC) |
| 5 | Refactor `debuggen.py` to import the spec; prove zero golden diff (§5) | S (~80 LOC churn) |
| 6 | Probe drift tripwire: add the `0xF0` vector to `cdl_codec` tests (§6 C) | XS |

Steps 1–4 are independent of the probe repo; step 6 is the only cross-repo touch
and is just a shared constant. None of it needs hardware or the CC5X compiler.

---

## 9. Open questions

- **Optional-field encoding for `TRACE.tick` / `BP_HIT.pc`:** carry a per-instance
  layout descriptor in the JSON map (so the decoder knows the width chosen at
  codegen), or advertise via `HELLO.caps` and have the decoder infer? Leaning map
  descriptor — explicit beats inference. (`01` §5, caps bit `CAP_TARGET_TICK`.)
- **`cdl_proto.h` helper bodies:** emit `cdl_crc8`/`cdl_send` from the spec for
  *both* targets, or keep the probe's `relay.c` encoder hand-written under flow
  (C)? Current rec: hand-written probe + generated stub, reconciled by golden
  vector — but a shared C helper file is the cleaner long-term answer.
- **Where this doc's deliverables are imported by P4:** confirm `debug-monitor`
  imports `cdl_codec` directly (it should — it is the reference impl, `04` §3).

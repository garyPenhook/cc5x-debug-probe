# Bench bring-up runbook — P5a / P5b / P6

Turnkey procedure to take the F042 probe from "builds clean" to "validated on
silicon". Every constant below is pulled from the firmware source of truth, not
restated from memory: pins from [`src/probe_pins.h`](src/probe_pins.h) (each line
carries its DS/UM citation), bauds/VID:PID from the files noted inline. The
hardware steps are the **only** part of the project that cannot be done without
the board on the bench; everything upstream is already green (see §0).

## 0. Pre-bench gate — PASSING (no hardware)

Re-run before every bench session; all must be green or stop and fix first:

```bash
cd firmware/probe-f042
cmake --preset tests && cmake --build --preset tests && ctest --preset tests
python3 tools/relay_decode.py --selftest
cmake --preset firmware-vcp     && cmake --build --preset firmware-vcp
cmake --preset firmware-usb-cdc && cmake --build --preset firmware-usb-cdc
arm-none-eabi-size build-vcp/probe_f042.elf build-usb-cdc/probe_f042.elf
```

Last run (this repo): `relay`, `usb_desc`, `usb_ctrl` → 3/3 pass; relay_decode
selftest ALL PASS; both transports build with **zero warnings**. Sizes (the
binding **6 KB SRAM** budget — DS confirmed, T-001):

| Build | Flash (text) | RAM (data+bss) |
|---|---|---|
| `build-vcp` (P5a) | 2336 B | 2152 B (35 %) |
| `build-usb-cdc` (P5b) | 4792 B | 2248 B (37 %) |

> Linking needs `vendor/` populated (CMSIS/startup/linker — fetch per
> [`vendor/README.md`](vendor/README.md); git-ignored by license).

## 1. Hardware needed

- **NUCLEO-F042K6** (onboard ST-LINK/V2-1 → flashing + the P5a Virtual COM port).
- A **CC5X PIC target** flashed with a generated CDL stub (e.g. PIC16F15244,
  `debug-stub` from `cc5x-helper`), or a logic analyzer / second UART to emulate
  the target stream for P5a-only validation.
- Jumper wires; common **GND** between probe and target.
- **P5b only:** a USB micro/Type-A breakout to reach PA11/PA12 (CN3-D10/D2).

Wiring (from `probe_pins.h`; CN refs are NUCLEO-F042K6 Arduino headers):

| Signal | F042 pin | NUCLEO header | Notes |
|---|---|---|---|
| Target USART1 TX (probe→…) | PA9 (AF1) | CN3-D1 | to target RX |
| Target USART1 RX (…→probe) | PA10 (AF1) | CN3-D0 | from target TX |
| PC link P5a — USART2 → ST-LINK VCP | PA2 / PA15 (AF1) | (on-board SB2/SB3) | no external wiring |
| PC link P5b — USB DM / DP | PA11 / PA12 | CN3-D10 / CN3-D2 | + GND to host USB |
| Status LED (heartbeat) | PB3 | CN4-D13 (LD3) | active-high, ~2 Hz |

Link bauds: target ↔ probe **115200** (`probe_pins.h:93`); probe → VCP **460800**
(`probe_pins.h:94`, 4× so the 115200 target stream never overruns the VCP).

## 2. P5a — ST-LINK VCP relay (default transport)

**Gate (00-master-plan §5):** relays + timestamps target frames to a PC terminal
over the ST-LINK VCP; PB3 heartbeat.

1. Build + flash the VCP image (openocd has the stlink + stm32f0x scripts):
   ```bash
   cmake --build --preset firmware-vcp
   openocd -f interface/stlink.cfg -f target/stm32f0x.cfg \
     -c "program build-vcp/probe_f042.elf verify reset exit"
   ```
2. **Heartbeat check:** LD3 (PB3) blinks ~2 Hz → core + TIM2 + clock alive
   (`main.c` heartbeat). No blink → clock/init fault, stop here.
3. The VCP appears as `/dev/ttyACM0` (Linux). Decode the live stream:
   ```bash
   python3 tools/relay_decode.py /dev/ttyACM0 --baud 460800   # needs pyserial
   ```
4. Drive PA10 (target RX-into-probe) from the CC5X stub or a UART at **115200**.
   **PASS** = each target frame prints as `[<ts> us] seq=NN  <hex>`; `seq`
   increments without gaps and `ts_us` is monotonic. CRC/escape errors surface
   as `!! crc` / `!! escape` and must **not** desync the following frame.

## 3. P5b — native USB-CDC (PA11/PA12)

**Gate:** enumerates as CDC; forwards + timestamps; buffers fit 6 KB SRAM.

1. Wire PA11→D−, PA12→D+, GND→GND of a USB lead to the host. Flash the USB image:
   ```bash
   cmake --build --preset firmware-usb-cdc
   openocd -f interface/stlink.cfg -f target/stm32f0x.cfg \
     -c "program build-usb-cdc/probe_f042.elf verify reset exit"
   ```
2. **Enumeration check:** a second `/dev/ttyACM*` appears with
   **VID:PID `0483:5740`** (`usb_desc.c` idVendor/idProduct), line coding
   115200 8N1 (`usb_desc.h`). Confirm:
   ```bash
   lsusb -d 0483:5740 && dmesg | tail
   ```
   No enumeration → check the F042 internal-48 MHz USB clock (crystal-less, RM0091)
   and the PA11/PA12 wiring; the descriptors themselves are unit-tested (§0).
3. Decode (USB-CDC ignores the host baud): `python3 tools/relay_decode.py /dev/ttyACMx`.
   **PASS** = same monotonic timestamped trace as P5a, no dropped-byte STATUS
   (`0xF1`) frames under sustained 115200 target traffic.

## 4. P6 — end-to-end on PIC16F15244

**Gate:** live timestamped trace; mem peek; breakpoint stop/continue.

Prereq: target running a Tier-A-full CDL stub (`debug-stub`, cc5x-helper) on the
PA9/PA10 link; PC running `cc5x-helper debug-monitor` (the canonical P1/P4 codec).

1. **Trace:** target `cdl_trace()` points stream as named events in
   `debug-monitor` (symbol map from `cdl_map.json`). PASS = names + timestamps.
2. **READ_MEM:** request a known RAM address; PASS = returned bytes match the
   target's actual value (cross-check via the stub or a second read).
3. **Breakpoints:** SET_BP at a known site → target halts in monitor (BP_HIT);
   CONTINUE resumes. PASS = stop is observed and CONTINUE cleanly resumes trace.

Record results in the T-001 ticket and tick the P5a/P5b/P6 gates in
[`../../00-master-plan.md`](../../00-master-plan.md) §5.

## 5. What remains after P6 (deferred, P7)

Tier-B bit-bang, Tier-C pulse-width/-train + STM32 input-capture, target-tick,
bank-crossing read, READ_PGM, trace compression, 16-bit BRG — each its own gate,
out of v0.1 scope. None block P6.

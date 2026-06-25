# probe-f042 — CC5X debug probe firmware (STM32F042K6 / NUCLEO-F042K6)

Fixed, target-family-independent probe firmware. See the design docs in
`../../00-master-plan.md` (§ P5a/P5b) and `HARDWARE.md` for the verified,
datasheet-sourced hardware spec. To take it to the bench, follow the turnkey
[`BRINGUP.md`](BRINGUP.md) runbook (build → flash → enumerate → validate).

## What this does
```
CC5X target --UART--> USART1 (PA9/PA10) --RX ISR--> relay (timestamps via TIM2)
   --main loop--> PC link --USB--> PC terminal
```
The **PC link is a build-time choice** (same `board_pc_write()`/`board_pc_poll()`
contract, relay core unchanged):
- **P5a / VCP (default):** USART2 (PA2/PA15) → on-board ST-LINK Virtual COM Port.
  Zero soldering — validates the relay/timestamp path.
- **P5b / USB-CDC:** native USB full-speed on **PA11 (D−)/PA12 (D+)**, enumerating
  as a CDC ACM serial port. Needs a USB breakout wired to **CN3-D10 (D−)** and
  **CN3-D2 (D+)** + GND (the board Micro-USB is ST-LINK only). Crystal-less
  (HSI48 + CRS disciplined by USB SOF).

Both expose the **same** CDL-conformant relay byte stream, so `tools/relay_decode.py`
decodes either (`/dev/ttyACM*` — for USB-CDC the baud is ignored by the host).

## Source layout
| File | Role | Status |
|---|---|---|
| `src/probe_pins.h` | Verified pin/peripheral map (cites DS + UM) | ✅ done |
| `src/relay.[ch]` | Portable relay core — CDL-conformant SLIP framing (01 §3) | ✅ done, tested |
| `src/bytefifo.h` | All-or-nothing byte FIFO backing the PC TX | ✅ done, tested |
| `src/board.h` | Hardware abstraction the app depends on | ✅ done |
| `src/main.c` | App: relay → VCP TX FIFO drain, LED heartbeat | ✅ done |
| `src/board_f042.c` | F042 register layer (clock/GPIO/TIM2/USART) + transport dispatch | ✅ builds clean, RM0091-cited |
| `src/usb_desc.[ch]` | USB-CDC (ACM) descriptor set — pure data | ✅ done, host-tested |
| `src/usb_ctrl.[ch]` | Pure EP0 control-request resolver (no hardware) | ✅ done, host-tested |
| `src/usb_cdc.[ch]` | Bare-metal F042 USB FS device CDC driver (P5b) — executes resolver actions on the USB registers | ✅ builds clean, RM0091-cited; pending bench |
| `tests/test_relay.c` | Host unit tests (framing, escaping, all-or-nothing) | ✅ passing |
| `tests/test_usb_desc.c` | Host unit tests (descriptor self-consistency) | ✅ passing |
| `tests/test_usb_ctrl.c` | Host unit tests (EP0 decisions: descriptor/ZLP/SET_*/STALL) | ✅ passing |

`board_f042.c` is fully implemented: the data-path wiring (RX ISR, timestamp,
LED, all-or-nothing VCP TX) **and** the peripheral init (SYSCLK=HSI48 48 MHz,
GPIO AF, TIM2 1 µs, USART1/USART2 @115200) — every register/bit cited inline to
its RM0091 Rev 10 page. It compiles against a mock CMSIS (catches typos) but is
**not yet hardware-validated**: review **ES0243** errata and bench-test before
trusting the image.

## Vendor files (you must add — cannot be auto-fetched; st.com blocks bots)
From the **STM32Cube_FW_F0** package, copy into `vendor/`:
- `CMSIS/Device/ST/STM32F0xx/Include/` (incl. `stm32f042x6.h`) and `CMSIS/Include/`
- `system_stm32f0xx.c`
- `startup_stm32f042x6.s`
- `STM32F042K6Tx_FLASH.ld` (32 KB flash / 6 KB RAM linker script)

## Test (host, no hardware)
```
cmake --preset tests && cmake --build --preset tests && ctest --preset tests
```
Builds and runs three host suites: `test_relay` (SLIP framing/escaping, multi-frame
chunking, all-or-nothing TX retry, FIFO — catches framing regressions),
`test_usb_desc` (CDC descriptor self-consistency), and `test_usb_ctrl` (the EP0
control-request resolver: descriptor selection + wLength cap + ZLP rule,
SET_ADDRESS, SET_CONFIGURATION 0/1/invalid, GET_STATUS, class requests). The
register-level USB sequencing in `usb_cdc.c` stays bench-gated.

## Linting & static analysis
All tools are free/open-source; CI (`.github/workflows/ci.yml`) runs them on
every push/PR, and `.pre-commit-config.yaml` runs the fast ones locally
(`pip install pre-commit && pre-commit install`).

| Tool | Scope | How to run |
|---|---|---|
| compiler warnings | `src/` (host + cross) | built in — `PROBE_WARNINGS` in `CMakeLists.txt` |
| **cppcheck** | `src/` C | `cmake --build build-tests --target cppcheck` |
| **clang-tidy** | portable C (`relay`/`usb_desc`/`usb_ctrl`) | `clang-tidy -p build-tests src/relay.c …` (config: `.clang-tidy`) |
| **ruff** | `tools/` Python (lint) | `ruff check .` (config: `pyproject.toml`) |
| **mypy** | `tools/` Python (types) | `mypy tools` |
| **codespell** | code + docs | `codespell` |

### Formatting (opt-in)
Formatting is **not** a CI gate — the register/descriptor tables and the Python
tool's comments are hand-aligned on purpose. The configs exist
(`.clang-format`, `[tool.ruff]`) so you can format **changed lines** on demand:

```
git clang-format                                   # C: only your staged diff
pre-commit run ruff-format --hook-stage manual     # Python: ruff/Black style
```

## Build (firmware)
```
cmake --preset firmware-vcp        # VCP (P5a, default)  -> build-vcp/
cmake --build --preset firmware-vcp

cmake --preset firmware-usb-cdc    # native USB-CDC (P5b) -> build-usb-cdc/
cmake --build --preset firmware-usb-cdc
```
The two presets pin `PROBE_PC_TRANSPORT` and use separate binary directories, so
switching transports never reuses a stale cached value. `PROBE_PC_TRANSPORT`
(`VCP` | `USB_CDC`) selects the PC link; it maps to the
`PROBE_PC_TRANSPORT_*` enum in `probe_pins.h`. With `vendor/` present this links
`probe_f042.elf`/`.bin` and prints a size report.
**Current builds (arm-none-eabi-gcc 16.1.0, MinSizeRel), zero warnings:**

| Transport | flash (text) | RAM (data+bss) |
|---|---|---|
| VCP (P5a) | 2336 B (7% of 32 KB) | 2152 B (35% of 6 KB) |
| USB-CDC (P5b) | 4792 B (15%) | 2248 B (37%) |

## Flash / run
Flash `probe_f042.bin` via the on-board ST-LINK (st-flash / STM32CubeProgrammer).
The LED (PB3) blinks ~2 Hz as a heartbeat. Once a CC5X target is wired to PA10
(RX), decode the timestamped frames off the VCP with the bench helper:

```
tools/relay_decode.py /dev/ttyACM0        # live; default --baud 460800 = PROBE_VCP_BAUD
tools/relay_decode.py --file capture.bin  # offline
tools/relay_decode.py --selftest          # no hardware/deps
```
(For the P5b USB-CDC link the host ignores the baud, so `/dev/ttyACM*` works as-is.)

`relay_decode.py` is an independent reimplementation of the wire format; a
C-encode → Python-decode cross-check confirms `relay.c` and the decoder agree
(including SLIP escaping of `0x7E`/`0x7D` payload bytes).

## Next steps
1. **Bench bring-up (P5a, VCP):** flash, confirm LED (PB3) ~2 Hz heartbeat,
   loopback PA9→PA10, decode the VCP with `tools/relay_decode.py` @460800.
2. **Bench bring-up (P5b, USB-CDC):** wire a USB breakout to CN3-D10 (D−)/CN3-D2
   (D+)/GND, flash the `USB_CDC` build, confirm CDC enumeration (`/dev/ttyACM*`),
   then decode the relay stream off it. Key untested-on-silicon assumptions to
   confirm first: PMA packed "2×16" addressing, HSI48+CRS lock without a crystal,
   and EP0 enumeration. See `HARDWARE.md`.
3. P5b refinements: per-CDL-frame timestamping; optional USART RX/TX DMA.
4. P1: fold the probe→host RELAY/STATUS envelopes into the single-source
   `cdl_proto` codec.

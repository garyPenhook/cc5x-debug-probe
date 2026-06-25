# Probe hardware spec — NUCLEO-F042K6 (STM32F042K6T6)

Every fact below is confirmed against the in-repo authoritative docs (see
`../../` for the PDFs). No values are from memory.

## Sources
- **DS** — `stm32f042k6.pdf`, datasheet DocID025832 Rev 5
- **RM** — `rm0091-...pdf`, RM0091 (F0x1/F0x2/F0x8 reference manual)
- **PM** — `pm0215-...pdf`, Cortex-M0 programming manual
- **UM** — `um1956-...pdf` Rev 6, Nucleo-32 (MB1180) user manual
- **ES** — `es0243-...pdf`, STM32F042x4/x6 device errata
- **DB** — `nucleo-f042k6.pdf`, Nucleo-32 data brief DB2638

## Confirmed device budget (DS p.1, Table 7)
| Resource | Value | Source |
|---|---|---|
| Core | Cortex-M0 @ 48 MHz | DS p.1 |
| SRAM | **6 KB** (HW parity) | DS p.1 |
| Flash | 32 KB | DS p.1 |
| DMA | 5 channels | DS p.1 |
| USART | 2 (USART1, USART2) | DS §3.15, Table 10 |
| 32-bit timer | **TIM2** (others 16-bit) | DS Table 7 p.21 |
| USB | FS device, crystal-less (HSI48+CRS) | DS p.1, §3.20 |

## Pin map (verified — mirrors `src/probe_pins.h`)
| Signal | STM32 pin | AF / fn | Arduino | Source |
|---|---|---|---|---|
| Target UART TX | PA9 | USART1_TX AF1 | CN3-D1 | DS T14 p37 / UM T10 |
| Target UART RX | PA10 | USART1_RX AF1 | CN3-D0 | DS T14 p37 / UM T10 |
| PC link TX (P5a) | PA2 | USART2_TX AF1 → ST-LINK VCP | (A7) | DS T14 / UM §7.9 |
| PC link RX (P5a) | PA15 | USART2_RX AF1 → ST-LINK VCP | — | DS T14 p37 / UM §7.9 |
| USB D− (P5b) | PA11 | USB_DM | CN3-D10 | DS T13 p34 / UM T10 |
| USB D+ (P5b) | PA12 | USB_DP | CN3-D2 | DS T13 p34 / UM T10 |
| Status LED | PB3 | GPIO out, active-high | CN4-D13 | UM §7.5 |
| Timestamp | TIM2 | 32-bit free-run CNT | — | DS T7 p21 |

Verified non-conflict: on LQFP32, PA9/PA10 and PA11/PA12 are independent pins;
the small-package remap (DS T13 footnote 5) is 28-/20-pin only.

Board facts: no crystal populated, HSI default (UM §7.8); board Micro-USB is
ST-LINK only (DB) → native USB (P5b) needs a breakout wired to CN3-D10/D2+GND.

## Configuration values — implemented in `board_f042.c` (RM0091 Rev 10)
| Init | Key register facts | RM0091 |
|---|---|---|
| Flash | ACR: PRFTBE=bit4, LATENCY=001 (1 WS, 24<SYSCLK≤48 MHz) | p69 |
| Clock | CR2 HSI48ON=16/RDY=17; CFGR **SW=11→HSI48**, SWS=11; HPRE/PPRE=0 (÷1) → 48 MHz | p112-114, 134 |
| GPIO | AHBENR IOPAEN=17/IOPBEN=18; MODER 10=AF/01=out; AFR 0001=AF1; PA13/14 untouched (SWD) | p121, 158, 162 |
| TIM2 | APB1ENR TIM2EN=0; PSC=47 → 1 MHz; ARR=0xFFFFFFFF (32-bit); EGR.UG to latch; CR1.CEN | p125, 446, 462-463 |
| USART | APB2ENR USART1EN=14; APB1ENR USART2EN=17; **BRR=fCK/baud** (OVER8=0, fCK=PCLK=48 MHz); CR1 UE=0/RE=2/TE=3/RXNEIE=5 | p123, 125, 716, 744, 755 |

Status: **cross-build green** (arm-none-eabi-gcc 16.1.0, zero warnings); CMSIS
confirms the RM0091 clock values (SW=0x3/SWS=0xC); `USART1_IRQHandler` links as
the concrete vector. Footprint (VCP/P5a): ~2.3 KB flash (7%), 2152 B RAM (35% of
6 KB); see the per-transport SRAM budget below. **Not yet hardware-validated** —
flash and bench-test next.

## Errata pass — ES0243 Rev 4 (silicon rev A, the only revision)
Reviewed against the P5a init. **No code change required for P5a:**
- **Clock / TIM2 / GPIO** — no applicable errata. There is no HSI48/RCC erratum;
  TIM errata are all PWM/compare/break/output (TIM2 is used only as a free-running
  counter); the GPIO erratum (2.3.1) affects only OTYPER *locking*, which is unused.
- **USART 2.11.1–2.11.6** — all N/A to a plain 8N1 async bridge: no TE toggling
  (2.11.1), no smartcard/SPI-slave mode (2.11.2/2.11.6), no CTS/RTS flow control
  (2.11.3/2.11.4), 1 stop bit + no receiver-timeout (2.11.5).
- **USART 2.11.7 — data corruption on a noisy RX line (no silicon workaround).**
  Applies to the target link (USART1, oversampling 16, 1 stop bit). Mitigation is
  at the protocol layer: CDL frames carry CRC8 (01 §3), so a corrupted byte fails
  CRC downstream (debug-monitor, P4) and the frame is dropped/resynced. Keep the
  target UART line short/clean.

**P5b USB errata (now active) — ES0243 Rev 4, addressed in `usb_cdc.c`:**
- **2.14.1** PMA overrun/underrun at low APB — *no workaround needed*: requires
  APB ≥ 10 MHz, we run **48 MHz**.
- **2.14.3** Incorrect CRC16 written into the RX buffer (no silicon fix) —
  *compliant*: the driver reads only the `COUNTn_RX` payload length, never the
  trailing CRC16 bytes (the hardware CRC check is still functional).
- **2.14.2** ESOF desync after resume — N/A: the probe never initiates remote
  wakeup / resume signaling.
- **2.14.4 / 2.14.5** BCD below −20 °C / DCD non-compliant — N/A: BCD and DCD are
  not used.
- DMA 2.4.1 — still deferred (no UART/USB DMA in P5b; the USB path is PMA-copy).

## USB-CDC native link (P5b) — `usb_cdc.c`, against RM0091 Rev 10 §30 / §7
| Item | Key facts | Source |
|---|---|---|
| Peripheral | USB FS device present on F04x; 1024 B PMA = 512×16-bit | RM Table 121 p868 |
| PMA access | **"2×16 bits/word" packed**: CPU half-word addr = `USB_PMAADDR + offset` (1:1, **not** the F103 ×2 stride); 8/16-bit access only | RM p868, p872, p897 |
| Base / IRQ | `USB_BASE` APB+0x5C00, `USB_PMAADDR` APB+0x6000, `USB_IRQn`=31 | CMSIS `stm32f042x6.h` |
| Clock | USB 48 MHz = HSI48 (`RCC_CFGR3.USBSW`=0, default); `USBEN`=APB1ENR.23 | RM p119, p125 |
| Crystal-less | CRS auto-trims HSI48 to USB SOF; `CRS_CFGR` reset (0x2022BB7F) already = SYNCSRC USB-SOF, RELOAD 47999 → only set `CRSEN` + `CRS_CR.AUTOTRIMEN\|CEN` | RM §7.7 p143-144 |
| Pins | PA11=USB_DM, PA12=USB_DP are **additional functions** (not in the AF table) → driven directly by the transceiver, **no MODER/AFR config**; connect via `BCDR.DPPU` | DS T13 p34 / T14 p37 |
| Endpoints | EP0 control 64 B; EP1 bulk IN(0x81)/OUT(0x01) 64 B (relay stream); EP2 interrupt IN(0x82) 8 B notification (present, NAK) | RM §30.6 |
| BTABLE | entries at `BTABLE + n*8 + {0,2,4,6}`; 64-B RX buffer = `BL_SIZE=1,NUM_BLOCK=1` | RM §30.6.2, Table 130 p899 |
| EPnR RMW | toggle bits (STAT/DTOG) written 0 = no change; CTR bits (rc_w0) written 1 = keep, 0 = clear; invariant mask 0x878F | RM p892 |

Class: USB 2.0 FS **CDC-ACM** (Virtual COM Port), VID/PID **0x0483/0x5740** (ST's
standard VCP pair → inbox host drivers). Descriptors in `usb_desc.c`,
host-unit-tested (`tests/test_usb_desc.c`).

The EP0 control-request *decision* logic (descriptor selection, wLength cap, ZLP
rule, SET_ADDRESS/SET_CONFIGURATION/GET_STATUS, class requests) is factored into a
pure resolver (`usb_ctrl.c`) and **host-unit-tested** (`tests/test_usb_ctrl.c`);
`usb_cdc.c` only executes the resolved actions on the registers.

**Not yet hardware-validated.** Highest-risk assumptions left to confirm on the
bench: (1) PMA packed "2×16" addressing, (2) HSI48+CRS lock without a crystal,
(3) the register-level EP0 sequencing (EPnR toggle/rc_w0 writes, data toggle)
that unit tests can't exercise.

## SRAM budget (6 KB)
- VCP (P5a): relay ring (`s_data[512]` + 32-entry mark ring) + 256 B VCP FIFO ≈
  **2152 B (35%)**.
- USB-CDC (P5b): relay ring + 256 B USB TX FIFO + descriptor scratch + EP state ≈
  **2248 B (37%)**. PMA buffers (≈328 B) live in the dedicated USB SRAM, not the
  6 KB system SRAM. Plenty of headroom; tighten `RING_SIZE` only if needed.

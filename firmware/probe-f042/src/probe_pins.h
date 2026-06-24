/*
 * probe_pins.h — verified pin/peripheral map for the CC5X debug probe
 * Target board: NUCLEO-F042K6 (STM32F042K6T6, LQFP32, MB1180).
 *
 * EVERY assignment here is confirmed against the in-repo authoritative docs —
 * not from memory. Sources:
 *   [DS]  stm32f042k6.pdf — datasheet DocID025832 Rev 5
 *           Table 14 (port A alternate functions, p.37)
 *           Table 13 (pin definitions, LQFP32 column, p.32-34)
 *           Table 7  (timer feature comparison, p.21)
 *   [UM]  um1956 Rev 6 — STM32 Nucleo-32 (MB1180) user manual
 *           Table 10 (NUCLEO-F042K6 Arduino Nano connectors, p.24)
 *           §7.5 LEDs, §7.8 OSC clock, §7.9 USART virtual communication
 *
 * Pin-conflict note (verified): on the LQFP32/K6 package, PA9/PA10 (USART1) and
 * PA11/PA12 (USB) are four INDEPENDENT pins — the small-package remap quirk
 * (DS Table 13 footnote 5) applies only to the 28-/20-pin packages. [DS p.34]
 */
#ifndef PROBE_PINS_H
#define PROBE_PINS_H

/* ---- Target link: USART1 (probe <-> CC5X target) ---------------------------
 * PA9  = USART1_TX (AF1)  -> Arduino CN3-D1   [DS T14 p37, UM T10 p24]
 * PA10 = USART1_RX (AF1)  -> Arduino CN3-D0   [DS T14 p37, UM T10 p24]
 * Both free on the header (no board function competes).                       */
#define TARGET_USART            USART1
#define TARGET_GPIO_PORT        GPIOA
#define TARGET_TX_PIN           9u      /* PA9  */
#define TARGET_RX_PIN           10u     /* PA10 */
#define TARGET_USART_AF         1u

/* ---- PC link (P5a): USART2 routed to the ST-LINK Virtual COM port -----------
 * PA2  = USART2_TX (AF1)  -> ST-LINK VCP via SB2 (default ON)   [DS T14, UM T7/§7.9]
 * PA15 = USART2_RX (AF1)  -> ST-LINK VCP via SB3 (default ON)   [DS T14 p37, UM §7.9]
 * NOTE: PA2 is exclusive with Arduino A7 (UM T10 footnote 5).                  */
#define PC_USART                USART2
#define PC_TX_PORT              GPIOA
#define PC_TX_PIN               2u      /* PA2  */
#define PC_RX_PORT              GPIOA
#define PC_RX_PIN               15u     /* PA15 */
#define PC_USART_AF             1u

/* ---- PC link (P5b): F042 native USB Full-Speed ------------------------------
 * PA11 = USB_DM  -> Arduino CN3-D10   [DS T13 p34 additional fn, UM T10 p24]
 * PA12 = USB_DP  -> Arduino CN3-D2    [DS T13 p34 additional fn, UM T10 p24]
 * The board Micro-USB is ST-LINK-only (nucleo data brief DB2638); native USB
 * needs an external breakout wired to CN3-D10 (D-) / CN3-D2 (D+) + GND.
 * IMPORTANT: PA11/PA12 are USB *additional* functions (DS Table 13), NOT in the
 * GPIO AF table (DS Table 14) — the USB transceiver drives them directly, so
 * they must be left at reset (no MODER/AFR config). See usb_cdc.c.             */
#define USB_DM_PIN              11u     /* PA11 */
#define USB_DP_PIN              12u     /* PA12 */

/* ---- PC-link transport selection -------------------------------------------
 * P5a = ST-LINK Virtual COM Port over USART2 (zero extra wiring).
 * P5b = native USB-CDC on PA11/PA12 (needs a USB breakout to CN3-D10/D2+GND).
 * Select at build time with the CMake cache var PROBE_PC_TRANSPORT (which sets
 * -DPROBE_PC_TRANSPORT=...). Default keeps the P5a VCP path so the existing
 * bench bring-up is unchanged.                                                */
#define PROBE_PC_TRANSPORT_VCP      0
#define PROBE_PC_TRANSPORT_USB_CDC  1
#ifndef PROBE_PC_TRANSPORT
#define PROBE_PC_TRANSPORT          PROBE_PC_TRANSPORT_VCP
#endif

/* ---- Status LED -------------------------------------------------------------
 * PB3 = user LED LD3, Arduino CN4-D13, via SB15 (default ON).  [UM §7.5, T10]
 * Active HIGH: I/O high => LED on.                                            */
#define LED_PORT                GPIOB
#define LED_PIN                 3u      /* PB3 */
#define LED_ACTIVE_HIGH         1

/* ---- Timestamp source -------------------------------------------------------
 * TIM2 is the only 32-bit general-purpose timer (DS Table 7 p21). Free-running
 * counter; no pin required for basic frame timestamps. (CH inputs reserved for
 * future Tier-C edge capture.)                                                */
#define TIMESTAMP_TIM           TIM2

/* ---- Clock ------------------------------------------------------------------
 * NUCLEO-F042K6 ships with NO crystal; HSI is the default source (UM §7.8,
 * Table 6). Plan: SYSCLK = HSI48 (48 MHz). For P5b, USB 48 MHz clock is HSI48
 * disciplined by CRS synced to USB SOF (DS §3.20; crystal-less USB).          */
#define SYSCLK_HZ               48000000u

/* USART kernel clock = PCLK = SYSCLK at reset (RM0091 §6: HPRE/PPRE not divided;
 * USARTxSW = 00 = PCLK). BRR = fCK/baud, OVER8=0 (RM0091 §27.5.4, §27.8.4).
 *
 * The PC link MUST run faster than the target link: the relay wraps each <=32
 * data bytes in >=10 bytes of framing (plus SLIP escaping), so an equal-baud VCP
 * cannot drain a saturated target stream and the ring would overflow. The VCP is
 * set to 4x the target for comfortable headroom; lower it if your ST-LINK VCP /
 * host terminal won't negotiate 460800. */
#define PROBE_TARGET_BAUD       115200u   /* probe <-> CC5X target link */
#define PROBE_VCP_BAUD          460800u   /* probe -> ST-LINK VCP -> PC (4x) */

#endif /* PROBE_PINS_H */

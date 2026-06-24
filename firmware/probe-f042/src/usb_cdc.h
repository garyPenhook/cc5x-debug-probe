/*
 * usb_cdc.h — native USB full-speed CDC-ACM transport for the probe PC link (P5b).
 *
 * Replaces the P5a ST-LINK VCP (USART2) path behind the same board.h sink
 * contract: board_pc_write() (all-or-nothing) + board_pc_poll(). The relay core
 * and main.c are unchanged; only board_f042.c chooses this transport when
 * PROBE_PC_TRANSPORT == PROBE_PC_TRANSPORT_USB_CDC.
 *
 * Bare-metal direct-register driver for the STM32F042 USB FS device peripheral
 * (RM0091 §30), in the same direct-LL style as board_f042.c. The hardware body
 * is CMSIS-gated; the descriptors it serves live in usb_desc.c (host-tested).
 *
 * Endpoint → EPnR mapping (RM0091 §30.6, one EPnR per endpoint *number*):
 *   EP0R  CONTROL    — enumeration / CDC class requests (64 B)
 *   EP1R  BULK       — IN (0x81) carries the relay byte stream; OUT (0x01) drained
 *   EP2R  INTERRUPT  — IN (0x82) CDC notification, present but unused (NAK)
 */
#ifndef USB_CDC_H
#define USB_CDC_H

#include <stdint.h>
#include <stddef.h>

/* Bring up HSI48/CRS USB clock, the USB peripheral, BTABLE/PMA, EP0, and assert
 * the DP pull-up (connect). Call once from board_init() (USB transport only). */
void usb_cdc_init(void);

/* All-or-nothing enqueue of an encoded frame for transmission to the host over
 * the bulk IN endpoint. Returns len if the whole buffer was queued, else 0
 * (the relay retries the same frame next poll) — honors the relay sink contract. */
size_t usb_cdc_write(const uint8_t *buf, size_t len);

/* Move queued TX bytes into the bulk IN endpoint when it is free. Call from the
 * main loop (mirrors board_pc_poll()). */
void usb_cdc_poll(void);

/* USB global interrupt handler body (invoked from USB_IRQHandler). */
void usb_cdc_irq(void);

/* Non-zero once the host has SET_CONFIGURATION'd the device (data EPs live). */
int usb_cdc_configured(void);

#endif /* USB_CDC_H */

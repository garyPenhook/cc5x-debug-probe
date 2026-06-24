/*
 * usb_desc.h — USB-CDC (ACM) descriptor set for the probe's native USB link (P5b).
 *
 * Pure data + a runtime string builder: no hardware/CMSIS dependency, so the
 * whole module compiles and is unit-tested on the host (tests/test_usb_desc.c)
 * exactly like relay.c. The hardware driver (usb_cdc.c) consumes these.
 *
 * Class: USB 2.0 full-speed CDC-ACM (Abstract Control Model) Virtual COM Port.
 * Endpoints (see usb_cdc.h for the EPnR mapping):
 *   EP0    control (64 B)
 *   EP1 IN/OUT bulk data (64 B)   — the byte stream carrying CDL relay frames
 *   EP2 IN  interrupt notification (8 B) — present per spec, unused (NAK)
 *
 * VID/PID = 0x0483/0x5740 = STMicroelectronics' standard Virtual COM Port pair,
 * which binds to the inbox usbserial/usb_acm drivers on Linux/macOS/Windows —
 * the pragmatic choice for bench bring-up (matches the ST CDC reference).
 */
#ifndef USB_DESC_H
#define USB_DESC_H

#include <stdint.h>
#include <stddef.h>

#define USB_CDC_EP0_MAXPACKET    64u
#define USB_CDC_BULK_MAXPACKET   64u
#define USB_CDC_NOTIF_MAXPACKET  8u

#define USB_CDC_CONFIG_TOTAL_LEN 67u    /* must match sizeof(usb_config_desc)   */
#define USB_CDC_DEVICE_LEN       18u

/* Standard USB descriptor type codes (USB 2.0 §9.4, Table 9-5). */
#define USB_DT_DEVICE            0x01u
#define USB_DT_CONFIG            0x02u
#define USB_DT_STRING            0x03u
#define USB_DT_INTERFACE         0x04u
#define USB_DT_ENDPOINT          0x05u
#define USB_DT_CS_INTERFACE      0x24u  /* CDC class-specific interface         */

extern const uint8_t usb_device_desc[USB_CDC_DEVICE_LEN];
extern const uint8_t usb_config_desc[USB_CDC_CONFIG_TOTAL_LEN];

/* Build the string descriptor for index `i` into `buf` (UTF-16LE, with the
 * 2-byte header). index 0 → LANGID (English-US). Returns the byte count
 * written, or 0 if the index is unknown or the buffer is too small. */
size_t usb_desc_string(uint8_t i, uint8_t *buf, size_t buflen);

#endif /* USB_DESC_H */

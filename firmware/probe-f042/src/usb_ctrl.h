/*
 * usb_ctrl.h — pure USB control-request resolver for the CDC-ACM device (P5b).
 *
 * Given an 8-byte SETUP packet, decide the response: what to STALL, what data to
 * return (already capped to wLength, with the terminating-ZLP rule applied),
 * when to send a zero-length status, when to expect an OUT data stage, and the
 * deferred SET_ADDRESS / SET_CONFIGURATION intents. This is the logic that had
 * the enumeration bugs; isolating it from the USB registers makes it
 * host-unit-testable (tests/test_usb_ctrl.c) — the register sequencing stays in
 * usb_cdc.c. No hardware/CMSIS dependency here.
 */
#ifndef USB_CTRL_H
#define USB_CTRL_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
    USB_CTRL_STALL,       /* unsupported request → STALL EP0 */
    USB_CTRL_TX_DATA,     /* IN data stage: send resp.data[0..len), then OUT status */
    USB_CTRL_STATUS,      /* no/!data: send zero-length IN status */
    USB_CTRL_EXPECT_OUT,  /* control-write: receive an OUT data stage, then status */
} usb_ctrl_kind;

typedef struct {
    usb_ctrl_kind  kind;
    const uint8_t *data;       /* TX_DATA: bytes to send (valid for the transfer) */
    uint16_t       len;        /* TX_DATA: count, already capped to wLength */
    uint8_t        zlp;        /* TX_DATA: a terminating zero-length packet is needed */
    int16_t        set_address;/* >=0: apply this device address after status; -1 none */
    int8_t         set_config; /* 0/1: (de)configure after status; -1 none */
} usb_ctrl_resp;

/* Resolve a standard or class SETUP packet. `configured` is the current
 * configured flag (for GET_CONFIGURATION). `scratch`/`scratch_len` is caller
 * storage that may back the returned data (string + 1-byte replies); it must
 * outlive the resulting transfer. `line_coding` points at the caller-owned
 * 7-byte CDC line coding (USB_CDC_LINE_CODING_LEN): GET_LINE_CODING returns it
 * verbatim, and SET_LINE_CODING's OUT payload is captured into it by the caller
 * — the resolver stays stateless. It must also outlive the transfer. */
usb_ctrl_resp usb_ctrl_resolve(const uint8_t setup[8], uint8_t configured,
                               uint8_t *scratch, size_t scratch_len,
                               const uint8_t *line_coding);

/* Exposed for testing: the control-IN terminating-ZLP rule. A ZLP is needed
 * only when fewer bytes than requested are returned AND that count is a nonzero
 * exact multiple of the max packet size. */
uint8_t usb_ctrl_zlp_needed(uint16_t sent, uint16_t wlength, uint16_t maxpacket);

#endif /* USB_CTRL_H */

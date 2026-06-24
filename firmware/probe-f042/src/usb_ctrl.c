/*
 * usb_ctrl.c — pure CDC-ACM control-request resolver (see usb_ctrl.h).
 * No hardware: maps a SETUP packet to a response. Unit-tested on the host.
 * Request codes mirror usb_cdc.c (USB 2.0 §9.4; CDC PSTN §6.3).
 */
#include "usb_ctrl.h"
#include "usb_desc.h"

#define REQ_GET_STATUS         0x00u
#define REQ_CLEAR_FEATURE      0x01u
#define REQ_SET_FEATURE        0x03u
#define REQ_SET_ADDRESS        0x05u
#define REQ_GET_DESCRIPTOR     0x06u
#define REQ_GET_CONFIGURATION  0x08u
#define REQ_SET_CONFIGURATION  0x09u
#define CDC_SET_LINE_CODING        0x20u
#define CDC_GET_LINE_CODING        0x21u
#define CDC_SET_CONTROL_LINE_STATE 0x22u
#define RT_TYPE_MASK   0x60u
#define RT_TYPE_STD    0x00u
#define RT_TYPE_CLASS  0x20u
/* bmRequestType direction (bit7) and recipient (bits4:0), USB 2.0 §9.3. */
#define RT_DIR_IN          0x80u   /* set = device-to-host (IN data stage) */
#define RT_RECIP_MASK      0x1Fu
#define RT_RECIP_DEVICE    0x00u
#define RT_RECIP_INTERFACE 0x01u
#define RT_RECIP_ENDPOINT  0x02u
/* Standard feature selectors (USB 2.0 §9.4.1, Table 9-6). */
#define FEAT_ENDPOINT_HALT 0x00u

/* Interfaces/endpoints this device exposes (must match usb_desc.c's config
 * descriptor). A GET_STATUS / CLEAR|SET_FEATURE / class request aimed at an
 * interface or endpoint the device does not implement must STALL, not be
 * silently accepted (USB 2.0 §9.4). Comm interface 0 owns all CDC management
 * requests (CDC PSTN §6.3). */
#define USB_CDC_COMM_INTERFACE  0u
#define USB_CDC_NUM_INTERFACES  2u      /* comm (0) + data (1) */

/* Endpoint addresses (dir bit7 | number): EP0 control, EP1 IN/OUT bulk, EP2 IN
 * notify. wIndex of an endpoint-directed request carries one of these. */
static uint8_t valid_endpoint(uint16_t windex)
{
    switch (windex) {
    case 0x00u:  /* EP0 control */
    case 0x01u:  /* EP1 OUT (bulk) */
    case 0x81u:  /* EP1 IN  (bulk) */
    case 0x82u:  /* EP2 IN  (notification) */
        return 1u;
    default:
        return 0u;
    }
}

uint8_t usb_ctrl_zlp_needed(uint16_t sent, uint16_t wlength, uint16_t maxpacket)
{
    /* A short or zero-length final packet already ends the data stage. A ZLP is
     * needed only when we return fewer bytes than asked AND the count is a
     * nonzero exact multiple of the max packet (so no short packet terminates). */
    return (sent != 0u && sent < wlength && (sent % maxpacket) == 0u) ? 1u : 0u;
}

static usb_ctrl_resp stall(void)
{
    usb_ctrl_resp r = { USB_CTRL_STALL, 0, 0, 0, -1, -1 };
    return r;
}

static usb_ctrl_resp status(void)
{
    usb_ctrl_resp r = { USB_CTRL_STATUS, 0, 0, 0, -1, -1 };
    return r;
}

/* Build an IN data response: cap to wLength and apply the ZLP rule. */
static usb_ctrl_resp tx_data(const uint8_t *data, uint16_t full_len, uint16_t wlen)
{
    uint16_t len = (full_len > wlen) ? wlen : full_len;
    usb_ctrl_resp r = { USB_CTRL_TX_DATA, data, len,
                        usb_ctrl_zlp_needed(len, wlen, USB_CDC_EP0_MAXPACKET),
                        -1, -1 };
    return r;
}

static usb_ctrl_resp resolve_get_descriptor(const uint8_t setup[8], uint16_t wlen,
                                            uint8_t *scratch, size_t scratch_len)
{
    uint8_t type = setup[3];    /* wValue high = descriptor type  */
    switch (type) {
    case USB_DT_DEVICE:
        return tx_data(usb_device_desc, USB_CDC_DEVICE_LEN, wlen);
    case USB_DT_CONFIG:
        return tx_data(usb_config_desc, USB_CDC_CONFIG_TOTAL_LEN, wlen);
    case USB_DT_STRING: {
        uint8_t index = setup[2];   /* wValue low = descriptor index */
        size_t n = usb_desc_string(index, scratch, scratch_len);
        return (n == 0) ? stall() : tx_data(scratch, (uint16_t)n, wlen);
    }
    default:
        return stall();         /* e.g. DEVICE_QUALIFIER on an FS-only device */
    }
}

static usb_ctrl_resp resolve_standard(const uint8_t setup[8], uint8_t configured,
                                      uint16_t wlen, uint8_t *scratch, size_t scratch_len)
{
    /* Validate the full SETUP tuple per request (USB 2.0 §9.3-9.4): a request
     * with the wrong direction, recipient, wValue, wIndex, or wLength is a
     * protocol error and must STALL rather than be silently accepted. */
    uint8_t  dir    = setup[0] & RT_DIR_IN;
    uint8_t  recip  = setup[0] & RT_RECIP_MASK;
    uint16_t wvalue = (uint16_t)(setup[2] | ((uint16_t)setup[3] << 8));
    uint16_t windex = (uint16_t)(setup[4] | ((uint16_t)setup[5] << 8));

    switch (setup[1]) {         /* bRequest */
    case REQ_GET_DESCRIPTOR:
        if (dir != RT_DIR_IN || recip != RT_RECIP_DEVICE)
            return stall();
        return resolve_get_descriptor(setup, wlen, scratch, scratch_len);
    case REQ_SET_ADDRESS: {
        /* Host-to-device, no data stage; address is 0..127 (USB 2.0 §9.4.6).
         * An out-of-range or malformed request STALLs (do not mask the value). */
        if (dir == RT_DIR_IN || recip != RT_RECIP_DEVICE ||
            wvalue > 0x7Fu || windex != 0u || wlen != 0u)
            return stall();
        usb_ctrl_resp r = status();
        r.set_address = (int16_t)wvalue;
        return r;
    }
    case REQ_SET_CONFIGURATION:
        if (dir == RT_DIR_IN || recip != RT_RECIP_DEVICE ||
            windex != 0u || wlen != 0u || (wvalue != 0u && wvalue != 1u))
            return stall();     /* no such configuration / malformed request */
        {
            usb_ctrl_resp r = status();
            r.set_config = (int8_t)wvalue;
            return r;
        }
    case REQ_GET_CONFIGURATION:
        if (dir != RT_DIR_IN || recip != RT_RECIP_DEVICE ||
            wvalue != 0u || windex != 0u || wlen != 1u)
            return stall();
        if (scratch_len < 1u)
            return stall();
        scratch[0] = configured ? 1u : 0u;
        return tx_data(scratch, 1u, wlen);
    case REQ_GET_STATUS:
        if (dir != RT_DIR_IN || wvalue != 0u || wlen != 2u)
            return stall();
        /* wIndex must name a recipient the device actually has (USB 2.0 §9.4.5):
         * device → 0, interface → a real interface, endpoint → a real endpoint. */
        if (recip == RT_RECIP_DEVICE) {
            if (windex != 0u)
                return stall();
        } else if (recip == RT_RECIP_INTERFACE) {
            if (windex >= USB_CDC_NUM_INTERFACES)
                return stall();
        } else if (recip == RT_RECIP_ENDPOINT) {
            if (!valid_endpoint(windex))
                return stall();
        } else {
            return stall();
        }
        if (scratch_len < 2u)
            return stall();
        /* Device: self-powered (bit0), no remote wakeup — must match the config
         * descriptor's bmAttributes (USB 2.0 §9.4.5). Interface status is
         * reserved-zero; endpoint status reports halt, and this device never
         * halts an endpoint, so both are 0x0000 (§9.4.5). */
        scratch[0] = (recip == RT_RECIP_DEVICE) ? 0x01u : 0x00u;
        scratch[1] = 0x00u;
        return tx_data(scratch, 2u, wlen);
    case REQ_CLEAR_FEATURE:
    case REQ_SET_FEATURE:
        /* Host-to-device, no data stage. The only feature this device honors is
         * ENDPOINT_HALT (as a no-op — no data endpoint is ever halted). Device
         * remote-wakeup is unsupported, so it and any malformed tuple STALL
         * rather than being blindly ACKed (USB 2.0 §9.4.1, §9.4.9). */
        if (dir == RT_DIR_IN || wlen != 0u)
            return stall();
        if (recip == RT_RECIP_ENDPOINT && wvalue == FEAT_ENDPOINT_HALT &&
            valid_endpoint(windex))
            return status();
        return stall();
    default:
        return stall();
    }
}

static usb_ctrl_resp resolve_class(const uint8_t setup[8], uint16_t wlen,
                                   const uint8_t *line_coding)
{
    /* All CDC-ACM management requests target the communication interface
     * (CDC PSTN §6.3): reject any other recipient, and any wIndex other than the
     * comm interface (e.g. a request aimed at the data interface 1 must STALL). */
    uint8_t  dir    = setup[0] & RT_DIR_IN;
    uint8_t  recip  = setup[0] & RT_RECIP_MASK;
    uint16_t wvalue = (uint16_t)(setup[2] | ((uint16_t)setup[3] << 8));
    uint16_t windex = (uint16_t)(setup[4] | ((uint16_t)setup[5] << 8));

    if (recip != RT_RECIP_INTERFACE || windex != USB_CDC_COMM_INTERFACE)
        return stall();

    switch (setup[1]) {         /* bRequest */
    case CDC_GET_LINE_CODING:
        /* Echo the host's last-set line coding (CDC PSTN §6.3.11). The probe's
         * UART baud is fixed at compile time, so this only keeps the CDC device
         * spec-correct; the bytes do not retune the link. */
        if (dir != RT_DIR_IN || wvalue != 0u || wlen != USB_CDC_LINE_CODING_LEN)
            return stall();
        return tx_data(line_coding, USB_CDC_LINE_CODING_LEN, wlen);
    case CDC_SET_LINE_CODING: {
        if (dir == RT_DIR_IN || wvalue != 0u || wlen != USB_CDC_LINE_CODING_LEN)
            return stall();
        usb_ctrl_resp r = { USB_CTRL_EXPECT_OUT, 0, 0, 0, -1, -1 };
        return r;               /* 7-byte OUT stage follows; caller stores it */
    }
    case CDC_SET_CONTROL_LINE_STATE:
        if (dir == RT_DIR_IN || wlen != 0u)
            return stall();
        return status();        /* DTR/RTS in wValue — no data stage */
    default:
        return stall();
    }
}

usb_ctrl_resp usb_ctrl_resolve(const uint8_t setup[8], uint8_t configured,
                               uint8_t *scratch, size_t scratch_len,
                               const uint8_t *line_coding)
{
    uint16_t wlen = (uint16_t)(setup[6] | ((uint16_t)setup[7] << 8));
    switch (setup[0] & RT_TYPE_MASK) {
    case RT_TYPE_STD:   return resolve_standard(setup, configured, wlen, scratch, scratch_len);
    case RT_TYPE_CLASS: return resolve_class(setup, wlen, line_coding);
    default:            return stall();
    }
}

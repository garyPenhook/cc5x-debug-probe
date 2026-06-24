/*
 * usb_desc.c — CDC-ACM descriptor data (see usb_desc.h). Pure, host-tested.
 *
 * Layout references: USB 2.0 spec §9.5/§9.6; USB CDC 1.2 (PSTN) §5 for the
 * class-specific functional descriptors. Lengths are checked in the host test.
 */
#include "usb_desc.h"

/* Device descriptor (USB 2.0 Table 9-8). bDeviceClass=0x02 (CDC) so the host
 * applies the CDC driver from the device level. bMaxPacketSize0 = 64. */
const uint8_t usb_device_desc[USB_CDC_DEVICE_LEN] = {
    18,                 /* bLength                                            */
    USB_DT_DEVICE,      /* bDescriptorType = DEVICE                           */
    0x00, 0x02,         /* bcdUSB = 2.00                                      */
    0x02,               /* bDeviceClass = Communications (CDC)                */
    0x00,               /* bDeviceSubClass                                    */
    0x00,               /* bDeviceProtocol                                    */
    USB_CDC_EP0_MAXPACKET, /* bMaxPacketSize0 = 64                            */
    0x83, 0x04,         /* idVendor  = 0x0483 (STMicroelectronics)            */
    0x40, 0x57,         /* idProduct = 0x5740 (ST Virtual COM Port)           */
    0x00, 0x02,         /* bcdDevice = 2.00                                   */
    0x01,               /* iManufacturer = string 1                          */
    0x02,               /* iProduct      = string 2                          */
    0x03,               /* iSerialNumber = string 3                          */
    0x01,               /* bNumConfigurations = 1                            */
};

/* Configuration descriptor + CDC-ACM interfaces/endpoints (67 bytes total).
 * Comm interface (0): 1 interrupt-IN notification endpoint (EP2 IN, 0x82).
 * Data interface (1): bulk OUT (EP1 OUT, 0x01) + bulk IN (EP1 IN, 0x81). */
const uint8_t usb_config_desc[USB_CDC_CONFIG_TOTAL_LEN] = {
    /* --- Configuration descriptor (USB 2.0 Table 9-10), 9 bytes --- */
    9,                  /* bLength                                            */
    USB_DT_CONFIG,      /* bDescriptorType = CONFIGURATION                    */
    USB_CDC_CONFIG_TOTAL_LEN, 0x00, /* wTotalLength = 67                      */
    0x02,               /* bNumInterfaces = 2                                 */
    0x01,               /* bConfigurationValue = 1                            */
    0x00,               /* iConfiguration                                     */
    0xC0,               /* bmAttributes = self-powered (bus is ST-LINK fed)   */
    0x32,               /* bMaxPower = 100 mA (50 * 2)                        */

    /* --- Interface 0: CDC Communications class, 9 bytes --- */
    9, USB_DT_INTERFACE,
    0x00,               /* bInterfaceNumber = 0                               */
    0x00,               /* bAlternateSetting                                 */
    0x01,               /* bNumEndpoints = 1 (notification)                  */
    0x02,               /* bInterfaceClass = Communications                  */
    0x02,               /* bInterfaceSubClass = Abstract Control Model       */
    0x01,               /* bInterfaceProtocol = AT commands (V.250)          */
    0x00,               /* iInterface                                        */

    /* --- CDC Header functional descriptor (CDC 1.2 §5.2.3.1), 5 bytes --- */
    5, USB_DT_CS_INTERFACE,
    0x00,               /* bDescriptorSubtype = Header                       */
    0x10, 0x01,         /* bcdCDC = 1.10                                     */

    /* --- CDC Call Management functional descriptor, 5 bytes --- */
    5, USB_DT_CS_INTERFACE,
    0x01,               /* bDescriptorSubtype = Call Management              */
    0x00,               /* bmCapabilities = none                             */
    0x01,               /* bDataInterface = 1                                */

    /* --- CDC ACM functional descriptor, 4 bytes --- */
    4, USB_DT_CS_INTERFACE,
    0x02,               /* bDescriptorSubtype = Abstract Control Management  */
    0x02,               /* bmCapabilities = Set/Get Line Coding + Line State */

    /* --- CDC Union functional descriptor, 5 bytes --- */
    5, USB_DT_CS_INTERFACE,
    0x06,               /* bDescriptorSubtype = Union                        */
    0x00,               /* bControlInterface = 0                             */
    0x01,               /* bSubordinateInterface0 = 1                        */

    /* --- Endpoint: notification, interrupt IN EP2 (0x82), 7 bytes --- */
    7, USB_DT_ENDPOINT,
    0x82,               /* bEndpointAddress = EP2 IN                         */
    0x03,               /* bmAttributes = Interrupt                          */
    USB_CDC_NOTIF_MAXPACKET, 0x00, /* wMaxPacketSize = 8                     */
    0xFF,               /* bInterval = 255 ms                                */

    /* --- Interface 1: CDC Data class, 9 bytes --- */
    9, USB_DT_INTERFACE,
    0x01,               /* bInterfaceNumber = 1                               */
    0x00,               /* bAlternateSetting                                 */
    0x02,               /* bNumEndpoints = 2 (bulk IN + bulk OUT)            */
    0x0A,               /* bInterfaceClass = CDC Data                        */
    0x00,               /* bInterfaceSubClass                                */
    0x00,               /* bInterfaceProtocol                                */
    0x00,               /* iInterface                                        */

    /* --- Endpoint: bulk OUT EP1 (0x01), 7 bytes --- */
    7, USB_DT_ENDPOINT,
    0x01,               /* bEndpointAddress = EP1 OUT                        */
    0x02,               /* bmAttributes = Bulk                               */
    USB_CDC_BULK_MAXPACKET, 0x00, /* wMaxPacketSize = 64                     */
    0x00,               /* bInterval (ignored for bulk)                     */

    /* --- Endpoint: bulk IN EP1 (0x81), 7 bytes --- */
    7, USB_DT_ENDPOINT,
    0x81,               /* bEndpointAddress = EP1 IN                         */
    0x02,               /* bmAttributes = Bulk                               */
    USB_CDC_BULK_MAXPACKET, 0x00, /* wMaxPacketSize = 64                     */
    0x00,               /* bInterval                                        */
};

/* ASCII source strings; usb_desc_string() expands them to UTF-16LE on demand
 * so we never hand-encode wide characters (and can't miscount their length). */
static const char *const k_strings[] = {
    0,                            /* index 0 is the LANGID table, handled below */
    "cc5x-helper",                /* 1: iManufacturer */
    "CC5X Debug Probe (CDL)",     /* 2: iProduct */
    "P5B-0001",                   /* 3: iSerialNumber */
};
#define NUM_STRINGS (sizeof k_strings / sizeof k_strings[0])

size_t usb_desc_string(uint8_t i, uint8_t *buf, size_t buflen)
{
    if (i == 0u) {                /* LANGID descriptor: English (US) = 0x0409 */
        if (buflen < 4u)
            return 0;
        buf[0] = 4u;
        buf[1] = USB_DT_STRING;
        buf[2] = 0x09u;
        buf[3] = 0x04u;
        return 4u;
    }
    if (i >= NUM_STRINGS || k_strings[i] == 0)
        return 0;

    const char *s = k_strings[i];
    size_t n = 0;
    while (s[n] != '\0')
        n++;
    size_t total = 2u + 2u * n;   /* header + UTF-16LE chars */
    if (total > 255u || total > buflen)
        return 0;

    buf[0] = (uint8_t)total;
    buf[1] = USB_DT_STRING;
    for (size_t k = 0; k < n; k++) {
        buf[2u + 2u * k] = (uint8_t)s[k];   /* low byte = ASCII */
        buf[3u + 2u * k] = 0x00u;           /* high byte = 0 (Basic Latin) */
    }
    return total;
}

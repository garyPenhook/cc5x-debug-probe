/*
 * test_usb_desc.c — host unit tests for the USB-CDC descriptor set (usb_desc.c).
 * Validates the static descriptors the bare-metal driver serves during
 * enumeration — the most error-prone hand-written data in the USB path — without
 * any hardware. Returns non-zero on any failure.
 */
#include "usb_desc.h"
#include <stdio.h>
#include <string.h>

static int fail(const char *m) { printf("  FAIL: %s\n", m); return 1; }

static int test_device_desc(void)
{
    printf("test_device_desc\n");
    const uint8_t *d = usb_device_desc;
    if (d[0] != 18) return fail("bLength != 18");
    if (d[1] != USB_DT_DEVICE) return fail("bDescriptorType");
    if (d[7] != USB_CDC_EP0_MAXPACKET) return fail("bMaxPacketSize0");
    if (d[2] != 0x00 || d[3] != 0x02) return fail("bcdUSB != 2.00");
    if (d[4] != 0x02) return fail("bDeviceClass != CDC");
    /* idVendor 0x0483, idProduct 0x5740 (little-endian). */
    if (d[8] != 0x83 || d[9] != 0x04) return fail("idVendor");
    if (d[10] != 0x40 || d[11] != 0x57) return fail("idProduct");
    if (d[17] != 1) return fail("bNumConfigurations");
    printf("  ok\n");
    return 0;
}

/* Walk the configuration descriptor; verify the sub-descriptor lengths sum to
 * wTotalLength, and that the interface/endpoint inventory is the CDC-ACM set. */
static int test_config_desc(void)
{
    printf("test_config_desc\n");
    const uint8_t *c = usb_config_desc;
    if (c[0] != 9 || c[1] != USB_DT_CONFIG) return fail("config header");

    unsigned wtotal = (unsigned)c[2] | ((unsigned)c[3] << 8);
    if (wtotal != USB_CDC_CONFIG_TOTAL_LEN) return fail("wTotalLength mismatch");
    if (wtotal != sizeof usb_config_desc) return fail("wTotalLength != array size");
    if (c[4] != 2) return fail("bNumInterfaces != 2");

    unsigned off = 0, ifaces = 0, eps = 0, cs = 0;
    unsigned ep_addrs = 0;            /* bitmask of seen bEndpointAddress bytes */
    while (off + 2u <= wtotal) {
        unsigned blen = c[off];
        unsigned btype = c[off + 1u];
        if (blen == 0 || off + blen > wtotal) return fail("sub-descriptor overrun");
        if (btype == USB_DT_INTERFACE) ifaces++;
        else if (btype == USB_DT_CS_INTERFACE) cs++;
        else if (btype == USB_DT_ENDPOINT) {
            eps++;
            unsigned addr = c[off + 2u];
            if (addr == 0x82u) ep_addrs |= 1u;       /* notify  IN  */
            else if (addr == 0x01u) ep_addrs |= 2u;  /* data    OUT */
            else if (addr == 0x81u) ep_addrs |= 4u;  /* data    IN  */
            else return fail("unexpected endpoint address");
        }
        off += blen;
    }
    if (off != wtotal) return fail("descriptors don't sum to wTotalLength");
    if (ifaces != 2) return fail("interface count");
    if (cs != 4) return fail("expected 4 CDC functional descriptors");
    if (eps != 3) return fail("endpoint count != 3");
    if (ep_addrs != 7u) return fail("missing one of EP 0x82/0x01/0x81");
    printf("  ok (wTotalLength=%u, %u ifaces, %u eps)\n", wtotal, ifaces, eps);
    return 0;
}

static int test_strings(void)
{
    printf("test_strings\n");
    uint8_t buf[64];

    size_t n = usb_desc_string(0, buf, sizeof buf);   /* LANGID */
    if (n != 4 || buf[1] != USB_DT_STRING || buf[2] != 0x09 || buf[3] != 0x04)
        return fail("langid");

    for (uint8_t i = 1; i <= 3; i++) {
        n = usb_desc_string(i, buf, sizeof buf);
        if (n == 0) return fail("string missing");
        if (buf[0] != n) return fail("bLength != bytes written");
        if (buf[1] != USB_DT_STRING) return fail("string type");
        if (((n - 2) % 2) != 0) return fail("odd UTF-16 payload");
        if (buf[3] != 0x00) return fail("expected Basic-Latin high byte 0");
    }

    if (usb_desc_string(9, buf, sizeof buf) != 0) return fail("OOB index not rejected");
    if (usb_desc_string(2, buf, 3) != 0) return fail("small buffer not rejected");
    printf("  ok\n");
    return 0;
}

int main(void)
{
    int rc = 0;
    rc |= test_device_desc();
    rc |= test_config_desc();
    rc |= test_strings();
    printf(rc ? "FAIL\n" : "ALL PASS\n");
    return rc;
}

/*
 * test_usb_ctrl.c — host unit tests for the pure USB control-request resolver
 * (usb_ctrl.c). Exercises the EP0 decision logic that the bench cannot reach:
 * GET_DESCRIPTOR selection + wLength capping + the terminating-ZLP rule,
 * SET_ADDRESS, SET_CONFIGURATION(0/1/invalid), GET_STATUS / GET_CONFIGURATION,
 * and the CDC class requests. Returns non-zero on any failure.
 */
#include "usb_ctrl.h"
#include "usb_desc.h"
#include <stdio.h>
#include <string.h>

static int fail(const char *m) { printf("  FAIL: %s\n", m); return 1; }

static uint8_t g_scratch[64];

/* Build a SETUP packet. */
static void mk(uint8_t s[8], uint8_t bmReq, uint8_t bReq,
              uint16_t wValue, uint16_t wIndex, uint16_t wLength)
{
    s[0] = bmReq; s[1] = bReq;
    s[2] = (uint8_t)wValue;  s[3] = (uint8_t)(wValue >> 8);
    s[4] = (uint8_t)wIndex;  s[5] = (uint8_t)(wIndex >> 8);
    s[6] = (uint8_t)wLength; s[7] = (uint8_t)(wLength >> 8);
}

static usb_ctrl_resp resolve(uint8_t bmReq, uint8_t bReq, uint16_t wValue,
                             uint16_t wIndex, uint16_t wLength, uint8_t cfg)
{
    uint8_t s[8];
    mk(s, bmReq, bReq, wValue, wIndex, wLength);
    return usb_ctrl_resolve(s, cfg, g_scratch, sizeof g_scratch);
}

#define GET_DESC 0x06u
#define DT_DEVICE 0x01u
#define DT_CONFIG 0x02u
#define DT_STRING 0x03u
#define FEAT_ENDPOINT_HALT_VAL 0x00u   /* feature selector ENDPOINT_HALT */

static int test_zlp_rule(void)
{
    printf("test_zlp_rule\n");
    if (usb_ctrl_zlp_needed(64, 255, 64) != 1) return fail("64<255 mult → ZLP");
    if (usb_ctrl_zlp_needed(128, 255, 64) != 1) return fail("128<255 mult → ZLP");
    if (usb_ctrl_zlp_needed(64, 64, 64) != 0)  return fail("len==wLength → no ZLP");
    if (usb_ctrl_zlp_needed(0, 255, 64) != 0)  return fail("0 bytes → no ZLP");
    if (usb_ctrl_zlp_needed(18, 64, 64) != 0)  return fail("non-multiple → no ZLP");
    if (usb_ctrl_zlp_needed(67, 255, 64) != 0) return fail("67 non-multiple → no ZLP");
    printf("  ok\n");
    return 0;
}

static int test_get_descriptor(void)
{
    printf("test_get_descriptor\n");
    /* DEVICE, generous wLength → full 18 bytes, no ZLP. */
    usb_ctrl_resp r = resolve(0x80, GET_DESC, DT_DEVICE << 8, 0, 64, 0);
    if (r.kind != USB_CTRL_TX_DATA || r.data != usb_device_desc ||
        r.len != USB_CDC_DEVICE_LEN || r.zlp != 0)
        return fail("device descriptor");

    /* DEVICE, wLength 8 → capped to 8, no ZLP. */
    r = resolve(0x80, GET_DESC, DT_DEVICE << 8, 0, 8, 0);
    if (r.kind != USB_CTRL_TX_DATA || r.len != 8 || r.zlp != 0)
        return fail("device descriptor capped to 8");

    /* CONFIG, wLength 0xFF → 67 bytes, no ZLP (67 not a multiple of 64). */
    r = resolve(0x80, GET_DESC, DT_CONFIG << 8, 0, 0xFF, 0);
    if (r.kind != USB_CTRL_TX_DATA || r.data != usb_config_desc ||
        r.len != USB_CDC_CONFIG_TOTAL_LEN || r.zlp != 0)
        return fail("config descriptor");

    /* CONFIG, wLength exactly 64 → capped to 64, NO ZLP (regression: a ZLP here
     * was the bug — host asked for 64, got 64, transfer complete). */
    r = resolve(0x80, GET_DESC, DT_CONFIG << 8, 0, 64, 0);
    if (r.kind != USB_CTRL_TX_DATA || r.len != 64 || r.zlp != 0)
        return fail("config wLength==64 must not append ZLP");

    /* STRING index 0 (LANGID) → 4 bytes. */
    r = resolve(0x80, GET_DESC, DT_STRING << 8, 0, 255, 0);
    if (r.kind != USB_CTRL_TX_DATA || r.len != 4)
        return fail("langid string");

    /* STRING out-of-range index → STALL. */
    r = resolve(0x80, GET_DESC, (DT_STRING << 8) | 9u, 0, 255, 0);
    if (r.kind != USB_CTRL_STALL) return fail("OOB string must STALL");

    /* Unknown descriptor type (e.g. DEVICE_QUALIFIER 0x06) → STALL. */
    r = resolve(0x80, GET_DESC, 0x06 << 8, 0, 255, 0);
    if (r.kind != USB_CTRL_STALL) return fail("unknown descriptor must STALL");
    printf("  ok\n");
    return 0;
}

static int test_set_address(void)
{
    printf("test_set_address\n");
    usb_ctrl_resp r = resolve(0x00, 0x05, 5, 0, 0, 0);   /* SET_ADDRESS 5 */
    if (r.kind != USB_CTRL_STATUS || r.set_address != 5 || r.set_config != -1)
        return fail("SET_ADDRESS 5");
    r = resolve(0x00, 0x05, 0, 0, 0, 0);                 /* SET_ADDRESS 0 (legal) */
    if (r.kind != USB_CTRL_STATUS || r.set_address != 0)
        return fail("SET_ADDRESS 0");
    printf("  ok\n");
    return 0;
}

static int test_set_configuration(void)
{
    printf("test_set_configuration\n");
    usb_ctrl_resp r = resolve(0x00, 0x09, 1, 0, 0, 0);   /* SET_CONFIGURATION 1 */
    if (r.kind != USB_CTRL_STATUS || r.set_config != 1 || r.set_address != -1)
        return fail("SET_CONFIGURATION 1");
    r = resolve(0x00, 0x09, 0, 0, 0, 1);                 /* SET_CONFIGURATION 0 */
    if (r.kind != USB_CTRL_STATUS || r.set_config != 0)
        return fail("SET_CONFIGURATION 0");
    r = resolve(0x00, 0x09, 2, 0, 0, 0);                 /* invalid config → STALL */
    if (r.kind != USB_CTRL_STALL) return fail("SET_CONFIGURATION 2 must STALL");
    printf("  ok\n");
    return 0;
}

static int test_status_and_config_query(void)
{
    printf("test_status_and_config_query\n");
    /* GET_STATUS device: 2 bytes, self-powered (bit0)=1, must match the config
     * descriptor's bmAttributes self-powered bit (offset 7, bit6). */
    usb_ctrl_resp r = resolve(0x80, 0x00, 0, 0, 2, 0);
    if (r.kind != USB_CTRL_TX_DATA || r.len != 2)
        return fail("GET_STATUS shape");
    uint8_t self_powered_status = r.data[0] & 0x01u;
    uint8_t self_powered_desc   = (usb_config_desc[7] >> 6) & 0x01u;
    if (self_powered_status != self_powered_desc)
        return fail("GET_STATUS self-powered disagrees with bmAttributes");

    /* GET_CONFIGURATION reflects the current configured flag. */
    r = resolve(0x80, 0x08, 0, 0, 1, 1);
    if (r.kind != USB_CTRL_TX_DATA || r.len != 1 || r.data[0] != 1)
        return fail("GET_CONFIGURATION configured");
    r = resolve(0x80, 0x08, 0, 0, 1, 0);
    if (r.kind != USB_CTRL_TX_DATA || r.data[0] != 0)
        return fail("GET_CONFIGURATION unconfigured");
    printf("  ok\n");
    return 0;
}

static int test_class_requests(void)
{
    printf("test_class_requests\n");
    /* GET_LINE_CODING → 7 bytes. */
    usb_ctrl_resp r = resolve(0xA1, 0x21, 0, 0, 7, 1);
    if (r.kind != USB_CTRL_TX_DATA || r.len != 7) return fail("GET_LINE_CODING");
    /* SET_LINE_CODING → expect an OUT data stage. */
    r = resolve(0x21, 0x20, 0, 0, 7, 1);
    if (r.kind != USB_CTRL_EXPECT_OUT) return fail("SET_LINE_CODING → EXPECT_OUT");
    /* SET_CONTROL_LINE_STATE → status only. */
    r = resolve(0x21, 0x22, 0x03, 0, 0, 1);
    if (r.kind != USB_CTRL_STATUS) return fail("SET_CONTROL_LINE_STATE → STATUS");
    /* Unknown class request → STALL. */
    r = resolve(0x21, 0x99, 0, 0, 0, 1);
    if (r.kind != USB_CTRL_STALL) return fail("unknown class → STALL");
    /* Vendor request type → STALL. */
    r = resolve(0x40, 0x01, 0, 0, 0, 1);
    if (r.kind != USB_CTRL_STALL) return fail("vendor type → STALL");
    printf("  ok\n");
    return 0;
}

/* Malformed SETUP tuples (wrong direction / recipient / wValue / wIndex /
 * wLength) must STALL rather than be silently accepted (USB 2.0 §9.3-9.4). */
static int test_setup_validation(void)
{
    printf("test_setup_validation\n");
    usb_ctrl_resp r;

    /* SET_ADDRESS out of range (>127) must STALL, not be masked to 0x7F. */
    r = resolve(0x00, 0x05, 200, 0, 0, 0);
    if (r.kind != USB_CTRL_STALL) return fail("SET_ADDRESS 200 must STALL");
    /* SET_ADDRESS with a nonzero wIndex / wLength is malformed → STALL. */
    r = resolve(0x00, 0x05, 5, 1, 0, 0);
    if (r.kind != USB_CTRL_STALL) return fail("SET_ADDRESS wIndex!=0 must STALL");
    r = resolve(0x00, 0x05, 5, 0, 1, 0);
    if (r.kind != USB_CTRL_STALL) return fail("SET_ADDRESS wLength!=0 must STALL");
    /* Wrong direction (device-to-host) on a no-data request → STALL. */
    r = resolve(0x80, 0x05, 5, 0, 0, 0);
    if (r.kind != USB_CTRL_STALL) return fail("SET_ADDRESS dir=IN must STALL");

    /* GET_DESCRIPTOR with OUT direction → STALL. */
    r = resolve(0x00, GET_DESC, DT_DEVICE << 8, 0, 64, 0);
    if (r.kind != USB_CTRL_STALL) return fail("GET_DESCRIPTOR dir=OUT must STALL");

    /* GET_STATUS interface/endpoint recipients: reserved/halt status = 0x0000,
     * NOT the device self-powered byte. */
    r = resolve(0x81, 0x00, 0, 0, 2, 0);     /* recipient = interface */
    if (r.kind != USB_CTRL_TX_DATA || r.len != 2 || r.data[0] != 0 || r.data[1] != 0)
        return fail("GET_STATUS interface must be 0x0000");
    r = resolve(0x82, 0x00, 0, 0, 2, 0);     /* recipient = endpoint */
    if (r.kind != USB_CTRL_TX_DATA || r.data[0] != 0)
        return fail("GET_STATUS endpoint (not halted) must be 0x0000");
    /* GET_STATUS with wrong wLength → STALL. */
    r = resolve(0x80, 0x00, 0, 0, 1, 0);
    if (r.kind != USB_CTRL_STALL) return fail("GET_STATUS wLength!=2 must STALL");

    /* CLEAR/SET_FEATURE: endpoint-halt is a no-op ACK; device remote-wakeup and
     * malformed tuples STALL (no more blind ACK). */
    r = resolve(0x02, 0x01, FEAT_ENDPOINT_HALT_VAL, 0x81, 0, 0);  /* CLEAR ep halt */
    if (r.kind != USB_CTRL_STATUS) return fail("CLEAR_FEATURE ep halt → STATUS");
    r = resolve(0x00, 0x03, 1, 0, 0, 0);     /* SET_FEATURE device remote wakeup */
    if (r.kind != USB_CTRL_STALL) return fail("SET_FEATURE remote-wakeup must STALL");
    r = resolve(0x02, 0x03, FEAT_ENDPOINT_HALT_VAL, 0x81, 2, 0); /* wLength!=0 */
    if (r.kind != USB_CTRL_STALL) return fail("SET_FEATURE wLength!=0 must STALL");

    /* Class request to a non-interface recipient → STALL. */
    r = resolve(0xA0, 0x21, 0, 0, 7, 1);     /* GET_LINE_CODING, recipient=device */
    if (r.kind != USB_CTRL_STALL) return fail("class to device recipient must STALL");
    /* SET_LINE_CODING with wrong wLength → STALL. */
    r = resolve(0x21, 0x20, 0, 0, 6, 1);
    if (r.kind != USB_CTRL_STALL) return fail("SET_LINE_CODING wLength!=7 must STALL");
    printf("  ok\n");
    return 0;
}

int main(void)
{
    int rc = 0;
    rc |= test_zlp_rule();
    rc |= test_get_descriptor();
    rc |= test_set_address();
    rc |= test_set_configuration();
    rc |= test_status_and_config_query();
    rc |= test_class_requests();
    rc |= test_setup_validation();
    printf(rc ? "FAIL\n" : "ALL PASS\n");
    return rc;
}

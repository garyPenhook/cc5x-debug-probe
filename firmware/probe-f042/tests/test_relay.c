/*
 * test_relay.c — host unit tests for the portable relay core + bytefifo.
 * Builds with host gcc (no hardware). Covers framing/escaping, all-or-nothing
 * TX, burst timestamping, and drop reporting. Returns non-zero on any failure.
 */
#include "relay.h"
#include "bytefifo.h"
#include <stdio.h>
#include <string.h>

/* ---- capture sink ---------------------------------------------------------- */
static uint8_t  g_out[65536];
static size_t   g_out_len;
static int      g_reject_n;     /* reject (return 0) this many times, then accept */

static size_t sink(const uint8_t *buf, size_t len)
{
    if (g_reject_n > 0) { g_reject_n--; return 0; }   /* all-or-nothing reject */
    if (g_out_len + len > sizeof g_out) return 0;
    memcpy(g_out + g_out_len, buf, len);
    g_out_len += len;
    return len;
}

static uint8_t crc8(uint8_t c, uint8_t b)
{
    c ^= b;
    for (int i = 0; i < 8; i++)
        c = (c & 0x80u) ? (uint8_t)((c << 1) ^ 0x07u) : (uint8_t)(c << 1);
    return c;
}

/* Parsed frame. */
struct frame { int type, seq; uint32_t ts; uint32_t dropped; uint8_t data[64]; int dlen; };

/* Decode the captured SLIP stream into frames[]; returns count or -1 on error.
 * Verifies FLAG framing, un-stuffing, CRC, LEN, and SEQ monotonicity. */
static int decode_all(struct frame *out, int max)
{
    int n = 0, prev_seq = -1;
    size_t i = 0;
    while (i < g_out_len) {
        if (g_out[i] != RELAY_FLAG) { printf("no SOF at %zu\n", i); return -1; }
        i++;
        uint8_t inner[128]; size_t m = 0;
        while (i < g_out_len && g_out[i] != RELAY_FLAG) {
            uint8_t b = g_out[i++];
            if (b == RELAY_ESC) {
                if (i >= g_out_len) { printf("dangling ESC\n"); return -1; }
                uint8_t e = g_out[i++];
                b = (e == RELAY_ESC_FLAG) ? RELAY_FLAG : (e == RELAY_ESC_ESC) ? RELAY_ESC : 0xFF;
                if (b == 0xFF && e != RELAY_ESC_FLAG) { printf("bad escape\n"); return -1; }
            }
            if (m < sizeof inner) inner[m++] = b;
        }
        if (i >= g_out_len) { printf("no closing FLAG\n"); return -1; }
        i++;
        if (m < 4) { printf("frame too short\n"); return -1; }
        uint8_t c = 0;
        for (size_t k = 0; k < m - 1; k++) c = crc8(c, inner[k]);
        if (c != inner[m - 1]) { printf("CRC mismatch\n"); return -1; }
        uint8_t type = inner[0], seq = inner[1], len = inner[2];
        if (len + 3u != m - 1u) { printf("LEN mismatch\n"); return -1; }
        if (prev_seq != -1 && seq != (uint8_t)(prev_seq + 1)) { printf("SEQ gap %d->%d\n", prev_seq, seq); return -1; }
        prev_seq = seq;
        if (n >= max) { printf("too many frames\n"); return -1; }
        struct frame *f = &out[n++];
        f->type = type; f->seq = seq; f->ts = 0; f->dropped = 0; f->dlen = 0;
        if (type == RELAY_FRAME_TYPE) {
            f->ts = inner[3] | (inner[4] << 8) | (inner[5] << 16) | ((uint32_t)inner[6] << 24);
            f->dlen = len - 4;
            memcpy(f->data, inner + 7, f->dlen);
        } else if (type == RELAY_STATUS_TYPE) {
            f->dropped = inner[3] | (inner[4] << 8) | (inner[5] << 16) | ((uint32_t)inner[6] << 24);
        } else { printf("unknown type 0x%02X\n", type); return -1; }
    }
    return n;
}

static int fail(const char *m) { printf("  FAIL: %s\n", m); return 1; }

static int test_framing_escapes(void)
{
    printf("test_framing_escapes\n");
    g_out_len = 0; g_reject_n = 0;
    uint8_t in[] = { 0x01, 0x7E, 0x7D, 0xAA, 0x7E, 0x00, 0xFF };  /* FLAG/ESC payloads */
    relay_init(sink);
    for (size_t k = 0; k < sizeof in; k++) relay_target_rx(in[k], 1000);  /* one burst */
    while (relay_poll()) {}
    struct frame f[8]; int nf = decode_all(f, 8);
    if (nf < 1) return fail("decode");
    uint8_t got[16]; int g = 0;
    for (int k = 0; k < nf; k++) {
        if (f[k].type != RELAY_FRAME_TYPE) return fail("unexpected status");
        if (f[k].ts != 1000) return fail("ts");
        memcpy(got + g, f[k].data, f[k].dlen); g += f[k].dlen;
    }
    if (g != (int)sizeof in || memcmp(got, in, g)) return fail("data roundtrip");
    printf("  ok (%d frame(s))\n", nf);
    return 0;
}

/* Cross-implementation anchor: the exact bytes relay.c must put on the wire for
 * one known frame (seq=0, ts=256, data={0x7E,0x7D,0x42} — chosen to exercise
 * SLIP stuffing of both FLAG and ESC). Derived by hand from the frame layout in
 * 01-debug-link-protocol.md §3, NOT from relay.c. The identical vector is
 * asserted by tools/relay_decode.py's selftest, so the C encoder and the Python
 * decoder cannot silently drift apart. If you change the wire format, update
 * BOTH and the spec. */
static int test_golden_vector(void)
{
    printf("test_golden_vector\n");
    static const uint8_t golden[] = {
        0x7E, 0xF0, 0x00, 0x07, 0x00, 0x01, 0x00, 0x00,
        0x7D, 0x5E, 0x7D, 0x5D, 0x42, 0xE2, 0x7E,
    };
    g_out_len = 0; g_reject_n = 0;
    relay_init(sink);
    uint8_t in[] = { 0x7E, 0x7D, 0x42 };
    for (size_t k = 0; k < sizeof in; k++) relay_target_rx(in[k], 256);  /* one burst */
    while (relay_poll()) {}
    if (g_out_len != sizeof golden || memcmp(g_out, golden, sizeof golden))
        return fail("encoded bytes differ from the spec golden vector");
    printf("  ok (%zu bytes match spec)\n", g_out_len);
    return 0;
}

static int test_bursts(void)
{
    printf("test_bursts\n");
    g_out_len = 0; g_reject_n = 0;
    relay_init(sink);
    /* Burst A at t=100, then a >GAP gap, burst B at t=100000. */
    for (int k = 0; k < 3; k++) relay_target_rx(0xA0 + k, 100);
    for (int k = 0; k < 3; k++) relay_target_rx(0xB0 + k, 100000);
    while (relay_poll()) {}
    struct frame f[8]; int nf = decode_all(f, 8);
    if (nf < 2) return fail("expected >=2 frames");
    /* first frame stamped 100, the frame carrying 0xB0.. stamped 100000 */
    if (f[0].ts != 100) return fail("burst A ts");
    int sawB = 0;
    for (int k = 0; k < nf; k++)
        if (f[k].dlen && f[k].data[0] == 0xB0) { sawB = 1; if (f[k].ts != 100000) return fail("burst B ts"); }
    if (!sawB) return fail("burst B missing");
    printf("  ok (A ts=100, B ts=100000)\n");
    return 0;
}

static int test_all_or_nothing_retry(void)
{
    printf("test_all_or_nothing_retry\n");
    g_out_len = 0; g_reject_n = 2;
    uint8_t in[] = { 0x11, 0x22, 0x33 };
    relay_init(sink);
    for (size_t k = 0; k < sizeof in; k++) relay_target_rx(in[k], 7);
    if (relay_poll() != 0 || relay_poll() != 0 || g_out_len != 0) return fail("reject leaked");
    if (relay_poll() == 0) return fail("never emitted after recover");
    while (relay_poll()) {}
    struct frame f[4]; int nf = decode_all(f, 4);
    if (nf != 1 || f[0].dlen != 3 || memcmp(f[0].data, in, 3)) return fail("frame");
    printf("  ok\n");
    return 0;
}

static int test_drops(void)
{
    printf("test_drops\n");
    g_out_len = 0; g_reject_n = 0;
    relay_init(sink);
    /* Flood far past the ring with no polling -> forces drops. */
    for (int k = 0; k < 4096; k++) relay_target_rx((uint8_t)k, 5000);
    if (relay_dropped() == 0) return fail("expected drops");
    while (relay_poll()) {}
    struct frame f[600]; int nf = decode_all(f, 600);
    if (nf < 1) return fail("decode");
    int saw_status = 0;
    for (int k = 0; k < nf; k++)
        if (f[k].type == RELAY_STATUS_TYPE && f[k].dropped > 0) saw_status = 1;
    if (!saw_status) return fail("no status frame reporting drops");
    printf("  ok (dropped=%u, surfaced in status frame)\n", relay_dropped());
    return 0;
}

static int test_bytefifo(void)
{
    printf("test_bytefifo\n");
    uint8_t store[8]; bytefifo_t fi; bytefifo_init(&fi, store, sizeof store);
    uint8_t a[] = {1,2,3,4,5};
    if (!bytefifo_push_all(&fi, a, 5)) return fail("push 5");
    if (bytefifo_push_all(&fi, a, 5)) return fail("overfill accepted");
    if (bytefifo_count(&fi) != 5) return fail("count");
    for (int k = 1; k <= 5; k++) { uint8_t b; if (!bytefifo_pop(&fi, &b) || b != k) return fail("order"); }
    uint8_t b; if (bytefifo_pop(&fi, &b)) return fail("pop empty");
    printf("  ok\n");
    return 0;
}

int main(void)
{
    int rc = 0;
    rc |= test_framing_escapes();
    rc |= test_golden_vector();
    rc |= test_bursts();
    rc |= test_all_or_nothing_retry();
    rc |= test_drops();
    rc |= test_bytefifo();
    printf(rc ? "FAIL\n" : "ALL PASS\n");
    return rc;
}

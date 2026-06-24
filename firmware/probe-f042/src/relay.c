/*
 * relay.c — portable timestamped relay core (P5a). See relay.h for the wire
 * format (CDL-conformant SLIP frames per 01-debug-link-protocol.md §3).
 *
 * Memory (STM32F042K6: 6 KB SRAM, DS Table 2):
 *   s_data[RING_SIZE] + s_marks[MARKS]*8 + small statics.
 *   Default 512 + 32*8 = 768 B (vs the old per-byte-timestamp 2.5 KB).
 *
 * Concurrency: single-core, single-producer (target RX ISR) / single-consumer
 * (main loop). The ISR is atomic w.r.t. the main loop on Cortex-M0; the data
 * ring and the mark ring are each SPSC with absolute (free-running) indices, so
 * no critical section is needed. RING_SIZE and MARKS must be powers of two.
 */
#include "relay.h"

#ifndef RING_SIZE
#define RING_SIZE 512u
#endif
#if (RING_SIZE & (RING_SIZE - 1u)) != 0u
#error "RING_SIZE must be a power of two"
#endif
#define RING_MASK (RING_SIZE - 1u)

#ifndef MARKS
#define MARKS 32u                 /* burst-timestamp marks (power of two) */
#endif
#if (MARKS & (MARKS - 1u)) != 0u
#error "MARKS must be a power of two"
#endif
#define MARK_MASK (MARKS - 1u)

/* Worst-case encoded size: 2 FLAGs + every inner byte stuffed (x2).
 * Inner = TYPE+SEQ+LEN+CRC (4) + ts (4) + data (RELAY_MAX_CHUNK). */
#define FRAME_INNER_MAX (4u + 4u + RELAY_MAX_CHUNK)
#define FRAME_ENC_MAX   (2u + 2u * FRAME_INNER_MAX)

struct mark { uint32_t pos; uint32_t ts; };

static volatile uint32_t s_head;          /* producer: absolute byte index */
static volatile uint32_t s_tail;          /* consumer: absolute byte index */
static uint8_t  s_data[RING_SIZE];
static volatile struct mark s_marks[MARKS];
static volatile uint32_t s_mhead;         /* producer: absolute mark index */
static volatile uint32_t s_mtail;         /* consumer: absolute mark index */
static volatile uint32_t s_last_ts;       /* producer: ts of previous byte */
static volatile uint8_t  s_have_last;     /* producer: s_last_ts valid */
static volatile uint32_t s_dropped;       /* bytes + marks dropped (overrun) */

static uint32_t s_cur_ts;                 /* consumer: ts currently in effect */
static uint32_t s_reported;               /* consumer: last dropped count sent */
static uint8_t  s_seq;

static relay_pc_write_fn s_pc_write;

/* CRC-8, poly 0x07, init 0x00 (CDL link spec, 01 §3). */
static uint8_t crc8(uint8_t crc, uint8_t b)
{
    crc ^= b;
    for (int i = 0; i < 8; i++)
        crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0x07u) : (uint8_t)(crc << 1);
    return crc;
}

void relay_init(relay_pc_write_fn pc_write)
{
    s_head = s_tail = 0;
    s_mhead = s_mtail = 0;
    s_last_ts = 0;
    s_have_last = 0;
    s_dropped = 0;
    s_cur_ts = 0;
    s_reported = 0;
    s_seq = 0;
    s_pc_write = pc_write;
}

void relay_target_rx(uint8_t byte, uint32_t ts_us)
{
    if ((s_head - s_tail) >= RING_SIZE) {     /* data ring full */
        s_dropped++;
        return;
    }
    /* Start a new timestamp burst on first byte or after an idle gap. */
    if (!s_have_last || (uint32_t)(ts_us - s_last_ts) > RELAY_GAP_US) {
        if ((s_mhead - s_mtail) < MARKS) {
            s_marks[s_mhead & MARK_MASK].pos = s_head;
            s_marks[s_mhead & MARK_MASK].ts  = ts_us;
            s_mhead++;
        } else {
            s_dropped++;                      /* lost a timestamp mark */
        }
    }
    s_last_ts = ts_us;
    s_have_last = 1;
    s_data[s_head & RING_MASK] = byte;
    s_head++;
}

void relay_note_dropped(uint32_t n) { s_dropped += n; }  /* producer/ISR context */

uint32_t relay_dropped(void) { return s_dropped; }

/* Append one inner byte with SLIP stuffing; returns new length. */
static size_t stuff(uint8_t *out, size_t n, uint8_t b)
{
    if (b == RELAY_FLAG) {
        out[n++] = RELAY_ESC;
        out[n++] = RELAY_ESC_FLAG;
    } else if (b == RELAY_ESC) {
        out[n++] = RELAY_ESC;
        out[n++] = RELAY_ESC_ESC;
    } else {
        out[n++] = b;
    }
    return n;
}

/* Encode + emit one frame (TYPE + ARG payload). Returns encoded bytes written,
 * or 0 if the sink could not take the whole frame (caller retries). On success
 * the per-frame SEQ is advanced. */
static size_t emit(uint8_t type, const uint8_t *arg, uint8_t arglen)
{
    uint8_t inner[FRAME_INNER_MAX];
    uint8_t m = 0;
    inner[m++] = type;
    inner[m++] = s_seq;
    inner[m++] = arglen;
    for (uint8_t i = 0; i < arglen; i++)
        inner[m++] = arg[i];

    uint8_t c = 0;
    for (uint8_t i = 0; i < m; i++)
        c = crc8(c, inner[i]);

    uint8_t enc[FRAME_ENC_MAX];
    size_t  e = 0;
    enc[e++] = RELAY_FLAG;
    for (uint8_t i = 0; i < m; i++)
        e = stuff(enc, e, inner[i]);
    e = stuff(enc, e, c);
    enc[e++] = RELAY_FLAG;

    if (s_pc_write(enc, e) != e)
        return 0;
    s_seq++;
    return e;
}

size_t relay_poll(void)
{
    if (s_pc_write == 0)
        return 0;

    /* 1) Surface drops before data, so a capture is never silently lossy. */
    uint32_t dropped = s_dropped;
    if (dropped != s_reported) {
        uint8_t arg[4] = { (uint8_t)dropped, (uint8_t)(dropped >> 8),
                           (uint8_t)(dropped >> 16), (uint8_t)(dropped >> 24) };
        if (emit(RELAY_STATUS_TYPE, arg, 4) == 0)
            return 0;                         /* sink full; retry next poll */
        s_reported = dropped;
    }

    uint32_t avail = s_head - s_tail;
    if (avail == 0)
        return 0;

    /* 2) Adopt the timestamp of every burst mark at or before s_tail. */
    while (s_mtail != s_mhead) {
        uint32_t mp = s_marks[s_mtail & MARK_MASK].pos;
        if ((int32_t)(mp - s_tail) > 0)
            break;                            /* mark is for a future byte */
        s_cur_ts = s_marks[s_mtail & MARK_MASK].ts;
        s_mtail++;
    }

    /* 3) Gather up to MAX_CHUNK bytes, but do not cross the next burst mark
     *    (so each frame carries exactly one burst's timestamp). */
    uint8_t n = (avail > RELAY_MAX_CHUNK) ? RELAY_MAX_CHUNK : (uint8_t)avail;
    if (s_mtail != s_mhead) {
        uint32_t to_next = s_marks[s_mtail & MARK_MASK].pos - s_tail;
        if (to_next < n)
            n = (uint8_t)to_next;
    }

    uint8_t arg[4 + RELAY_MAX_CHUNK];
    arg[0] = (uint8_t)(s_cur_ts & 0xFFu);
    arg[1] = (uint8_t)((s_cur_ts >> 8) & 0xFFu);
    arg[2] = (uint8_t)((s_cur_ts >> 16) & 0xFFu);
    arg[3] = (uint8_t)((s_cur_ts >> 24) & 0xFFu);
    for (uint8_t i = 0; i < n; i++)
        arg[4 + i] = s_data[(s_tail + i) & RING_MASK];

    size_t e = emit(RELAY_FRAME_TYPE, arg, (uint8_t)(4u + n));
    if (e == 0)
        return 0;                             /* sink full; ring untouched */
    s_tail += n;
    return e;
}

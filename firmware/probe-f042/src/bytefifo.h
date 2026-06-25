/*
 * bytefifo.h — portable single-threaded byte FIFO with all-or-nothing push.
 *
 * Used by the board layer's PC transmitter so board_pc_write() can honor the
 * relay sink contract (accept the WHOLE frame or none of it — never a partial
 * prefix, which would desync the receiver). Host-testable; no hardware deps.
 *
 * P5a usage is single-threaded (push from the main-loop relay_poll, pop from
 * the main-loop board_pc_poll); it is NOT an ISR-safe SPSC ring.
 */
#ifndef BYTEFIFO_H
#define BYTEFIFO_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint8_t *buf;
    size_t   cap;     /* capacity in bytes (buffer is cap, holds cap-1 usable) */
    size_t   head;    /* write index */
    size_t   tail;    /* read index */
} bytefifo_t;

static inline void bytefifo_init(bytefifo_t *f, uint8_t *storage, size_t cap)
{
    f->buf = storage;
    f->cap = cap;
    f->head = 0;
    f->tail = 0;
}

static inline size_t bytefifo_count(const bytefifo_t *f)
{
    return (f->head - f->tail + f->cap) % f->cap;
}

static inline size_t bytefifo_free(const bytefifo_t *f)
{
    return f->cap - 1u - bytefifo_count(f);   /* keep one slot to disambiguate */
}

/* All-or-nothing: push the whole buffer iff it fits; return 1 on success
 * (all len bytes enqueued) or 0 if it does not fit (nothing enqueued). */
static inline int bytefifo_push_all(bytefifo_t *f, const uint8_t *src, size_t len)
{
    if (len > bytefifo_free(f))
        return 0;
    for (size_t i = 0; i < len; i++) {
        f->buf[f->head] = src[i];
        f->head = (f->head + 1u) % f->cap;
    }
    return 1;
}

/* Discard all queued bytes (drop unsent data). Touches only the read index, so
 * it cannot corrupt a concurrent producer's indices — but it is NOT a clean
 * drop if a bytefifo_push_all is in progress: tail=head would discard only the
 * bytes pushed so far and leave the producer to append the rest. A caller that
 * flushes from another context (e.g. an ISR) MUST therefore serialize against
 * the producer — usb_cdc.c masks the USB IRQ around usb_cdc_write so this flush
 * sees a whole frame or none of it. */
static inline void bytefifo_clear(bytefifo_t *f)
{
    f->tail = f->head;
}

/* Pop one byte; return 1 and set *out if available, else 0. */
static inline int bytefifo_pop(bytefifo_t *f, uint8_t *out)
{
    if (f->head == f->tail)
        return 0;
    *out = f->buf[f->tail];
    f->tail = (f->tail + 1u) % f->cap;
    return 1;
}

#endif /* BYTEFIFO_H */

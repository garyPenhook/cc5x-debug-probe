/*
 * relay.h — portable, hardware-independent timestamped relay core (P5a).
 *
 * Data path: target USART RX (ISR) -> relay_target_rx() -> internal ring ->
 *            relay_poll() (main loop) -> framed output -> PC transport.
 *
 * No hardware/register dependency; unit-tested on a host (see tests/).
 *
 * Wire format — a CDL-conformant frame per 01-debug-link-protocol.md §3:
 *
 *   FLAG | TYPE SEQ LEN  ts[4 LE] data[N]  CRC8 | FLAG
 *   0x7E |  \________ covered by CRC8 _______/  | 0x7E
 *
 *   FLAG = 0x7E delimits frames (emitted at both ends).
 *   SLIP byte stuffing inside the frame: 0x7E -> 0x7D 0x5E, 0x7D -> 0x7D 0x5D.
 *   TYPE = RELAY_FRAME_TYPE (data) or RELAY_STATUS_TYPE (drop report). Both are
 *          probe→host envelopes (reserved in 01 §"Probe→Host"; reconciled with
 *          the single-source cdl_proto in P1/P4).
 *   SEQ  = per-frame counter (wraps) so the host can detect dropped frames.
 *   LEN  = number of ARG bytes. Data frame: 4 (timestamp) + N (data); status
 *          frame: 4 (cumulative dropped-byte count, LE).
 *   CRC8 = poly 0x07, init 0x00, over TYPE..last ARG (unstuffed).
 *
 * Timestamping: the probe stamps each emitted data frame with the arrival time
 * of the FIRST byte of its burst — a burst being a run of target bytes with no
 * inter-byte idle gap longer than RELAY_GAP_US. This costs a small mark ring
 * (not one timestamp per byte) and aligns frames to burst boundaries. (True
 * per-CDL-frame timestamping — parsing target FLAGs — is a P5b refinement.)
 *
 * Drop visibility: bytes dropped on a full ring (and marks dropped on a full
 * mark ring) are counted and surfaced as a RELAY_STATUS_TYPE frame, so a
 * capture is never silently lossy (review fix).
 */
#ifndef RELAY_H
#define RELAY_H

#include <stdint.h>
#include <stddef.h>

#define RELAY_FLAG        0x7Eu
#define RELAY_ESC         0x7Du
#define RELAY_ESC_FLAG    0x5Eu   /* 0x7E after ESC */
#define RELAY_ESC_ESC     0x5Du   /* 0x7D after ESC */
#define RELAY_FRAME_TYPE  0xF0u   /* probe timestamped-relay (data) envelope */
#define RELAY_STATUS_TYPE 0xF1u   /* probe status: cumulative dropped count */
#define RELAY_MAX_CHUNK   32u     /* max data bytes per frame */

#ifndef RELAY_GAP_US
#define RELAY_GAP_US      200u    /* inter-byte gap (us) that starts a new burst */
#endif

/* Sink for an ENCODED frame headed to the PC (USART2/VCP in P5a, USB-CDC P5b).
 * Contract: ALL-OR-NOTHING. Return len if the whole buffer was accepted, or 0
 * if it could not be (the relay then retries the same frame on the next poll).
 * Must never accept a partial frame — a truncated frame desyncs the receiver. */
typedef size_t (*relay_pc_write_fn)(const uint8_t *buf, size_t len);

/* Wire up the PC sink. Call once at startup before relay_poll(). */
void relay_init(relay_pc_write_fn pc_write);

/* Producer — call from the target-USART RX path (ISR-safe, single producer).
 * ts_us is a TIM2-derived microsecond timestamp captured at byte arrival. */
void relay_target_rx(uint8_t byte, uint32_t ts_us);

/* Consumer — call repeatedly from the main loop. Emits at most one frame per
 * call. Returns encoded bytes emitted (0 if nothing buffered or sink full). */
size_t relay_poll(void);

/* Producer-side: account for bytes lost outside the relay (e.g. a USART overrun
 * in the RX ISR) so they surface in the next STATUS frame. ISR-safe. */
void relay_note_dropped(uint32_t n);

/* Diagnostics: cumulative bytes dropped (ring/mark overrun + relay_note_dropped). */
uint32_t relay_dropped(void);

#endif /* RELAY_H */

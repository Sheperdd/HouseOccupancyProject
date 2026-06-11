/*
 * evbuf.h -- NVS-backed store-and-forward ring buffer for crossing events.
 *
 * WHY: a crossing detected while WiFi/MQTT is down (router reboot, Pi offline,
 * broker restart) must NOT be lost, or the backend's occupancy count drifts
 * permanently. So every event is first PERSISTED here, then (Phase 3) drained
 * to MQTT; an event is only deleted once the broker has acknowledged it.
 *
 * WHY FLASH (NVS), not RAM: the buffer must survive exactly the events it exists
 * to cover -- reboots, crashes, power loss -- all of which wipe RAM. NVS lives
 * in flash, is wear-levelled, and is power-loss safe.
 *
 * SHAPE: a fixed-capacity FIFO ring faked over NVS key/value storage. Two
 * monotonic counters (head, tail) persist in NVS; each event lives under key
 * "e<seq % EVBUF_CAPACITY>". When full, the OLDEST event is dropped to make room
 * (newest data is the most relevant to current occupancy). count = tail - head.
 *
 * Phase 3 forward loop:  while (evbuf_peek(&r)) { if (mqtt_publish(&r)) evbuf_pop(); else break; }
 *
 * NOT thread-safe: call only from the main detector task (the single writer).
 */
#ifndef STORAGE_EVBUF_H
#define STORAGE_EVBUF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Max events held before the oldest is dropped. 64 * record + NVS overhead fits
 * the default 24 KB NVS partition with large margin. */
#define EVBUF_CAPACITY 64

/* One buffered crossing. Purpose-built (NOT the detector's crossing_event_t) so
 * the on-flash format is decoupled from the detector internals and maps 1:1 to
 * the Phase 3 MQTT JSON payload. Stored as a raw blob; size-checked on read. */
typedef struct
{
    uint32_t seq;        /* monotonic id, assigned on enqueue -- the dedup key for Phase 4 */
    int64_t t_us;        /* esp_timer timestamp at detection (boot-relative until Phase 4 NTP) */
    uint8_t direction;   /* 0 = out, 1 = in */
    float net;           /* net displacement (cells) */
    float confidence;    /* 0..1 */
    uint16_t peak_blob;  /* peak largest-blob size (cells) */
} evbuf_record_t;

/* Open NVS + load (or create) head/tail. Idempotent; call once at startup before
 * any other evbuf call. Initializes the NVS flash partition if needed. */
void evbuf_init(void);

/* Persist one event (seq is assigned internally). Drops the oldest if full.
 * Returns true on success, false on an NVS write error (event not stored). */
bool evbuf_enqueue(const evbuf_record_t *r);

/* Copy the OLDEST pending event into *out WITHOUT removing it. Returns false if
 * the buffer is empty. Pair with evbuf_pop() once the event is acknowledged. */
bool evbuf_peek(evbuf_record_t *out);

/* Remove the oldest event (call only after a successful publish/ack). Returns
 * false if the buffer was already empty. */
bool evbuf_pop(void);

/* Number of events currently buffered (0..EVBUF_CAPACITY). */
size_t evbuf_count(void);

#endif /* STORAGE_EVBUF_H */

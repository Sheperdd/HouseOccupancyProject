/*
 * net.h -- WiFi + MQTT client for the doorway node (Phase 3).
 *
 * Owns the radio side of the node: joins WiFi (with auto-reconnect), keeps an
 * authenticated MQTT session to the Pi 5 broker, and exposes three "step"
 * functions the MAIN TASK calls when it has idle time. Everything that touches
 * the event buffer runs in the main task -- evbuf is single-writer by design,
 * and the MQTT library's internal task only flips flags for us.
 *
 * Topics (per-node, built from node_id at init):
 *   home/doorways/<node_id>/events   QoS1            -- crossing events (JSON)
 *   home/doorways/<node_id>/status   QoS1, retained  -- heartbeat; LWT writes
 *                                                       {"online":false} here
 *   home/doorways/<node_id>/config   QoS1, retained  -- parameters pushed TO us
 *
 * Delivery model: a crossing is ALWAYS persisted to evbuf first (main.c does
 * that), then net_drain_step() forwards oldest-first: peek -> publish QoS1 ->
 * wait for the broker's PUBACK -> pop. The event leaves flash only after the
 * broker acknowledged it, so a crash/outage at any point loses nothing -- at
 * worst the broker sees the same event twice (seq is the Phase 4 dedup key).
 */
#ifndef NET_NET_H
#define NET_NET_H

#include <stdbool.h>
#include <stdint.h> /* int64_t (net_epoch_ms) */

/* Start WiFi + MQTT (non-blocking; connects and reconnects in the background).
 * Call ONCE at startup, AFTER evbuf_init() (NVS must be ready -- WiFi stores
 * radio calibration there). node_id must outlive the program (a literal). */
void net_init(const char *node_id);

/* True while the MQTT session is up (broker reachable + authenticated). */
bool net_connected(void);

/* Forward at most one buffered event per call (peek -> publish -> on PUBACK
 * pop). Cheap no-op when offline, empty, or a publish is still in flight.
 * MAIN TASK ONLY (touches evbuf). */
void net_drain_step(void);

/* Publish the retained status heartbeat if the interval elapsed. Cheap no-op
 * otherwise. MAIN TASK ONLY (reads evbuf_count). */
void net_heartbeat_step(void);

/* Most recent "motion_threshold" from the retained config topic, or `fallback`
 * if no config has arrived. main.c re-reads this before every re-arm, so a
 * remote change takes effect at the next ARMED cycle -- no reflash. */
int net_motion_threshold(int fallback);

/* True once SNTP has set the system clock at least once this boot (Phase 4).
 * The clock keeps running off esp_timer between syncs; lwIP re-polls the NTP
 * server hourly to bound drift. */
bool net_time_synced(void);

/* Wall-clock now as Unix milliseconds, or 0 if the clock was never synced
 * this boot (pre-sync system time starts at the 1970 epoch -- garbage for
 * timestamping, so callers get an explicit "unknown" instead). */
int64_t net_epoch_ms(void);

#endif /* NET_NET_H */

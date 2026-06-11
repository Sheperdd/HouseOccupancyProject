/*
 * node_config_02.h -- install constants for doorway-node-02 (second doorway).
 * Selected by [env:node02] in platformio.ini. See node_config.h.
 *
 * STATUS: bench bring-up values. Before trusting this node's events:
 *   1. IN_AXIS: placeholder -- direction labels are MEANINGLESS until derived
 *      from known-direction calibration walks at the final mount.
 *   2. NODE_HOT_PIXELS: empty -- find this sensor's defect map via an
 *      empty-baseline run with INSTRUMENT_STATUS before/at mounting.
 */
#ifndef NODE_CONFIG_02_H
#define NODE_CONFIG_02_H

/* Small integer identity: selects this node's broker password in
 * net/secrets.h. */
#define NODE_NUM 2

/* Identity: flows into MQTT topics, event payloads, LWT. Broker username ==
 * NODE_ID by convention. */
#define NODE_ID "doorway-node-02"

/* PLACEHOLDER: points "in" along -Y. Re-derive at the final mount. */
#define IN_AXIS_X (0.44f)
#define IN_AXIS_Y (-0.898f)

/* PLACEHOLDER: no known hot pixels yet ({-1} = none; a sentinel index no cell
 * ever matches -- C needs a non-empty initializer). */
#define NODE_HOT_PIXELS {0} /* (0, 0) */

/* PLACEHOLDER wake distance window (mm), copied from node 01. At the final
 * mount: read this doorway's floor distance from the calibrated background
 * (FRAME lines / calib STATHIST dump), then set CAP ~100+ mm under it. MIN =
 * anything closer is too close to be a person under this mount. */
#define NODE_WAKE_DIST_MIN_MM 700  // min distance from the sensor
#define NODE_WAKE_DIST_CAP_MM 2200 // max distance from the sensor, distance from floor

/* Per-zone wake ceiling = calibrated bg - this clearance: a return must be at
 * least this much CLOSER than the zone's floor to count. PLACEHOLDER copied
 * from node 01. This mount is higher, so a person's deviation from the floor
 * is LARGER -- room to raise this for extra storm margin. Raise it if false
 * wakes / strike demotions show up in the logs; a person still clears
 * comfortably as long as it stays well under (floor - person height). */
#define NODE_WAKE_BG_CLEARANCE_MM 400 // clearance from floor to count as a wake, distance from floor

#endif /* NODE_CONFIG_02_H */

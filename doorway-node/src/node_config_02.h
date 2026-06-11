/*
 * node_config_02.h -- install constants for doorway-node-02 (second doorway).
 * Selected by [env:node02] in platformio.ini. See node_config.h.
 *
 * STATUS: calibrated at the current mount (walks fixture:
 * fixtures/node02_walks.*). Re-derive IN_AXIS if the sensor is remounted.
 */
#ifndef NODE_CONFIG_02_H
#define NODE_CONFIG_02_H

/* Small integer identity: selects this node's broker password in
 * net/secrets.h. */
#define NODE_NUM 2

/* Identity: flows into MQTT topics, event payloads, LWT. Broker username ==
 * NODE_ID by convention. */
#define NODE_ID "doorway-node-02"

/* Direction axis: in_axis = -mean(known-out displacements), normalized.
 * Derived from calibration walks at THIS mount (fixtures/node02_walks.*);
 * MUST be re-derived if the sensor is remounted. */
#define IN_AXIS_X (0.44f)
#define IN_AXIS_Y (-0.898f)

/* Per-sensor defect map: cell indices the background model never trusts. */
#define NODE_HOT_PIXELS {0} /* (0, 0) */

/* Wake distance window (mm), mount geometry. MIN = anything closer is too
 * close to be a person under this mount. CAP = absolute "too far" ceiling;
 * must sit clearly UNDER this mount's floor distance or floor noise storms
 * the INT line. This mount is higher than node 01's, hence the wider window. */
#define NODE_WAKE_DIST_MIN_MM 700  // min distance from the sensor
#define NODE_WAKE_DIST_CAP_MM 2200 // max distance from the sensor, distance from floor

/* Per-zone wake ceiling = calibrated bg - this clearance: a return must be at
 * least this much CLOSER than the zone's floor to count. Higher mount means a
 * person's deviation from the floor is LARGER -- room to raise this for extra
 * storm margin if false wakes / strike demotions show up in the logs. */
#define NODE_WAKE_BG_CLEARANCE_MM 400 // clearance from floor to count as a wake, distance from floor

#endif /* NODE_CONFIG_02_H */

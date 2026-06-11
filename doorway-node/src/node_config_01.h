/*
 * node_config_01.h -- install constants for doorway-node-01 (first doorway).
 * Selected by [env:node01] in platformio.ini. See node_config.h.
 */
#ifndef NODE_CONFIG_01_H
#define NODE_CONFIG_01_H

/* Small integer identity: selects this node's broker password in
 * net/secrets.h. */
#define NODE_NUM 1

/* Identity: flows into MQTT topics, event payloads, LWT. Broker username ==
 * NODE_ID by convention. */
#define NODE_ID "doorway-node-01"

/* Direction axis: in_axis = -mean(known-out displacements), normalized.
 * Derived from calibration walks at THIS mount; MUST be re-derived if the
 * sensor is remounted (see Phase 2 entries in the project doc). */
#define IN_AXIS_X (-0.547f)
#define IN_AXIS_Y (-0.837f)

/* Per-sensor defect map: cell indices the background model never trusts.
 * Differs per physical sensor. No hot pixels -> use {-1} (a sentinel index no
 * cell ever matches; C needs a non-empty initializer). */
#define NODE_HOT_PIXELS {1, 5} /* (0,1) bimodal flicker, (0,5) stuck hot */

/* Wake distance window (mm), mount geometry. MIN = anything closer is too
 * close to be a person under this mount (clutter/noise). CAP = absolute "too
 * far" ceiling for the wake window; must sit clearly UNDER this mount's floor
 * distance or floor noise dips into the window and storms the INT line (the
 * per-zone ceiling bg - clearance is the primary guard; this is the fallback
 * cap). Floor at this mount ~2000 mm. */
#define NODE_WAKE_DIST_MIN_MM 400  // min distance from the sensor
#define NODE_WAKE_DIST_CAP_MM 1900 // max distance from the sensor, distance from floor

/* Per-zone wake ceiling = calibrated bg - this clearance: a return must be at
 * least this much CLOSER than the zone's floor to count. A person's
 * head/shoulders deviate 600-900 mm at this mount (Phase 1), so they clear it
 * easily. 400 not 300: bimodal edge cells (zone 13's doorframe flicker,
 * ~320 mm deviation, status 5) slipped a 300 mm clearance and stormed. */
#define NODE_WAKE_BG_CLEARANCE_MM 400 // clearance from floor to count as a wake, distance from floor

#endif /* NODE_CONFIG_01_H */

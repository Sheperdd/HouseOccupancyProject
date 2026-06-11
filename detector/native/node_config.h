/*
 * node_config.h -- PINNED install constants for the HOST regression build.
 *
 * These are doorway-node-01's values at the mount where the fixtures were
 * captured. The goldens (the .txt files in golden/) and gen_golden.py depend
 * on them, so
 * this copy is COMMITTED and must NOT track any device's gitignored
 * src/node_config.h. If these reference values ever change, regenerate the
 * goldens in the same change.
 */
#ifndef NODE_CONFIG_H
#define NODE_CONFIG_H

#define NODE_ID "doorway-node-01"

/* Keep in sync with gen_golden.py IN_AXIS. */
#define IN_AXIS_X (-0.456f)
#define IN_AXIS_Y (-0.890f)

#define NODE_HOT_PIXELS {1, 5} /* (0,1) bimodal flicker, (0,5) stuck hot */

#endif /* NODE_CONFIG_H */

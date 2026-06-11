/*
 * node_config.h -- dispatcher for per-node install constants.
 *
 * Each node has a committed node_config_XX.h next to this file; the PlatformIO
 * env for that node selects it via -D NODE_CONFIG_FILE="node_config_XX.h"
 * (see platformio.ini). Build/flash with `pio run -e nodeXX [-t upload]`.
 * Adding a node = new node_config_XX.h + new [env:nodeXX] -- this file does
 * not change.
 *
 * The host regression build never sees this file: detector/native/ has its own
 * committed node_config.h pinned to the golden (node 01 fixture) values.
 */
#ifndef NODE_CONFIG_H
#define NODE_CONFIG_H

#ifndef NODE_CONFIG_FILE
#error "NODE_CONFIG_FILE undefined -- build via a node env: pio run -e node01 (or node02)"
#endif

#include NODE_CONFIG_FILE

#endif /* NODE_CONFIG_H */

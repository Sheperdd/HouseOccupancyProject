// Single owner of all live backend state.
//
// Design: the server sends a full snapshot on every (re)connect, then only
// deltas with ABSOLUTE values. So the reducer never reconciles — snapshot
// replaces state wholesale, deltas merge keys. Reconnect = new snapshot =
// guaranteed consistency with zero client-side logic.

import { useEffect, useReducer, useRef } from "react";
import type { EventLogEntry, NodeStatus, ServerMsg } from "../types";

const MAX_LOG = 100;
const BACKOFF_MIN_MS = 1000;
const BACKOFF_MAX_MS = 10000;

export interface BackendState {
  connected: boolean;
  occupancy: Record<string, number>;
  houseTotal: number;
  clampCount: number;
  nodes: Record<string, NodeStatus>;
  eventLog: EventLogEntry[]; // newest first
}

const initial: BackendState = {
  connected: false,
  occupancy: {},
  houseTotal: 0,
  clampCount: 0,
  nodes: {},
  eventLog: [],
};

type Action = { type: "ws_open" } | { type: "ws_close" } | { type: "msg"; msg: ServerMsg };

function reducer(state: BackendState, action: Action): BackendState {
  switch (action.type) {
    case "ws_open":
      return { ...state, connected: true };
    case "ws_close":
      return { ...state, connected: false };
    case "msg": {
      const msg = action.msg;
      switch (msg.type) {
        case "snapshot":
          return {
            connected: true,
            occupancy: msg.occupancy,
            houseTotal: msg.house_total,
            clampCount: msg.clamp_count,
            nodes: msg.nodes,
            eventLog: [...msg.recent_events].reverse(), // server sends oldest-first
          };
        case "occupancy_delta":
          return {
            ...state,
            occupancy: { ...state.occupancy, ...msg.changes },
            houseTotal: msg.house_total,
            clampCount: msg.clamp_count,
          };
        case "node_status": {
          const { type: _type, ...status } = msg;
          return { ...state, nodes: { ...state.nodes, [msg.node_id]: status } };
        }
        case "event_log_append":
          return { ...state, eventLog: [msg.event, ...state.eventLog].slice(0, MAX_LOG) };
      }
    }
  }
}

export function useBackend(): BackendState {
  const [state, dispatch] = useReducer(reducer, initial);
  const backoffRef = useRef(BACKOFF_MIN_MS);

  useEffect(() => {
    let ws: WebSocket | null = null;
    let timer: number | undefined;
    let closed = false;

    const connect = () => {
      const proto = location.protocol === "https:" ? "wss" : "ws";
      ws = new WebSocket(`${proto}://${location.host}/ws`);
      ws.onopen = () => {
        backoffRef.current = BACKOFF_MIN_MS;
        dispatch({ type: "ws_open" });
      };
      ws.onmessage = (e) => dispatch({ type: "msg", msg: JSON.parse(e.data) as ServerMsg });
      ws.onclose = () => {
        if (closed) return;
        dispatch({ type: "ws_close" });
        timer = window.setTimeout(connect, backoffRef.current);
        backoffRef.current = Math.min(backoffRef.current * 2, BACKOFF_MAX_MS);
      };
    };
    connect();

    return () => {
      closed = true;
      window.clearTimeout(timer);
      ws?.close();
    };
  }, []);

  return state;
}

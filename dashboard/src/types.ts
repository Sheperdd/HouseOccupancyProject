// WebSocket/REST protocol types — mirror backend/app/pipeline.py shapes.
// If these drift from the backend, the compiler catches it here, not at 2am.

export interface NodeStatus {
  node_id: string;
  online: boolean;
  fw: string | null;
  uptime_s: number | null;
  heap_free: number | null;
  pending: number | null; // events buffered in node NVS awaiting drain
  rssi: number | null;
  time_synced: boolean | null;
  sync_age_s: number | null;
  last_seen_ms: number;
}

export interface EventLogEntry {
  node_id: string;
  seq: number;
  direction: "in" | "out";
  event_ts_ms: number;
  arrival_ts_ms: number;
  clock_synced: boolean;
  net: number | null;
  confidence: number | null;
  peak_blob: number | null;
  duplicate: boolean;
  unmapped: boolean;
  doorway_id: string | null;
}

export interface CrossingCause {
  kind: "crossing";
  node_id: string;
  seq: number;
  doorway_id: string;
  direction: "in" | "out";
  event_ts_ms: number;
}

export interface ResetCause {
  kind: "manual_reset";
}

export interface SnapshotMsg {
  type: "snapshot";
  occupancy: Record<string, number>;
  house_total: number;
  clamp_count: number;
  nodes: Record<string, NodeStatus>;
  recent_events: EventLogEntry[];
}

export interface OccupancyDeltaMsg {
  type: "occupancy_delta";
  changes: Record<string, number>; // ABSOLUTE new counts, not diffs
  house_total: number;
  clamp_count: number;
  cause: CrossingCause | ResetCause;
}

export type NodeStatusMsg = { type: "node_status" } & NodeStatus;

export interface EventLogAppendMsg {
  type: "event_log_append";
  event: EventLogEntry;
}

export type ServerMsg = SnapshotMsg | OccupancyDeltaMsg | NodeStatusMsg | EventLogAppendMsg;

// ----- floorplan.json shapes (GET /api/floorplan) -----

export interface PlanRoom {
  id: string;
  name: string;
  zone?: string; // counting zone; defaults to the room's own id
  x: number;
  y: number;
  w: number;
  h: number;
}

export interface PlanFloor {
  id: string;
  name: string;
  rooms: PlanRoom[];
}

export interface PlanDoorway {
  id: string;
  rooms: [string, string];
  node_id: string | null; // null = unsensored
  in_room: string;
  floor: string;
  x: number;
  y: number;
}

export interface FloorplanData {
  floors: PlanFloor[];
  doorways: PlanDoorway[];
}

import type { NodeStatus } from "../types";

function age(lastSeenMs: number): string {
  const s = Math.max(0, Math.round((Date.now() - lastSeenMs) / 1000));
  if (s < 90) return `${s}s`;
  if (s < 5400) return `${Math.round(s / 60)}m`;
  return `${Math.round(s / 3600)}h`;
}

export default function NodeStatusBar({ nodes }: { nodes: Record<string, NodeStatus> }) {
  const list = Object.values(nodes).sort((a, b) => a.node_id.localeCompare(b.node_id));
  return (
    <div className="node-bar">
      {list.length === 0 && <span className="chip">no nodes seen yet</span>}
      {list.map((n) => (
        <span key={n.node_id} className={`chip ${n.online ? "chip-online" : "chip-offline"}`}>
          <span className="dot" />
          {n.node_id.replace("doorway-node-", "node ")}
          {n.online ? (
            <>
              {" "}· {n.rssi ?? "?"} dBm · pending {n.pending ?? "?"}
              {n.time_synced === false && " · ⚠ unsynced"}
            </>
          ) : (
            " · offline"
          )}
          {" "}· seen {age(n.last_seen_ms)} ago
        </span>
      ))}
    </div>
  );
}

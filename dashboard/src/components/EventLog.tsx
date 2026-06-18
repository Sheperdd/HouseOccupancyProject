import type { EventLogEntry } from "../types";

export default function EventLog({ events }: { events: EventLogEntry[] }) {
  return (
    <section className="event-log">
      <h2>Events</h2>
      {events.length === 0 && <p className="muted">No crossings yet.</p>}
      <ol>
        {events.map((e) => (
          <li
            key={`${e.node_id}-${e.seq}-${e.arrival_ts_ms}`}
            className={[e.duplicate && "dup", e.unmapped && "unmapped"].filter(Boolean).join(" ")}
          >
            <span className="muted">{new Date(e.event_ts_ms).toLocaleTimeString()}</span>{" "}
            <strong>{e.direction}</strong> @{e.doorway_id ?? `${e.node_id} (unmapped)`}
            {e.confidence != null && <span className="muted"> conf {e.confidence.toFixed(2)}</span>}
            {e.duplicate && <span className="muted"> (duplicate)</span>}
            {!e.clock_synced && <span className="muted"> (arrival-stamped)</span>}
          </li>
        ))}
      </ol>
    </section>
  );
}

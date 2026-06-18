import { useEffect, useState } from "react";
import { useBackend } from "./hooks/useBackend";
import type { FloorplanData } from "./types";
import FloorPlan from "./components/FloorPlan";
import NodeStatusBar from "./components/NodeStatusBar";
import EventLog from "./components/EventLog";
import "./app.css";

export default function App() {
  const { connected, occupancy, houseTotal, clampCount, nodes, eventLog } = useBackend();
  const [plan, setPlan] = useState<FloorplanData | null>(null);

  // Floorplan is static config — fetched once, not part of the WS stream.
  useEffect(() => {
    fetch("/api/floorplan")
      .then((r) => r.json())
      .then(setPlan)
      .catch(() => setPlan(null));
  }, []);

  return (
    <main className="layout">
      <header>
        <h1>House Occupancy</h1>
        <span className="total">
          {houseTotal} {houseTotal === 1 ? "person" : "people"} home
          {clampCount > 0 && <span className="clamps" title="count drift detected"> · {clampCount} clamps</span>}
        </span>
        {!connected && <span className="banner">disconnected — reconnecting…</span>}
      </header>

      <NodeStatusBar nodes={nodes} />

      <div className="content">
        {plan ? (
          <FloorPlan plan={plan} occupancy={occupancy} nodes={nodes} />
        ) : (
          <p className="muted">Loading floor plan…</p>
        )}
        <EventLog events={eventLog} />
      </div>
    </main>
  );
}

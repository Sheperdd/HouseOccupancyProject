// The digital twin view: schematic SVG per floor, rooms tinted by their
// zone's live count, doorway markers colored by sensor state. Geometry
// comes straight from floorplan.json — one viewBox per floor, so the SVG
// scales to any screen for free.

import { useMemo, useState } from "react";
import type { FloorplanData, NodeStatus } from "../types";
import RoomCell from "./RoomCell";

interface Props {
  plan: FloorplanData;
  occupancy: Record<string, number>;
  nodes: Record<string, NodeStatus>;
}

const PAD = 0.6;

export default function FloorPlan({ plan, occupancy, nodes }: Props) {
  const [activeId, setActiveId] = useState(plan.floors[0]?.id);
  const floor = plan.floors.find((f) => f.id === activeId) ?? plan.floors[0];

  // zone -> number of rooms; a "solo" zone's count is room-precise,
  // a shared zone's count means "somewhere in these rooms".
  const zoneSizes = useMemo(() => {
    const sizes: Record<string, number> = {};
    for (const f of plan.floors)
      for (const r of f.rooms) {
        const z = r.zone ?? r.id;
        sizes[z] = (sizes[z] ?? 0) + 1;
      }
    return sizes;
  }, [plan]);

  if (!floor) return null;

  const xs = floor.rooms.map((r) => r.x);
  const ys = floor.rooms.map((r) => r.y);
  const x2 = floor.rooms.map((r) => r.x + r.w);
  const y2 = floor.rooms.map((r) => r.y + r.h);
  const minX = Math.min(...xs) - PAD;
  const minY = Math.min(...ys) - PAD;
  const width = Math.max(...x2) - minX + PAD;
  const height = Math.max(...y2) - minY + PAD;

  // Zones spanning multiple rooms get their count shown once in a banner
  // (showing it inside every room would read as N people per room).
  const sharedZoneCounts = Object.entries(occupancy).filter(([z]) => (zoneSizes[z] ?? 0) > 1);

  return (
    <section className="floorplan">
      <div className="floor-tabs" role="tablist">
        {plan.floors.map((f) => (
          <button
            key={f.id}
            role="tab"
            aria-selected={f.id === floor.id}
            className={f.id === floor.id ? "tab active" : "tab"}
            onClick={() => setActiveId(f.id)}
          >
            {f.name}
          </button>
        ))}
      </div>

      {sharedZoneCounts.length > 0 && (
        <p className="zone-banner">
          {sharedZoneCounts.map(([z, n]) => `${z} zone: ${n} ${n === 1 ? "person" : "people"}`).join(" · ")}
        </p>
      )}

      <svg viewBox={`${minX} ${minY} ${width} ${height}`} className="plan-svg">
        {floor.rooms.map((room) => {
          const zone = room.zone ?? room.id;
          return (
            <RoomCell
              key={room.id}
              room={room}
              count={occupancy[zone] ?? 0}
              soloZone={(zoneSizes[zone] ?? 1) === 1}
            />
          );
        })}
        {plan.doorways
          .filter((dw) => dw.floor === floor.id)
          .map((dw) => {
            const sensored = dw.node_id !== null;
            const online = sensored && (nodes[dw.node_id!]?.online ?? false);
            const cls = !sensored ? "dw-unsensored" : online ? "dw-online" : "dw-offline";
            return (
              <g key={dw.id} className={`doorway ${cls}`}>
                <circle cx={dw.x} cy={dw.y} r={0.28} />
                {sensored && (
                  <title>{`${dw.id} — ${dw.node_id} (${online ? "online" : "OFFLINE"})`}</title>
                )}
              </g>
            );
          })}
      </svg>
    </section>
  );
}

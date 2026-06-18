import type { PlanRoom } from "../types";

interface Props {
  room: PlanRoom;
  count: number; // the room's ZONE count
  soloZone: boolean; // true when this room is its zone's only room -> count is room-precise
}

// SVG units are floorplan units (~rooms are 3-4 wide), so font sizes are <1.
export default function RoomCell({ room, count, soloZone }: Props) {
  const occClass = count === 0 ? "occ-0" : count === 1 ? "occ-1" : "occ-many";
  return (
    <g className={`room ${occClass}`}>
      <rect x={room.x} y={room.y} width={room.w} height={room.h} rx={0.15} />
      <text x={room.x + room.w / 2} y={room.y + 0.65} className="room-name">
        {room.name}
      </text>
      {soloZone && count > 0 && (
        <text x={room.x + room.w / 2} y={room.y + room.h / 2 + 0.45} className="room-count">
          {count}
        </text>
      )}
    </g>
  );
}

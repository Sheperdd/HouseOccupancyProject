"""WebSocket hub: snapshot on connect, fanout after.

Server -> client only. Reset and config changes go over REST so the WS
protocol stays trivial: a client that can parse four message types is done.
"""

from __future__ import annotations

import json
import logging

from fastapi import WebSocket

log = logging.getLogger(__name__)


class Hub:
    def __init__(self) -> None:
        self._connections: set[WebSocket] = set()

    async def register(self, ws: WebSocket, snapshot: dict) -> None:
        await ws.send_text(json.dumps(snapshot))
        self._connections.add(ws)

    def unregister(self, ws: WebSocket) -> None:
        self._connections.discard(ws)

    async def broadcast(self, message: dict) -> None:
        text = json.dumps(message)
        for ws in list(self._connections):
            try:
                await ws.send_text(text)
            except Exception:
                # Dead socket; the /ws endpoint's receive loop will clean up
                # too, but don't let one stale client block the others.
                self._connections.discard(ws)

    @property
    def client_count(self) -> int:
        return len(self._connections)

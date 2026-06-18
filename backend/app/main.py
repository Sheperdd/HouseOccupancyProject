"""FastAPI app: wires config, floorplan, pipeline, MQTT task, WebSocket.

Run from backend/ with:  python -m uvicorn app.main:app --port 8000
(--reload for development)
"""

from __future__ import annotations

import asyncio
import contextlib
import logging
from pathlib import Path

from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.staticfiles import StaticFiles

from .config import BACKEND_DIR, load_config
from .floorplan import load_floorplan
from .mqtt_ingest import run_ingest
from .pipeline import Pipeline
from .ws import Hub

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
log = logging.getLogger(__name__)

DASHBOARD_DIST = BACKEND_DIR.parent / "dashboard" / "dist"


def create_app() -> FastAPI:
    config = load_config()
    floorplan = load_floorplan(config.storage.floorplan_path)
    hub = Hub()
    pipeline = Pipeline(floorplan, hub)

    @contextlib.asynccontextmanager
    async def lifespan(app: FastAPI):
        # The MQTT consumer lives exactly as long as the app: started here,
        # cancelled on shutdown. It owns its own connection retry loop.
        task = asyncio.create_task(run_ingest(config.mqtt, pipeline), name="mqtt-ingest")
        yield
        task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await task

    app = FastAPI(title="House Occupancy Backend", lifespan=lifespan)
    app.state.config = config
    app.state.pipeline = pipeline
    app.state.hub = hub

    @app.get("/api/floorplan")
    def get_floorplan() -> dict:
        return floorplan.raw

    @app.websocket("/ws")
    async def ws_endpoint(websocket: WebSocket) -> None:
        await websocket.accept()
        # Snapshot-on-connect: the client gets full state up front, then only
        # deltas — a reconnecting client never needs reconciliation logic.
        await hub.register(websocket, pipeline.snapshot())
        try:
            while True:
                await websocket.receive_text()  # ignore client chatter; raises on disconnect
        except WebSocketDisconnect:
            pass
        finally:
            hub.unregister(websocket)

    # Serve the built dashboard when it exists (M3+; in dev Vite serves it).
    if DASHBOARD_DIST.is_dir():
        app.mount("/", StaticFiles(directory=DASHBOARD_DIST, html=True), name="dashboard")

    return app


app = create_app()

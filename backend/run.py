"""Backend entry point — use this instead of `python -m uvicorn`:

    .venv\\Scripts\\python run.py

WHY THIS EXISTS: paho-mqtt (under aiomqtt) registers its socket with
loop.add_reader(), which Windows' default Proactor event loop does not
implement — `python -m uvicorn app.main:app` dies with NotImplementedError
on the first MQTT connect. Uvicorn's CLI has no selector-loop option, so we
run the server programmatically on a SelectorEventLoop. Harmless on the Pi
(Linux default loop already supports add_reader).

Tradeoff: no `--reload` supervisor here — restart manually after edits.
"""

import asyncio
import sys

import uvicorn

from app.config import load_config


def main() -> None:
    cfg = load_config()
    server = uvicorn.Server(
        uvicorn.Config("app.main:app", host=cfg.server.host, port=cfg.server.port)
    )
    if sys.platform == "win32":
        with asyncio.Runner(loop_factory=asyncio.SelectorEventLoop) as runner:
            runner.run(server.serve())
    else:
        asyncio.run(server.serve())


if __name__ == "__main__":
    main()

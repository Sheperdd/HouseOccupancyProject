"""Load and validate config.toml (gitignored; template: config.example.toml)."""

from __future__ import annotations

import tomllib
from dataclasses import dataclass
from pathlib import Path

BACKEND_DIR = Path(__file__).resolve().parent.parent


@dataclass(frozen=True)
class MqttConfig:
    host: str
    port: int
    username: str
    password: str
    client_id: str


@dataclass(frozen=True)
class ServerConfig:
    host: str
    port: int


@dataclass(frozen=True)
class StorageConfig:
    db_path: Path
    floorplan_path: Path


@dataclass(frozen=True)
class Config:
    mqtt: MqttConfig
    server: ServerConfig
    storage: StorageConfig


def load_config(path: str | Path | None = None) -> Config:
    path = Path(path) if path else BACKEND_DIR / "config.toml"
    if not path.exists():
        raise SystemExit(
            f"Config not found: {path}\n"
            f"Copy {BACKEND_DIR / 'config.example.toml'} to config.toml "
            "and fill in the broker credentials."
        )
    data = tomllib.loads(path.read_text(encoding="utf-8"))
    try:
        mqtt = data["mqtt"]
        server = data["server"]
        storage = data["storage"]
        return Config(
            mqtt=MqttConfig(
                host=mqtt["host"],
                port=int(mqtt.get("port", 1883)),
                username=mqtt["username"],
                password=mqtt["password"],
                client_id=mqtt.get("client_id", "backend"),
            ),
            server=ServerConfig(
                host=server.get("host", "0.0.0.0"),
                port=int(server.get("port", 8000)),
            ),
            storage=StorageConfig(
                # Relative paths resolve against backend/, not the CWD, so
                # `uvicorn` works the same from any directory.
                db_path=BACKEND_DIR / storage.get("db_path", "occupancy.db"),
                floorplan_path=BACKEND_DIR / storage.get("floorplan_path", "floorplan.json"),
            ),
        )
    except KeyError as exc:
        raise SystemExit(f"Config missing required key {exc} — see config.example.toml") from exc

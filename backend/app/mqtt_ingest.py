"""MQTT ingest: one asyncio task consuming events + status from the broker.

Persistent-session design (the backend half of store-and-forward):
- stable client identifier + clean_session=False means mosquitto remembers
  this client's QoS1 subscriptions and QUEUES events published while the
  backend is down; they are delivered on reconnect. The node side covers
  broker outages (its NVS buffer); this covers backend outages.
- aiomqtt 2.x has no built-in reconnect: the outer while/except MqttError
  loop IS the reconnect strategy (covers initial connect failure, broker
  restarts, network drops). Resubscribing after reconnect is idempotent —
  the persistent session usually still has the subscriptions, but a broker
  that lost state (restart without persistence) gets them back this way.
"""

from __future__ import annotations

import asyncio
import logging

import aiomqtt

from .config import MqttConfig
from .pipeline import Pipeline, now_ms

log = logging.getLogger(__name__)

EVENTS_TOPIC = "home/doorways/+/events"
STATUS_TOPIC = "home/doorways/+/status"
RETRY_DELAY_S = 3


def make_client(cfg: MqttConfig) -> aiomqtt.Client:
    return aiomqtt.Client(
        hostname=cfg.host,
        port=cfg.port,
        username=cfg.username,
        password=cfg.password,
        identifier=cfg.client_id,
        clean_session=False,  # persistent session: broker queues QoS1 while we're down
        keepalive=30,
    )


async def run_ingest(cfg: MqttConfig, pipeline: Pipeline) -> None:
    while True:
        try:
            async with make_client(cfg) as client:
                log.info("connected to broker %s:%d as %s", cfg.host, cfg.port, cfg.client_id)
                await client.subscribe(EVENTS_TOPIC, qos=1)
                await client.subscribe(STATUS_TOPIC, qos=1)
                async for message in client.messages:
                    arrival_ms = now_ms()
                    payload = message.payload
                    if isinstance(payload, (bytearray, memoryview)):
                        payload = bytes(payload)
                    if message.topic.matches(EVENTS_TOPIC):
                        await pipeline.handle_event(payload, arrival_ms)
                    elif message.topic.matches(STATUS_TOPIC):
                        await pipeline.handle_status(payload, arrival_ms)
        except asyncio.CancelledError:
            raise  # normal shutdown path (lifespan cancels us)
        except aiomqtt.MqttError as exc:
            log.warning("MQTT connection lost (%s) — reconnecting in %ds", exc, RETRY_DELAY_S)
            await asyncio.sleep(RETRY_DELAY_S)

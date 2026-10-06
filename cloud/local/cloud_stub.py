"""Local stand-in for the AWS backend.

Does locally what the AWS stack does in the cloud, so the whole system can run
on one PC without an AWS account:

    AWS:    IoT Core -> IoT Rule -> DynamoDB -> Lambda + API Gateway (GET /events)
    local:  Mosquitto -> this subscriber -> SQLite -> this HTTP server (GET /api/events)

Both return the same JSON, so the dashboard works against either. This server
also serves the dashboard itself at http://127.0.0.1:8000/.

    python cloud/local/cloud_stub.py
"""

from __future__ import annotations

import argparse
import json
import sqlite3
import threading
import time
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

import paho.mqtt.client as mqtt

ROOT = Path(__file__).resolve().parents[2]
TOPIC = "edgevision/+/events"


def log(msg: str) -> None:
    print(f"[cloud] {msg}", flush=True)


class EventStore:
    """Plays DynamoDB: one row per event, keyed by device and arrival time."""

    def __init__(self, path: Path) -> None:
        self.db = sqlite3.connect(path, check_same_thread=False)
        self.lock = threading.Lock()
        self.db.execute(
            "CREATE TABLE IF NOT EXISTS events (device TEXT, received_at INTEGER, payload TEXT, "
            "PRIMARY KEY (device, received_at))"
        )
        self.db.commit()

    def put(self, device: str, payload: dict) -> None:
        received_at = int(time.time() * 1000)
        with self.lock:
            # Same-millisecond arrivals would collide on the key: nudge forward.
            while self.db.execute("SELECT 1 FROM events WHERE device=? AND received_at=?", (device, received_at)).fetchone():
                received_at += 1
            self.db.execute("INSERT INTO events VALUES (?, ?, ?)", (device, received_at, json.dumps(payload)))
            self.db.commit()

    def latest(self, device: str | None, limit: int) -> list[dict]:
        sql = "SELECT device, received_at, payload FROM events"
        args: tuple = ()
        if device:
            sql += " WHERE device=?"
            args = (device,)
        sql += " ORDER BY received_at DESC LIMIT ?"
        with self.lock:
            rows = self.db.execute(sql, args + (limit,)).fetchall()
        return [{**json.loads(p), "device": d, "received_at": r} for d, r, p in rows]


def make_handler(store: EventStore, dashboard_dir: Path):
    class Handler(SimpleHTTPRequestHandler):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, directory=str(dashboard_dir), **kwargs)

        def log_message(self, fmt, *args) -> None:  # quiet
            pass

        def do_GET(self) -> None:
            url = urlparse(self.path)
            if url.path in ("/api/events", "/events"):
                q = parse_qs(url.query)
                limit = max(1, min(200, int(q.get("limit", ["50"])[0])))
                body = json.dumps({"events": store.latest(q.get("device", [None])[0], limit)}).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Access-Control-Allow-Origin", "*")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            else:
                super().do_GET()

    return Handler


def main() -> None:
    p = argparse.ArgumentParser(description="Local stand-in for the EdgeVision AWS backend")
    p.add_argument("--broker", default="localhost")
    p.add_argument("--port", type=int, default=1883)
    p.add_argument("--http-port", type=int, default=8000)
    p.add_argument("--db", default=str(Path(__file__).with_name("events.db")))
    args = p.parse_args()

    store = EventStore(Path(args.db))

    def on_connect(client, userdata, flags, reason_code, properties):
        if not reason_code.is_failure:
            client.subscribe(TOPIC, qos=1)
            log(f"subscribed to {TOPIC} on {args.broker}:{args.port}")

    def on_message(client, userdata, msg):
        try:
            payload = json.loads(msg.payload)
        except json.JSONDecodeError:
            log(f"ignored non-JSON message on {msg.topic}")
            return
        device = msg.topic.split("/")[1]  # Same as topic(2) in the AWS IoT rule.
        store.put(device, payload)
        log(f"stored {payload.get('event')} from {device} ({len(msg.payload)} bytes)")

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="edgevision-cloud-stub")
    client.on_connect = on_connect
    client.on_message = on_message
    client.reconnect_delay_set(min_delay=1, max_delay=10)
    client.connect_async(args.broker, args.port)
    client.loop_start()

    server = ThreadingHTTPServer(("127.0.0.1", args.http_port), make_handler(store, ROOT / "dashboard"))
    log(f"events API on http://127.0.0.1:{args.http_port}/api/events, dashboard at http://127.0.0.1:{args.http_port}/")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()

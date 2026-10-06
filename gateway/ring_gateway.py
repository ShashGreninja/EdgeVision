"""Ring webhook gateway: Ring motion events -> the camera's MOTION interrupt.

Ring delivers events as HTTPS POSTs (webhook v1.1, see docs/ring-notes.md).
This gateway:
  1. verifies the X-Signature header (HMAC-SHA256 of the raw body, hex) with
     the app's HMAC signing key, before parsing anything;
  2. drops duplicate deliveries by meta.request_id (idempotency);
  3. answers 200 immediately (Ring requires a reply within 5 s);
  4. for motion_detected events, sends "MOTION ring <sub_type> <device>"
     over UDP to the firmware, which raises its motion pin.

The signing key comes from the RING_HMAC_KEY environment variable and is
never written to disk by this code. Ring needs a public HTTPS URL; for local
development, expose this server with a tunnel (see README).

    set RING_HMAC_KEY=<key from the Ring developer console>
    python gateway/ring_gateway.py --port 8787
"""

from __future__ import annotations

import argparse
import hashlib
import hmac
import json
import os
import socket
import threading
from collections import OrderedDict
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

WEBHOOK_PATH = "/webhooks/ring"
MAX_BODY = 64 * 1024


def log(msg: str) -> None:
    print(f"[gateway] {msg}", flush=True)


def verify_signature(key: str, raw_body: bytes, header: str | None) -> bool:
    """Check Ring's X-Signature header ("sha256=<hex>") against the raw body."""
    if not key or not header:
        return False
    expected = hmac.new(key.encode(), raw_body, hashlib.sha256).hexdigest()
    received = header.strip().removeprefix("sha256=")
    return hmac.compare_digest(expected, received)


def sign(key: str, raw_body: bytes) -> str:
    """The X-Signature value Ring would send for this body."""
    return "sha256=" + hmac.new(key.encode(), raw_body, hashlib.sha256).hexdigest()


class Dedup:
    """Remembers recent request ids so retried deliveries are processed once."""

    def __init__(self, capacity: int = 1000) -> None:
        self.capacity = capacity
        self.seen: OrderedDict[str, None] = OrderedDict()
        self.lock = threading.Lock()

    def first_time(self, request_id: str) -> bool:
        with self.lock:
            if request_id in self.seen:
                self.seen.move_to_end(request_id)
                return False
            self.seen[request_id] = None
            if len(self.seen) > self.capacity:
                self.seen.popitem(last=False)
            return True


def motion_command(payload: dict) -> str | None:
    """The firmware command for a webhook payload, or None if it is not motion."""
    data = payload.get("data", {})
    if data.get("type") != "motion_detected":
        return None
    attrs = data.get("attributes", {})
    sub_type = str(attrs.get("sub_type", "motion")).replace(" ", "_")
    device = str(attrs.get("source", "unknown")).replace(" ", "_")
    return f"MOTION ring {sub_type} {device}"


class Gateway:
    def __init__(self, key: str, firmware_port: int, device_filter: str | None) -> None:
        self.key = key
        self.firmware = ("127.0.0.1", firmware_port)
        self.device_filter = device_filter
        self.dedup = Dedup()
        self.counts = {"accepted": 0, "duplicate": 0, "bad_signature": 0, "ignored": 0}

    def handle(self, raw: bytes, signature: str | None) -> tuple[int, dict]:
        if not verify_signature(self.key, raw, signature):
            self.counts["bad_signature"] += 1
            log("rejected: bad or missing X-Signature")
            return 401, {"error": "invalid signature"}

        try:
            payload = json.loads(raw)
        except json.JSONDecodeError:
            return 400, {"error": "invalid JSON"}

        request_id = str(payload.get("meta", {}).get("request_id", ""))
        if request_id and not self.dedup.first_time(request_id):
            self.counts["duplicate"] += 1
            log(f"duplicate delivery {request_id} ignored")
            return 200, {"status": "duplicate"}

        command = motion_command(payload)
        source = payload.get("data", {}).get("attributes", {}).get("source")
        if command is None or (self.device_filter and source != self.device_filter):
            self.counts["ignored"] += 1
            log(f"ignored {payload.get('data', {}).get('type', '?')} event")
            return 200, {"status": "ignored"}

        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.sendto(command.encode(), self.firmware)
        self.counts["accepted"] += 1
        log(f"motion from Ring ({command.split()[2]}) -> camera motion pin, request {request_id}")
        return 200, {"status": "processed"}


def make_handler(gateway: Gateway):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt, *args) -> None:  # quiet
            pass

        def _reply(self, code: int, body: dict) -> None:
            data = json.dumps(body).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self) -> None:
            if self.path == "/healthz":
                self._reply(200, {"ok": True, **gateway.counts})
            else:
                self._reply(404, {"error": "not found"})

        def do_POST(self) -> None:
            if self.path != WEBHOOK_PATH:
                self._reply(404, {"error": "not found"})
                return
            length = int(self.headers.get("Content-Length", "0"))
            if length <= 0 or length > MAX_BODY:
                self._reply(413 if length > MAX_BODY else 400, {"error": "bad body length"})
                return
            raw = self.rfile.read(length)  # Raw bytes: the signature covers exactly these.
            code, body = gateway.handle(raw, self.headers.get("X-Signature"))
            self._reply(code, body)

    return Handler


def main() -> None:
    p = argparse.ArgumentParser(description="Ring webhook -> EdgeVision motion gateway")
    p.add_argument("--port", type=int, default=8787)
    p.add_argument("--firmware-udp", type=int, default=5055)
    p.add_argument("--device", help="only forward events from this Ring device id")
    args = p.parse_args()

    key = os.environ.get("RING_HMAC_KEY", "")
    if not key:
        raise SystemExit("RING_HMAC_KEY is not set: refusing to accept unsigned webhooks")

    gateway = Gateway(key, args.firmware_udp, args.device)
    server = ThreadingHTTPServer(("127.0.0.1", args.port), make_handler(gateway))
    log(f"listening on http://127.0.0.1:{args.port}{WEBHOOK_PATH}, forwarding motion to UDP {args.firmware_udp}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()

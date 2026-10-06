"""EdgeVision Wi-Fi module emulator.

Plays the role of a Wi-Fi module with an on-board TCP/TLS/MQTT stack (the
kind driven by AT commands). The firmware never handles TLS itself: it sends
short text commands over its "UART" (a localhost TCP connection) and this
module does the networking.

Firmware link (line protocol, see firmware/services/network.h):
    AT+PUB=<id>,<topic>,<json>   -> publish QoS 1, reply +PUBACK:<id>,OK|ERR
    AT+TEL=<json>                -> latest telemetry, streamed to the dashboard
    AT+NETDOWN=<seconds>         -> simulate a network outage
    module sends +LINK:UP / +LINK:DOWN when cloud reachability changes

HTTP API for the dashboard (CORS open, localhost only):
    GET  /api/telemetry/stream   Server-Sent Events, one JSON per second
    GET  /api/status             module state
    POST /api/outage?seconds=N   simulate a network outage
    POST /api/fault/<name>       forward a fault-injection command to the firmware
    POST /api/motion             raise the motion pin (as a Ring event would)

Brokers:
    local:  python netmodule/wifi_module.py                       (Mosquitto on localhost:1883)
    AWS:    python netmodule/wifi_module.py --broker <iot-endpoint> --port 8883 \
                --ca AmazonRootCA1.pem --cert device.pem.crt --key private.pem.key
"""

from __future__ import annotations

import argparse
import json
import queue
import socket
import socketserver
import ssl
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

import paho.mqtt.client as mqtt

FAULTS = {"camera_disconnect", "frame_corrupt", "mem_pressure", "inference_hang", "network_down", "cpu_overload", "selftest"}


def log(msg: str) -> None:
    print(f"[module] {msg}", flush=True)


class WifiModule:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.lock = threading.Lock()
        self.conn: socket.socket | None = None
        self.mqtt_connected = False
        self.outage_until = 0.0
        self.reported: bool | None = None
        self.pending: dict[int, int] = {}  # MQTT mid -> firmware publish id
        self.published = 0
        self.bytes_up = 0
        self.telemetry: dict | None = None
        self.subscribers: list[queue.Queue] = []

        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=args.client_id)
        self.client.on_connect = self._on_connect
        self.client.on_disconnect = self._on_disconnect
        self.client.on_publish = self._on_publish
        self.client.reconnect_delay_set(min_delay=1, max_delay=10)
        if args.cert:
            self.client.tls_set(ca_certs=args.ca, certfile=args.cert, keyfile=args.key, tls_version=ssl.PROTOCOL_TLS_CLIENT)

    # ---- cloud side -------------------------------------------------------------

    def start_mqtt(self) -> None:
        log(f"MQTT broker {self.args.broker}:{self.args.port}{' (TLS)' if self.args.cert else ''}, client id {self.args.client_id}")
        self.client.connect_async(self.args.broker, self.args.port, keepalive=30)
        self.client.loop_start()

    def _on_connect(self, client, userdata, flags, reason_code, properties) -> None:
        if reason_code.is_failure:
            log(f"MQTT connect refused: {reason_code}")
            return
        log("MQTT connected")
        self.mqtt_connected = True
        self.report_link()

    def _on_disconnect(self, client, userdata, flags, reason_code, properties) -> None:
        if self.mqtt_connected:
            log(f"MQTT disconnected: {reason_code}")
        self.mqtt_connected = False
        self.report_link()

    def _on_publish(self, client, userdata, mid, reason_code, properties) -> None:
        with self.lock:
            fw_id = self.pending.pop(mid, None)
        if fw_id is not None:
            self.send(f"+PUBACK:{fw_id},OK")

    def online(self) -> bool:
        return self.mqtt_connected and time.monotonic() >= self.outage_until

    def start_outage(self, seconds: float) -> None:
        self.outage_until = time.monotonic() + seconds
        log(f"simulated network outage for {seconds:g} s")
        self.report_link()

    def report_link(self, force: bool = False) -> None:
        state = self.online()
        if force or state != self.reported:
            self.reported = state
            self.send("+LINK:UP" if state else "+LINK:DOWN")

    # ---- firmware side -----------------------------------------------------------

    def send(self, line: str) -> None:
        with self.lock:
            conn = self.conn
        if conn is None:
            return
        try:
            conn.sendall((line + "\n").encode())
        except OSError:
            pass

    def handle_line(self, line: str) -> None:
        if line.startswith("AT+PUB="):
            fw_id, topic, payload = line[7:].split(",", 2)
            if not self.online():
                self.send(f"+PUBACK:{fw_id},ERR")
                return
            info = self.client.publish(topic, payload, qos=1)
            with self.lock:
                self.pending[info.mid] = int(fw_id)
                self.published += 1
                self.bytes_up += len(topic) + len(payload)
            log(f"published #{fw_id} to {topic}: {payload[:96]}")
        elif line.startswith("AT+TEL="):
            try:
                tel = json.loads(line[7:])
            except json.JSONDecodeError:
                return
            tel["module"] = {"online": self.online(), "mqtt": self.mqtt_connected,
                             "outage_s": max(0.0, round(self.outage_until - time.monotonic(), 1))}
            with self.lock:
                self.telemetry = tel
                subscribers = list(self.subscribers)
            for q in subscribers:
                q.put(tel)
        elif line.startswith("AT+NETDOWN="):
            self.start_outage(float(line[11:] or 10))

    def serve_firmware(self) -> None:
        module = self

        class Handler(socketserver.StreamRequestHandler):
            def handle(self) -> None:
                log("firmware connected")
                with module.lock:
                    module.conn = self.connection
                module.report_link(force=True)
                try:
                    for raw in self.rfile:
                        line = raw.decode(errors="replace").strip()
                        if line:
                            module.handle_line(line)
                except OSError:
                    pass  # Cable pulled (firmware exited or reset): a normal event.
                with module.lock:
                    module.conn = None
                log("firmware disconnected")

        socketserver.ThreadingTCPServer.allow_reuse_address = True
        server = socketserver.ThreadingTCPServer(("127.0.0.1", self.args.link_port), Handler)
        log(f"waiting for firmware on 127.0.0.1:{self.args.link_port}")
        threading.Thread(target=server.serve_forever, daemon=True).start()

    def watch_outage(self) -> None:
        # Report the end of a simulated outage.
        while True:
            time.sleep(0.2)
            self.report_link()

    # ---- dashboard side ----------------------------------------------------------

    def send_udp(self, text: str) -> None:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.sendto(text.encode(), ("127.0.0.1", self.args.firmware_udp))

    def serve_http(self) -> None:
        module = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, fmt, *args) -> None:  # quiet
                pass

            def _headers(self, code: int, ctype: str = "application/json") -> None:
                self.send_response(code)
                self.send_header("Content-Type", ctype)
                self.send_header("Access-Control-Allow-Origin", "*")
                self.send_header("Cache-Control", "no-cache")
                self.end_headers()

            def _json(self, code: int, body: dict) -> None:
                self._headers(code)
                self.wfile.write(json.dumps(body).encode())

            def do_OPTIONS(self) -> None:
                self.send_response(204)
                self.send_header("Access-Control-Allow-Origin", "*")
                self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
                self.end_headers()

            def do_GET(self) -> None:
                path = urlparse(self.path).path
                if path == "/api/status":
                    self._json(200, {"online": module.online(), "mqtt": module.mqtt_connected,
                                     "firmware": module.conn is not None, "published": module.published,
                                     "bytes_up": module.bytes_up})
                elif path == "/api/telemetry/stream":
                    self._stream()
                else:
                    self._json(404, {"error": "not found"})

            def _stream(self) -> None:
                q: queue.Queue = queue.Queue()
                with module.lock:
                    module.subscribers.append(q)
                    latest = module.telemetry
                self._headers(200, "text/event-stream")
                try:
                    if latest:
                        q.put(latest)
                    while True:
                        try:
                            tel = q.get(timeout=15)
                            self.wfile.write(f"data: {json.dumps(tel)}\n\n".encode())
                        except queue.Empty:
                            self.wfile.write(b": keep-alive\n\n")
                        self.wfile.flush()
                except OSError:
                    pass
                finally:
                    with module.lock:
                        module.subscribers.remove(q)

            def do_POST(self) -> None:
                url = urlparse(self.path)
                parts = url.path.strip("/").split("/")
                if url.path == "/api/outage":
                    seconds = float(parse_qs(url.query).get("seconds", ["10"])[0])
                    module.start_outage(seconds)
                    self._json(200, {"outage_s": seconds})
                elif url.path == "/api/motion":
                    module.send_udp("MOTION dashboard")
                    self._json(200, {"motion": True})
                elif len(parts) == 3 and parts[:2] == ["api", "fault"] and parts[2] in FAULTS:
                    if parts[2] == "network_down":
                        module.start_outage(10)
                    else:
                        module.send_udp(f"FAULT {parts[2]}")
                    log(f"fault injected: {parts[2]}")
                    self._json(200, {"fault": parts[2]})
                else:
                    self._json(404, {"error": "unknown endpoint or fault"})

        server = ThreadingHTTPServer(("127.0.0.1", self.args.http_port), Handler)
        log(f"dashboard API on http://127.0.0.1:{self.args.http_port}/api/")
        threading.Thread(target=server.serve_forever, daemon=True).start()


def main() -> None:
    p = argparse.ArgumentParser(description="EdgeVision Wi-Fi module emulator")
    p.add_argument("--link-port", type=int, default=5056, help="firmware link (UART) port")
    p.add_argument("--http-port", type=int, default=8081, help="dashboard API port")
    p.add_argument("--firmware-udp", type=int, default=5055, help="firmware UDP port for MOTION/FAULT commands")
    p.add_argument("--broker", default="localhost", help="MQTT broker host (AWS IoT endpoint for the cloud)")
    p.add_argument("--port", type=int, default=1883, help="MQTT port (8883 for AWS IoT)")
    p.add_argument("--client-id", default="edge-cam-01", help="MQTT client id (AWS IoT thing name)")
    p.add_argument("--ca", help="CA certificate (AmazonRootCA1.pem)")
    p.add_argument("--cert", help="device certificate")
    p.add_argument("--key", help="device private key")
    args = p.parse_args()

    module = WifiModule(args)
    module.serve_firmware()
    module.serve_http()
    module.start_mqtt()
    try:
        module.watch_outage()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()

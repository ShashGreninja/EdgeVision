"""Test harness for the EdgeVision firmware: run it, script it, read its results.

Used by the functional tests (firmware/tests/test_firmware.py) and the
evaluation (eval/run_eval.py).

    run = run_firmware(["--auto-motion", "60"], seconds=8)
    run.done["camera"]["wakeups"]      # the final telemetry JSON
    run.events                         # every [event] line, parsed
    run.lines                          # raw output

A FakeModule stands in for the Wi-Fi module: tests choose when the network
is up, and it records every publish and telemetry line it receives.
"""

from __future__ import annotations

import json
import os
import socket
import subprocess
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS2_BIN = r"C:\msys64\ucrt64\bin"
READY_MARKER = "[boot] heap locked"  # Printed once the scheduler runs and every input is up.


def build_dir() -> Path:
    return Path(os.environ.get("EDGEVISION_BUILD", Path(os.environ.get("LOCALAPPDATA", ".")) / "edgevision" / "build"))


def firmware_exe() -> Path:
    return build_dir() / "edgevision.exe"


def unit_test_exe() -> Path:
    return build_dir() / "edgevision_tests.exe"


def env() -> dict:
    e = dict(os.environ)
    e["PATH"] = MSYS2_BIN + os.pathsep + e.get("PATH", "")  # OpenCV and runtime DLLs
    return e


def free_port(kind: int = socket.SOCK_DGRAM) -> int:
    with socket.socket(socket.AF_INET, kind) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def send_udp(port: int, text: str) -> None:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.sendto(text.encode(), ("127.0.0.1", port))


class FakeModule:
    """A scriptable Wi-Fi module: answers AT+PUB, reports +LINK, records everything."""

    def __init__(self, online: bool = True) -> None:
        self.server = socket.socket()
        self.server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.server.bind(("127.0.0.1", 0))
        self.server.listen(1)
        self.port = self.server.getsockname()[1]
        self.online = online
        self.conn: socket.socket | None = None
        self.published: list[dict] = []   # {"id", "topic", "payload"} in order
        self.telemetry: list[dict] = []
        self.lock = threading.Lock()
        threading.Thread(target=self._serve, daemon=True).start()

    def _send(self, line: str) -> None:
        if self.conn:
            try:
                self.conn.sendall((line + "\n").encode())
            except OSError:
                pass

    def set_online(self, online: bool) -> None:
        self.online = online
        self._send("+LINK:UP" if online else "+LINK:DOWN")

    def _serve(self) -> None:
        try:
            self.conn, _ = self.server.accept()
        except OSError:
            return
        self._send("+LINK:UP" if self.online else "+LINK:DOWN")
        buf = b""
        while True:
            try:
                data = self.conn.recv(65536)
            except OSError:
                return
            if not data:
                return
            buf += data
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                self._handle(raw.decode(errors="replace"))

    def _handle(self, line: str) -> None:
        if line.startswith("AT+PUB="):
            msg_id, topic, payload = line[7:].split(",", 2)
            if not self.online:
                self._send(f"+PUBACK:{msg_id},ERR")
                return
            with self.lock:
                self.published.append({"id": int(msg_id), "topic": topic, "payload": json.loads(payload)})
            self._send(f"+PUBACK:{msg_id},OK")
        elif line.startswith("AT+TEL="):
            with self.lock:
                self.telemetry.append(json.loads(line[7:]))

    def latest_telemetry(self) -> dict | None:
        with self.lock:
            return self.telemetry[-1] if self.telemetry else None

    def close(self) -> None:
        self.server.close()
        if self.conn:
            self.conn.close()


@dataclass
class Run:
    returncode: int
    lines: list[str]
    events: list[dict] = field(default_factory=list)
    done: dict | None = None
    module: FakeModule | None = None

    def has(self, text: str) -> bool:
        return any(text in line for line in self.lines)

    def event_names(self) -> list[str]:
        return [e["event"] for e in self.events]


def run_firmware(args: list[str], seconds: int, *, clip: str | None = None, module: FakeModule | None = None,
                 udp_port: int = 0, actions: list[tuple[float, callable]] | None = None, timeout: float | None = None,
                 log_name: str | None = None) -> Run:
    """Run the firmware for `seconds` (its own clock) and collect its output.

    actions: (seconds, callable) pairs run on timer threads, counted from the
    moment the firmware reports it is up (scheduler running, inputs ready) so
    they line up with its own clock, e.g.
    (2.0, lambda: send_udp(port, "FAULT frame_corrupt")).
    """
    cmd = [str(firmware_exe())]
    if clip:
        cmd.append(clip)
    cmd += ["--run-seconds", str(seconds), "--udp-port", str(udp_port),
            "--link-port", str(module.port if module else 0)] + args

    proc = subprocess.Popen(cmd, cwd=ROOT, env=env(), stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    lines: list[str] = []
    timers = [threading.Timer(t, fn) for t, fn in (actions or [])]

    def read() -> None:
        started = False
        for raw in proc.stdout:
            line = raw.rstrip("\n")
            lines.append(line)
            if not started and line.startswith(READY_MARKER):
                started = True
                for t in timers:
                    t.start()

    reader = threading.Thread(target=read, daemon=True)
    reader.start()
    try:
        proc.wait(timeout=timeout or seconds * 3 + 30)
    finally:
        for t in timers:
            t.cancel()
        if proc.poll() is None:
            proc.kill()
        reader.join(timeout=5)
        proc.stdout.close()

    if log_name:
        # Keep the full output of every run, to debug a failing test afterwards.
        logs = build_dir() / "test-logs"
        logs.mkdir(parents=True, exist_ok=True)
        (logs / f"{log_name}.log").write_text("\n".join([" ".join(cmd), *lines]) + "\n", encoding="utf-8")

    run = Run(proc.returncode, lines, module=module)
    for line in lines:
        if line.startswith("[event] "):
            run.events.append(json.loads(line[8:]))
        elif line.startswith("[done] "):
            run.done = json.loads(line[7:])
    return run


def wait_until(predicate, timeout: float, interval: float = 0.1) -> bool:
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if predicate():
            return True
        time.sleep(interval)
    return False

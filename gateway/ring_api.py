r"""Call Ring's Partner API and optionally forward new motion to the firmware.

    .venv\Scripts\python gateway/ring_api.py
    .venv\Scripts\python gateway/ring_api.py --watch

Paste a Playground token at the hidden prompt, or set RING_ACCESS_TOKEN in the
environment. Tokens are never saved. Video input remains separate from events.
"""

from __future__ import annotations

import argparse
import getpass
import json
import os
import re
import socket
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid

from ring_gateway import Dedup

BASE_URL = "https://api.amazonvision.com"


class RingAPIError(Exception):
    """A safe diagnostic that does not include credentials or response bodies."""


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise urllib.error.HTTPError(req.full_url, code, "Redirect refused", headers, fp)


class RingAPI:
    def __init__(self, token: str, hardware_id: str):
        if not token or any(c.isspace() for c in token):
            raise RingAPIError("Provide a Ring access token without whitespace.")
        self.token = token
        self.hardware_id = hardware_id
        self.opener = urllib.request.build_opener(NoRedirect())

    def _get(self, path: str) -> list[dict]:
        request = urllib.request.Request(BASE_URL + path, headers={
            "Authorization": "Bearer " + self.token,
            "Content-Type": "application/json",
            "User-Agent": "EdgeVision/1.0",
            "Hardware_Id": self.hardware_id,
        })
        try:
            with self.opener.open(request, timeout=15) as response:
                raw = response.read(1024 * 1024 + 1)
            if len(raw) > 1024 * 1024:
                raise RingAPIError("Ring response exceeded 1 MB.")
            payload = json.loads(raw)
        except urllib.error.HTTPError as error:
            code = error.code
            error.close()
            messages = {
                401: "Ring token is invalid or expired; generate a new token in the Playground.",
                403: "Ring denied access; check the app's device permissions and account link.",
                406: "Ring rejected this client; check required headers or contact Ring developer support.",
                429: "Ring rate limit reached; wait before restarting.",
            }
            raise RingAPIError(messages.get(code, f"Ring API returned HTTP {code}.")) from None
        except (urllib.error.URLError, OSError):
            raise RingAPIError("Could not reach Ring over HTTPS; check connectivity.") from None
        except (ValueError, UnicodeError):
            raise RingAPIError("Ring returned invalid JSON.") from None
        if not isinstance(payload, dict) or not isinstance(payload.get("data"), list):
            raise RingAPIError("Ring returned an unexpected response shape.")
        if any(not isinstance(item, dict) for item in payload["data"]):
            raise RingAPIError("Ring returned invalid resource entries.")
        return payload["data"]

    def devices(self) -> list[dict]:
        return self._get("/v1/devices")

    def history(self, device: str, playground: bool = False) -> list[dict]:
        device_path = urllib.parse.quote(device, safe="")
        event_types = "motion,on_demand" if playground else "motion"
        return self._get(f"/v1/history/devices/{device_path}/events?event_types={event_types}")


class MotionHistory:
    def __init__(self, device: str, playground: bool = False):
        self.device = re.sub(r"\s", "_", device)
        self.playground = playground
        self.seen = Dedup()

    def seed(self, events: list[dict]) -> None:
        for event in events:
            if event.get("id"):
                self.seen.first_time(str(event["id"]))

    def commands(self, events: list[dict]) -> list[str]:
        commands = []
        for event in reversed(events):
            event_id = event.get("id")
            attrs = event.get("attributes")
            if not event_id or not isinstance(attrs, dict):
                continue
            event_type = str(attrs.get("event_type", ""))
            live_view = self.playground and event_type == "on_demand"
            if not live_view and event_type.split(".", 1)[0] != "motion":
                continue
            if self.seen.first_time(str(event_id)):
                subtype = "playground_live_view" if live_view else re.sub(r"\s", "_", event_type.partition(".")[2] or "motion")
                commands.append(f"MOTION ring {subtype} {self.device}")
        return commands


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", help="Ring device ID; required when more than one device is accessible")
    parser.add_argument("--watch", action="store_true", help="poll new motion events and wake the running firmware")
    parser.add_argument("--playground", action="store_true", help="also use Playground live-view events as simulated wake triggers")
    parser.add_argument("--firmware-udp", type=int, default=5055)
    parser.add_argument("--poll-seconds", type=float, default=5)
    args = parser.parse_args()
    if not 1 <= args.firmware_udp <= 65535 or args.poll_seconds < 1:
        parser.error("use a valid UDP port and a polling interval of at least one second")
    token = os.environ.get("RING_ACCESS_TOKEN") or getpass.getpass("Ring Playground token (hidden): ")
    hardware_id = str(uuid.uuid5(uuid.NAMESPACE_URL, "edgevision:" + socket.gethostname() + ":" + getpass.getuser()))
    try:
        api = RingAPI(token.strip(), hardware_id)
        devices = api.devices()
        print(f"Ring API connected: {len(devices)} accessible device(s).", flush=True)
        for device in devices:
            print(json.dumps({"id": device.get("id"), "attributes": device.get("attributes", {})}), flush=True)
        ids = [device["id"] for device in devices if isinstance(device.get("id"), str)]
        selected = args.device or (ids[0] if len(ids) == 1 else None)
        if selected not in ids:
            raise RingAPIError("Select an accessible Ring device with --device <id>.")
        events = api.history(selected, playground=args.playground)
        print(f"Ring event history: {len(events)} event(s).", flush=True)
        if not args.watch:
            return
        history = MotionHistory(selected, playground=args.playground)
        history.seed(events)
        print("Watching new Ring motion; existing history will not be replayed. Ctrl+C stops.", flush=True)
        if args.playground:
            print("Playground mode: new live-view sessions also wake the local simulator; these are not real motion alerts.", flush=True)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            while True:
                time.sleep(args.poll_seconds)
                for command in history.commands(api.history(selected, playground=args.playground)):
                    udp.sendto(command.encode(), ("127.0.0.1", args.firmware_udp))
                    label = "Playground live-view trigger" if " playground_live_view " in command else "Ring motion"
                    print(f"{label} forwarded to firmware.", flush=True)
    except RingAPIError as error:
        raise SystemExit(str(error)) from None
    except KeyboardInterrupt:
        print("\nRing connection stopped.")


if __name__ == "__main__":
    main()

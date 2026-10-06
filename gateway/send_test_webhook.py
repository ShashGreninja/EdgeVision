"""Send a signed Ring-style motion webhook to the local gateway.

Builds a webhook v1.1 motion_detected payload in the format Ring documents,
signs it the way Ring does, and POSTs it. Useful without a Ring account, and
to demonstrate signature checking and duplicate handling.

    set RING_HMAC_KEY=<same key the gateway uses>
    python gateway/send_test_webhook.py                  # one human motion event
    python gateway/send_test_webhook.py --twice          # same request twice: second is a duplicate
    python gateway/send_test_webhook.py --bad-signature  # rejected with 401
"""

from __future__ import annotations

import argparse
import json
import os
import time
import urllib.error
import urllib.request
import uuid
from datetime import datetime, timezone

from ring_gateway import sign


def motion_payload(device: str, sub_type: str) -> dict:
    now_ms = int(time.time() * 1000)
    return {
        "meta": {
            "version": "1.1",
            "time": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
            "request_id": str(uuid.uuid4()),
            "account_id": "ava1.ring.account.TEST",
        },
        "data": {
            "id": f"{device}_{sub_type}_{now_ms}",
            "type": "motion_detected",
            "attributes": {
                "source": device,
                "source_type": "devices",
                "timestamp": now_ms,
                "sub_type": sub_type,
            },
            "relationships": {"devices": {"links": {"self": f"/v1/devices/{device}"}}},
        },
    }


def post(url: str, body: bytes, signature: str) -> tuple[int, str]:
    req = urllib.request.Request(url, data=body, method="POST",
                                 headers={"Content-Type": "application/json", "X-Signature": signature})
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            return resp.status, resp.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--url", default="http://127.0.0.1:8787/webhooks/ring")
    p.add_argument("--device", default="test-doorbell-1")
    p.add_argument("--sub-type", default="human")
    p.add_argument("--twice", action="store_true", help="send the same delivery twice")
    p.add_argument("--bad-signature", action="store_true")
    args = p.parse_args()

    key = os.environ.get("RING_HMAC_KEY", "")
    if not key:
        raise SystemExit("set RING_HMAC_KEY to the key the gateway uses")

    body = json.dumps(motion_payload(args.device, args.sub_type)).encode()
    signature = sign("wrong-key" if args.bad_signature else key, body)

    for attempt in range(2 if args.twice else 1):
        code, text = post(args.url, body, signature)
        print(f"delivery {attempt + 1}: HTTP {code} {text}")


if __name__ == "__main__":
    main()

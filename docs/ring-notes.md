# Ring integration notes

How EdgeVision uses Ring, and what Ring's documentation says about it.
**Never commit tokens, client secrets or the HMAC signing key.** Replace IDs
with placeholders like `<device-id>`.

## How EdgeVision uses Ring

Ring is the trigger. A Ring motion event wakes the virtual camera, which then
decides locally whether a person, vehicle or animal is really there:

```
Ring (device or Playground) --HTTPS webhook--> gateway/ring_gateway.py --UDP "MOTION ring <sub_type> <device>"--> firmware IRQ_MOTION
```

## What Ring's docs specify (Partner API, webhook v1.1)

- REST API base: `https://api.amazonvision.com` (JSON:API, OAuth 2.0 Bearer tokens).
- Webhooks are HTTPS POSTs to the URL configured for the app in the developer console.
- Every webhook carries `X-Signature: sha256=<hex>`, an HMAC-SHA256 of the **raw
  request body** with the app's HMAC signing key. Verify before parsing; never
  re-serialise the JSON before verifying.
- Reply HTTP 200 within 5 seconds.
- Idempotency: deduplicate on `meta.request_id` (or `data.id`).
- Motion payload:

  ```json
  {
    "meta": { "version": "1.1", "time": "<ISO 8601>", "request_id": "<uuid>", "account_id": "<account>" },
    "data": {
      "id": "<device_id>_motion_<timestamp>",
      "type": "motion_detected",
      "attributes": {
        "source": "<device_id>", "source_type": "devices",
        "timestamp": 1786715596787, "timestamp_readable": "2026-08-14 08:53:16",
        "sub_type": "motion"
      },
      "relationships": { "devices": { "links": { "self": "/v1/devices/<device_id>" } } }
    }
  }
  ```

  `sub_type` is `motion` without Smart Alerts, or `human`, `vehicle`,
  `package_delivery`, `other_motion` with Smart Alerts. Treat it as open-ended.
- Live video: WebRTC via WHEP at `/v1/devices/<device_id>/media/streaming/whep/sessions`
  (video only); battery devices stream for at most 30 seconds.
- The Developer Playground (`developer.amazon.com/ring/console/playground`)
  generates 30-minute OAuth tokens, has an API explorer, and simulates live-view
  events (motion, package, vehicle).

Sources: developer.amazon.com/docs/ring/api-documentation.html (Notifications,
Motion Detection sections) and developer.amazon.com/docs/ring/develop.html.

## Connecting a real Ring webhook

1. In the Ring developer console, open the EdgeVision app and note the HMAC
   signing key (shown once at app creation).
2. Start the gateway with the key in the environment:
   `set RING_HMAC_KEY=<key>` then `python gateway/ring_gateway.py`.
3. Expose it over HTTPS, for example with a Cloudflare quick tunnel:
   `cloudflared tunnel --url http://localhost:8787`.
4. Set the app's webhook URL to `https://<tunnel-host>/webhooks/ring`.
5. Trigger motion on a linked device (or from a staging user's device).

Without Ring access, `gateway/send_test_webhook.py` sends a correctly signed
payload in the same format.

## Direct Ring API connection

`gateway/ring_api.py` calls Ring's official Partner API from Python. It sends
the required Bearer token, User-Agent and Hardware_Id headers over HTTPS.
Paste a temporary token from the Ring Developer Playground at the hidden
prompt; the client keeps it in memory and never saves it. Alternatively,
provide `RING_ACCESS_TOKEN` through the process environment, not a CLI argument.

```powershell
# Check access: discover devices and read motion history.
.venv\Scripts\python gateway\ring_api.py

# With run_demo.ps1 already running, forward new Ring motion to the firmware.
.venv\Scripts\python gateway\ring_api.py --watch

# Official Playground: use its live-view simulation as a local wake trigger.
.venv\Scripts\python gateway\ring_api.py --watch --playground
```

If multiple devices are accessible, select one with `--device <Ring-device-id>`.
Watch mode polls every five seconds and deliberately skips history present at
startup. New motion events are deduplicated and forwarded to localhost UDP
5055, using the same command as the webhook gateway. Ctrl+C stops the client.
On token expiry, generate a new Playground token and restart the client.

The Playground's Motion/Package/Vehicle buttons create live-view sessions.
Their history entries have `event_type: on_demand`, not `motion`. Use the
explicit `--playground` switch for these sessions; they are labeled
`playground_live_view` when forwarded. Normal watch mode accepts only motion
events, and does not treat a user opening live view as a motion alert.

This polling path does not require a public tunnel or the app's HMAC signing
key. The signed webhook receiver remains available for real-time delivery.
Polling examines the latest history page, so it is a demo integration rather
than a durable backfill mechanism for large event volumes.

### Validation and limits

On 2026-10-29, authenticated device discovery and motion-history requests
succeeded against Ring's official API using a temporary Playground token.
The Python client returned one device named `Playground Device` and a history
event. Inspecting the Playground response confirmed it was an `on_demand`
live-view event. Gateway tests also verify HMAC rejection, duplicate suppression,
and UDP delivery; API-client tests cover authentication headers, credential-safe
errors, redirect rejection, device-path escaping, and event deduplication.

The complete Playground test also passed: a newly generated Ring live-view
event was polled by `--watch --playground`, the firmware logged
`motion pin raised (ring playground_live_view ...)`, and local-clip inference
produced `person_detected` events with confidence 0.72 and 0.76. The Wi-Fi module
published both over MQTT/TLS, and the deployed AWS events API returned them.
This checks Ring event ingestion through to cloud output, with simulated video
input. All 18 API-client and webhook-gateway tests passed.

This validates the official Ring sandbox, not a physical Ring camera. Video
inference still uses the local clip: Ring WHEP/RTSPS video is not yet connected
to the virtual sensor. Delivery from Ring to the public webhook endpoint also
remains unverified.

Sources: [Ring event history and polling](https://developer.amazon.com/docs/ring/api-documentation.html#event-history),
[required request headers](https://developer.amazon.com/docs/ring/api-documentation.html#required-http-headers).

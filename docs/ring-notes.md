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

## Still to confirm with a real account

- Whether the Playground's simulated events are delivered to the app's webhook
  URL, or only shown in the Playground UI.
- Whether WHEP live view is usable from our own code, which would let the
  virtual sensor use real Ring frames instead of a local clip.

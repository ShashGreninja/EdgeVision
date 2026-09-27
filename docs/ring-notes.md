# Ring Developer Playground: findings

Fill this in while exploring the Playground. **Never paste tokens, client secrets or the HMAC key here.**
Replace IDs with placeholders like `<device-id>`.

## Known from Ring's docs

- REST API base: `https://api.amazonvision.com`
- Webhooks: Ring POSTs JSON to an **HTTPS** endpoint you register. The endpoint must:
  - verify the HMAC-SHA256 signature (hex) using the app's HMAC Signature Key
  - return HTTP 200 within 5 seconds
  - de-duplicate using `request_id`
- Events include a classification (human / animal / vehicle)
- Live video: WebRTC via WHEP (`/v1/devices/<device-id>/media/streaming/whep/sessions`), video only.
  Battery devices are limited to 30-second streams.
- The Playground simulates Motion, Package and Vehicle events, including live view.

## To find out

1. **Access**: how did we get into the Playground (hackathon credentials, developer registration)?
2. **Motion event payload**: paste one simulated motion event, with IDs replaced:

   ```json
   ```

3. **Delivery**: can the Playground deliver webhooks to a URL we choose, or do we poll event history?
   If webhooks: we need a public HTTPS URL (e.g. a Cloudflare quick tunnel to the local gateway).
4. **Live view**: can we open the WHEP stream from our own code? What resolution and FPS?
   (If yes, the virtual sensor can use real Ring frames instead of a local clip.)
5. **Event history API**: endpoint and fields, for the eval step.

## Decisions (fill in after exploring)

- Gateway input: webhook via tunnel / polling / Playground UI only
- Sensor source: local clip / Ring live view

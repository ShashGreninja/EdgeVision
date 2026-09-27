# EdgeVision

Software-defined embedded vision pipeline for privacy-preserving Ring security events.

A virtual smart camera running on FreeRTOS (Windows simulator port) processes video locally
under embedded constraints (512 KB RAM, fixed per-frame CPU budget) and sends only small
event metadata to AWS. Ring motion events from the Ring Developer Playground wake the pipeline.

Design: https://claude.ai/artifact/JPmYmWXydyyioKvHsSLYMX

## Layout

| Folder       | Language           | What lives here                                                      |
|--------------|--------------------|----------------------------------------------------------------------|
| `firmware/`  | C (FreeRTOS)       | HAL, drivers, pipeline, services: the embedded core                  |
| `gateway/`   | Python             | Ring webhook receiver → simulated MOTION interrupt over UDP          |
| `cloud/`     | TypeScript (CDK)   | IoT Core policy, IoT Rule, DynamoDB, Lambda, API Gateway             |
| `dashboard/` | TypeScript         | Live telemetry (WebSocket) + cloud event history                     |
| `eval/`      | Python             | Labeled clips, bandwidth-saved vs. recall measurement                |
| `models/`    | —                  | Detector model files (ONNX), not committed                           |
| `docs/`      | Markdown           | Notes, including what the Ring Playground actually sends             |

## Toolchain (Windows, no WSL)

- MSYS2 UCRT64: GCC, CMake, Ninja, OpenCV (`C:\msys64\ucrt64\bin`)
- Python 3 for the gateway and eval scripts
- Node.js for the dashboard and CDK

## Status

- [ ] Day 0: toolchain, Ring Playground findings (`docs/ring-notes.md`), AWS account
- [ ] Day 1: FreeRTOS sim build, HAL, frame pool, first task → queue → task
- [ ] Day 2: ISP, motion gate, simulated NPU inference, decision, governor
- [ ] Day 3: Ring gateway, coreMQTT → AWS IoT Core → DynamoDB, offline queue
- [ ] Day 4: watchdog, fault injection, dashboard
- [ ] Day 5: evaluation, README diagram, demo video

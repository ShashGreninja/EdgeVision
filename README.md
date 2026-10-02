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

## Build and run the firmware

From `firmware/` in PowerShell:

```powershell
.\build.ps1 -Run                       # synthetic scene; press m for motion, q to quit
.\build.ps1 -Run -- ..\eval\clips\door.mp4
.\build.ps1 -Run -- --run-seconds 11 --auto-motion 60   # unattended test run
```

Options:

| Option | Default | What it does |
|---|---|---|
| `--model fp32\|int8\|path` | `fp32` | Detector for the virtual NPU (NanoDet-Plus, OpenCV model zoo) |
| `--npu-ms N` | 75 (fp32), 30 (int8) | Emulated NPU latency per job |
| `--motion-thresh T` | 20 | Per-cell brightness change that counts as motion |
| `--cpu-scale K` | 20 | The device CPU is K times slower than this PC |
| `--no-governor` | off | Admit every frame, to compare against the governor |
| `--udp-port P` | 5055 | MOTION datagrams from the gateway (0 = off) |
| `--auto-motion S` | off | Raise MOTION every S seconds |
| `--run-seconds N` | forever | Exit with a summary after N seconds |

The models are not committed. Download them into `models/` from
https://huggingface.co/opencv/object_detection_nanodet (Apache-2.0):
`object_detection_nanodet_2022nov.onnx` and `object_detection_nanodet_2022nov_int8.onnx`.

Build output goes to `%LOCALAPPDATA%\edgevision\build`, outside OneDrive.
`edgevision_tests.exe` in the same folder runs the frame-pool unit tests.

## Status

- [ ] Day 0: toolchain, Ring Playground findings (`docs/ring-notes.md`), AWS account
- [x] Day 1: FreeRTOS sim build, HAL, frame pool, first task → queue → task
- [x] Day 2: ISP, motion gate, simulated NPU inference, decision, governor
- [ ] Day 3: Ring gateway, coreMQTT → AWS IoT Core → DynamoDB, offline queue
- [ ] Day 4: watchdog, fault injection, dashboard
- [ ] Day 5: evaluation, README diagram, demo video

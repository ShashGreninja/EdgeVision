# EdgeVision

**A software-defined smart-camera firmware that decides locally what matters and
sends the cloud an event, not the video.**

EdgeVision simulates the software inside a Ring-style security camera. A Ring
motion event wakes it; the firmware, running on FreeRTOS under the constraints
of a small device (512 KB of RAM, a fixed compute budget per second, a 75 ms
neural accelerator), checks the scene itself and publishes a ~170-byte event
such as `person_detected` to AWS IoT over MQTT. Video never leaves the device.

On the labelled test clip (see [eval/results.md](eval/results.md)), every run
caught the visit, 4.5 s after the person appeared, with no false events; the
camera uploaded about 430 bytes per visit instead of about 8.5 MB of video,
while staying within 280 KB of RAM with no memory allocated after start-up.

Design overview: https://claude.ai/artifact/JPmYmWXydyyioKvHsSLYMX

## How it works

```
Ring device / Playground --HTTPS webhook--> gateway (Python): verify HMAC, dedupe
                                               | UDP "MOTION"
                                               v
+----------------------- firmware (C, FreeRTOS, 512 KB heap) -------------------------+
| IRQ_MOTION -> CameraTask --frame queue--> MotionTask --infer queue--> InferenceTask  |
|               |  DMA + CRC check            | motion gate, re-checks     | NPU job    |
|               |  governor: admit/drop       v                            v            |
|               |                         (frame freed)             IRQ_NPU_DONE       |
|               v                                                          |            |
|          3-slot frame pool                     DecisionTask <--decisions--+            |
|                                                 | 3 of 5 hits -> event                 |
| WatchdogTask  FaultTask  TelemetryTask          v                                      |
|                                            NetworkTask: store-and-forward (16 events) |
+------------------------------------------------|-------------------------------------+
   virtual hardware (Windows threads): sensor + ISP + DMA, NPU, Wi-Fi link (UART)
                                                 | AT+PUB / AT+TEL
                                                 v
                          Wi-Fi module emulator (Python): MQTT, TLS offload
                                                 | MQTT
                      +--------------------------+---------------------------+
                      v                                                      v
   AWS: IoT Core -> rule -> DynamoDB -> Lambda + HTTP API      local: Mosquitto -> stub -> SQLite -> API
                                                 |
                                                 v
                          dashboard: live telemetry, events, fault buttons
```

**Embedded techniques on show**

| Technique | Where |
|---|---|
| RTOS tasks with priorities, queues, task notifications, mutexes | `firmware/` throughout |
| Simulated interrupts from "hardware" threads (motion pin, DMA done, NPU done, link RX, debug port) | `hal/`, ISRs in drivers and services |
| Static frame pool with an ownership state machine; illegal transitions assert | `drivers/frame_pool.c` |
| Hard RAM budget; every allocation counted; none allowed after start-up | `config/FreeRTOSConfig.h`, `main.c` |
| Frame-skip governor: token bucket over a per-second compute budget | `services/governor.c` |
| Cheap motion gate in front of an expensive detector | `pipeline/motion.c` |
| Off-loaded accelerator with job submit / completion interrupt / reset | `hal/hal_npu.cpp`, `pipeline/inference.c` |
| Watchdog restarting a hung task in place (static task memory) | `services/watchdog.c` |
| CRC-32 integrity check on every DMA transfer | `common/crc32.c`, `drivers/camera_driver.c` |
| Sensor recovery with exponential backoff | `drivers/camera_driver.c` |
| Store-and-forward networking over an AT-command Wi-Fi module | `services/network.c`, `netmodule/` |
| Fault injection for every failure mode | `services/faults.c` |

## Repository

| Folder | Language | Contents |
|---|---|---|
| `firmware/` | C (C++ for virtual hardware) | FreeRTOS firmware: `hal/`, `drivers/`, `pipeline/`, `services/`, unit and functional tests |
| `netmodule/` | Python | Wi-Fi module emulator: MQTT client (local or AWS IoT over TLS), dashboard API |
| `gateway/` | Python | Ring webhook receiver with signature check and deduplication, plus a signed test sender |
| `cloud/aws/` | TypeScript (CDK), Python | IoT thing and policy, IoT rule, DynamoDB, Lambda + HTTP API |
| `cloud/local/` | Python | The same backend on one PC: Mosquitto subscriber, SQLite, events API; serves the dashboard |
| `dashboard/` | HTML, CSS, JS | Live dashboard |
| `eval/` | Python | Labelled evaluation and results |
| `tools/` | Python | Test harness shared by tests and evaluation |
| `scripts/` | PowerShell | Start and stop the full local demo |
| `docs/` | Markdown | Ring integration notes, demo script |

## Setup (Windows)

1. **Toolchain.** Install [MSYS2](https://www.msys2.org/) to `C:\msys64`, then in the *MSYS2 UCRT64* shell:
   ```bash
   pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-opencv mingw-w64-ucrt-x86_64-mosquitto
   ```
   If Windows Smart App Control blocks the compiler, it has to be turned off
   (Windows Security > App & browser control).
2. **Python.** From the repo root:
   ```powershell
   python -m venv .venv
   .venv\Scripts\pip install -r requirements.txt
   ```
3. **Models** (not committed). Download into `models/` from
   https://huggingface.co/opencv/object_detection_nanodet (Apache-2.0):
   `object_detection_nanodet_2022nov.onnx` and `object_detection_nanodet_2022nov_int8.onnx`.
4. **Build.** `.\firmware\build.ps1` (CMake fetches FreeRTOS-Kernel V11.3.1; output
   goes to `%LOCALAPPDATA%\edgevision\build`, outside OneDrive).
5. **Editor (VS Code).** The build writes `compile_commands.json` to the build
   folder. Point the C/C++ extension at it, or IntelliSense cannot find the
   FreeRTOS headers (CMake downloads them into the build folder):
   `.vscode/c_cpp_properties.json` with
   `"compileCommands": "${env:LOCALAPPDATA}/edgevision/build/compile_commands.json"`
   and `"compilerPath": "C:/msys64/ucrt64/bin/gcc.exe"`.
6. **Test clip** (optional, not committed): put an `.mp4` of someone walking to a
   door in `eval/clips/`. Without one, the sensor shows a synthetic moving block.

## Run

```powershell
.\scripts\run_demo.ps1 -Clip eval\clips\testclip.mp4   # broker, cloud stub, Wi-Fi module, firmware, dashboard
.\scripts\stop_demo.ps1
```

The dashboard opens at http://127.0.0.1:8000/. Press **Trigger motion** (or `m` in
the firmware window), then try the fault buttons. See [docs/demo-script.md](docs/demo-script.md)
for a three-minute walkthrough.

The firmware alone: `.\firmware\build.ps1 -Run -- eval\clips\testclip.mp4`. Keys:
`m` motion, `1`-`7` faults, `q` quit.

| Option | Default | What it does |
|---|---|---|
| `--model fp32\|int8\|path` | `fp32` | Detector for the virtual NPU |
| `--npu-ms N` | 75 (fp32), 30 (int8) | Emulated NPU latency per job |
| `--motion-thresh T` | 20 | Per-cell brightness change that counts as motion |
| `--cpu-scale K` | 20 | The device CPU is K times slower than this PC |
| `--no-governor` | off | Admit every frame, to compare against the governor |
| `--udp-port P` | 5055 | MOTION and FAULT commands (0 = off) |
| `--link-port P` | 5056 | Wi-Fi module link (0 = no module) |
| `--device-id ID` | `edge-cam-01` | Device name in MQTT topics |
| `--auto-motion S` | off | Raise MOTION every S seconds |
| `--run-seconds N` | forever | Exit after N seconds, printing `[done] <telemetry JSON>` |

### With Ring

Set the app's HMAC signing key and start the gateway (`run_demo.ps1` starts it
automatically when the key is set):

```powershell
$env:RING_HMAC_KEY = "<signing key from the Ring developer console>"
.venv\Scripts\python gateway\ring_gateway.py
```

Expose it over HTTPS (for example `cloudflared tunnel --url http://localhost:8787`) and
set the app's webhook URL to `https://<host>/webhooks/ring`. Without Ring access,
`gateway\send_test_webhook.py` sends a correctly signed motion webhook.
Details and the payload format: [docs/ring-notes.md](docs/ring-notes.md).

### With AWS

`cloud/aws` deploys the backend with CDK and explains the device certificate:
see [cloud/aws/README.md](cloud/aws/README.md). Then run the Wi-Fi module against
the AWS IoT endpoint and open the dashboard with `?api=<EventsApiUrl>`.

## Tests and evaluation

```powershell
.\firmware\test.ps1                         # build, gateway tests, 17 firmware tests (~2.5 min)
.\firmware\test.ps1 -Quick                  # build and the fast group only
.venv\Scripts\python eval\run_eval.py       # labelled evaluation -> eval/results.md
```

The functional tests run the real firmware through each scenario: wake and
sleep, the governor, detection on the clip and none on an empty scene, every
fault with its recovery, and store-and-forward networking. The full output of
every test run is kept in `%LOCALAPPDATA%\edgevision\build\test-logs`.

## Limits

- The camera, ISP, NPU and Wi-Fi module are simulated; NPU latency is a
  modelled 75 ms, not a measurement on camera hardware.
- One labelled clip so far: the evaluation shows the system works end to end;
  it is not a general recall figure for the detector. The camera stayed awake
  for the whole visit in 4 of 6 runs; it can lose a person who turns away.
- The AWS stack is written and synthesizes, but has not been deployed from this
  repository yet.
- The firmware runs on the FreeRTOS Windows port, which is not real-time;
  timings are representative, not cycle-accurate.

## Status

- [x] Day 0: toolchain, Ring Playground access, project layout
- [x] Day 1: FreeRTOS simulator build, virtual camera, DMA, frame pool, first tasks
- [x] Day 2: motion gate, virtual NPU inference, decision, frame-skip governor
- [x] Day 3: Ring gateway, NetworkTask with store-and-forward, Wi-Fi module, cloud (local and AWS CDK)
- [x] Day 4: watchdog, fault injection with recovery, JSON telemetry, dashboard
- [x] Day 5: functional tests, evaluation, documentation
- [ ] Deploy the AWS stack and connect a real Ring webhook

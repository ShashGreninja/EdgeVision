# Demo script (about 3 minutes)

Before recording: `.\scripts\run_demo.ps1 -Clip eval\clips\testclip.mp4`, arrange
the dashboard and the firmware window side by side. If you have the Ring
gateway set up, have the Ring Playground or a test webhook ready.

| Time | Do | Say |
|---|---|---|
| 0:00 | Show the design overview | "A doorbell camera has 512 KB of RAM and a few hundred milliseconds per frame. EdgeVision is the firmware that decides locally what matters, so only a tiny event goes to the cloud." |
| 0:20 | Trigger motion: Ring webhook (`gateway\send_test_webhook.py`) or **Trigger motion** | "A Ring motion event raises the camera's motion interrupt. The sensor powers on." |
| 0:35 | Point at the frame chart and the governor strip | "Green frames stop at the motion gate for a few milliseconds. Blue ones go to the AI chip. Orange ones the governor drops on purpose, so the pipeline never falls behind." |
| 0:55 | The `person_detected` event appears in the cloud table | "Three positive checks out of five confirm a person: one 170-byte MQTT message, not a video stream." |
| 1:10 | Point at **Bandwidth saved** | "About 400 bytes per visit instead of megabytes of video." |
| 1:25 | **Network down**, then **Trigger motion** | "Wi-Fi drops. The camera keeps detecting and holds events in RAM..." |
| 1:45 | Wait for the link pill to turn green | "...and sends them in order when the network comes back." |
| 2:00 | **AI chip hang** | "The neural accelerator stops answering. Two seconds later the watchdog restarts the inference task and resets the chip, reusing its memory: still zero allocations after start-up." |
| 2:20 | **Camera disconnect**, **Frame corruption** | "A loose sensor cable is reset with exponential backoff; corrupted transfers fail their CRC and are dropped." |
| 2:35 | **CPU overload** | "If the processor slows down, the governor drops more frames and the camera stays real-time." |
| 2:50 | Show `eval/results.md` | "On the test clip it caught every visit, with no false events and 280 KB of RAM. Everything is in the repo with tests." |

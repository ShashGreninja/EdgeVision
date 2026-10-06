# EdgeVision evaluation

Generated 2026-10-07 00:16 by `eval/run_eval.py` (6 clip runs, 3 empty-scene runs).
Model: NanoDet-Plus FP32, emulated NPU latency 75 ms, governor on. One Ring-style wake-up per run.

## Person visits

| Run | Visit detected | First event (clip time) | Awake while person present | False events | Frames: motion gate / AI chip / dropped | Uploaded | Video baseline (awake) | Saved |
|---|---|---|---|---|---|---|---|---|
| testclip.mp4 #1 | yes | 4.5 s | 100% | 0 | 77% / 12% / 11% of 1228 | 520 B (3 events) | 9.8 MB | 99.995% |
| testclip.mp4 #2 | yes | 4.5 s | 100% | 0 | 77% / 11% / 11% of 1237 | 520 B (3 events) | 9.8 MB | 99.995% |
| testclip.mp4 #3 | yes | 4.5 s | 100% | 0 | 75% / 12% / 12% of 905 | 346 B (2 events) | 7.2 MB | 99.995% |
| testclip.mp4 #4 | yes | 4.5 s | 100% | 0 | 76% / 12% / 12% of 1233 | 520 B (3 events) | 9.8 MB | 99.995% |
| testclip.mp4 #5 | yes | 4.5 s | 55% | 0 | 82% / 10% / 7% of 445 | 173 B (1 event) | 3.4 MB | 99.995% |
| testclip.mp4 #6 | yes | 4.5 s | 54% | 0 | 83% / 9% / 7% of 445 | 173 B (1 event) | 3.4 MB | 99.995% |

## Summary

- **Visit recall:** 6/6 runs raised a correct person event.
- **Time to first event:** median 4.5 s of clip time after the person appears.
- **Stayed awake while the person was there:** median 100% of the visit; the whole visit in 4/6 runs, worst run 54%.
- **False events:** 0 on the clip; 0 on the empty scene (639 AI-chip checks without a single event).
- **AI chip load:** median 12% of captured frames reached the AI chip; 77% stopped at the motion gate.
- **Uploaded:** median 433 bytes per visit, versus 8.5 MB of 2 Mbit/s video for the time the camera was awake (99.995% less).
- **Memory:** peak 280 KB of 512 KB, 0 allocations after start-up.

## Limits of this evaluation

- One labelled clip with one visit: this shows the pipeline works end to end, not a recall figure for the detector in general.
- Runs differ because frame timing on a PC varies; when the man bends and turns away (around 11-16 s of the clip) the detector can lose him and the camera then sleeps early.
- With a 10 s cooldown, a long visit raises one event about every 10 s; a product would likely alert once per visit.
- The detector and NPU timing are simulated on a PC; the 75 ms NPU latency is a modelled value, not a measurement on camera hardware.
- The video baseline is an assumption (2 Mbit/s 1080p H.264 while awake); 24/7 streaming would make the saving larger.

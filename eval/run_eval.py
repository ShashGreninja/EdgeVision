"""Evaluate EdgeVision on labelled clips: does it catch the visit, how fast,
does it stay awake while someone is there, and how much does it upload?

    .venv\\Scripts\\python eval\\run_eval.py            # 3 runs per clip
    .venv\\Scripts\\python eval\\run_eval.py --runs 5

Each run wakes the camera once (as one Ring motion event would) and lets the
firmware decide everything else. A fake Wi-Fi module that is always online
records exactly what is uploaded. A synthetic scene with no people checks for
false events. Writes eval/results.md and eval/results.json.

Clip time vs device time: the virtual sensor delivers one clip frame per
sensor frame at 30 fps, and only while powered, so sensor frame number `seq`
shows clip frame (seq - 1) % frames. Event times are reported in clip time.
"""

from __future__ import annotations

import argparse
import json
import statistics
import sys
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.harness import ROOT, FakeModule, run_firmware  # noqa: E402

EVAL = ROOT / "eval"
SENSOR_FPS = 30


def in_any(t: float, intervals: list[list[float]]) -> bool:
    return any(a <= t <= b for a, b in intervals)


def evaluate_clip(name: str, label: dict, seconds: int, run_index: int) -> dict:
    module = FakeModule(online=True)
    run = run_firmware(["--auto-motion", "100000"], seconds=seconds, clip=str(EVAL / "clips" / name),
                       module=module, log_name=f"eval-{name}-{run_index}")
    module.close()
    d = run.done
    fps, frames, person = label["fps"], label["frames"], label["person"]
    present_s = sum(b - a for a, b in person)

    events = []
    for e in run.events:
        clip_t = ((e["seq"] - 1) % frames) / fps if "seq" in e else None
        correct = e["event"] == "person_detected" and clip_t is not None and in_any(clip_t, person)
        events.append({"event": e["event"], "confidence": e["confidence"], "clip_t": clip_t, "correct": correct})

    first = next((e["clip_t"] for e in events if e["correct"]), None)
    clip_seen_s = min(d["camera"]["awake_ms"] / 1000 * SENSOR_FPS / fps, frames / fps)
    seen_while_present = sum(max(0.0, min(b, clip_seen_s) - a) for a, b in person)
    m, g = d["motion"], d["governor"]
    captured = max(1, d["camera"]["captured"])

    return {
        "clip": name,
        "run": run_index,
        "detected": first is not None,
        "first_event_clip_s": first,
        "false_events": sum(1 for e in events if not e["correct"]),
        "events": events,
        "coverage": seen_while_present / present_s,
        "awake_s": d["camera"]["awake_ms"] / 1000,
        "frames": {
            "captured": d["camera"]["captured"],
            "gated_pct": 100 * m["gated_total"] / captured,
            "npu_pct": 100 * d["npu"]["jobs_total"] / captured,
            "dropped_pct": 100 * g["dropped_total"] / captured,
            "npu_jobs": d["npu"]["jobs_total"],
        },
        "uploaded_bytes": sum(len(p["topic"]) + len(json.dumps(p["payload"], separators=(",", ":"))) for p in module.published),
        "published": len(module.published),
        "bandwidth": d["bandwidth"],
        "late_allocs": d["heap"]["late_allocs"],
        "heap_peak_kb": d["heap"]["peak_kb"],
    }


def evaluate_empty_scene(seconds: int, run_index: int) -> dict:
    module = FakeModule(online=True)
    run = run_firmware(["--auto-motion", "3"], seconds=seconds, module=module, log_name=f"eval-empty-{run_index}")
    module.close()
    return {"run": run_index, "events": len(run.events), "npu_jobs": run.done["npu"]["jobs_total"],
            "published": len(module.published)}


def fmt_s(v):
    return "-" if v is None else f"{v:.1f} s"


def write_report(results: dict) -> str:
    clip_runs = results["clip_runs"]
    empty = results["empty_runs"]
    lines = [
        "# EdgeVision evaluation",
        "",
        f"Generated {results['generated']} by `eval/run_eval.py` ({len(clip_runs)} clip runs, {len(empty)} empty-scene runs).",
        "Model: NanoDet-Plus FP32, emulated NPU latency 75 ms, governor on. One Ring-style wake-up per run.",
        "",
        "## Person visits",
        "",
        "| Run | Visit detected | First event (clip time) | Awake while person present | False events | Frames: motion gate / AI chip / dropped | Uploaded | Video baseline (awake) | Saved |",
        "|---|---|---|---|---|---|---|---|---|",
    ]
    for r in clip_runs:
        f = r["frames"]
        bw = r["bandwidth"]
        lines.append(
            f"| {r['clip']} #{r['run']} | {'yes' if r['detected'] else 'NO'} | {fmt_s(r['first_event_clip_s'])} | "
            f"{100 * r['coverage']:.0f}% | {r['false_events']} | "
            f"{f['gated_pct']:.0f}% / {f['npu_pct']:.0f}% / {f['dropped_pct']:.0f}% of {f['captured']} | "
            f"{r['uploaded_bytes']} B ({r['published']} event{'s' if r['published'] != 1 else ''}) | "
            f"{bw['streaming_baseline_bytes'] / 1e6:.1f} MB | "
            f"{bw['saved_vs_streaming_pct']:.3f}% |"
        )

    s = results["summary"]
    lines += [
        "",
        "## Summary",
        "",
        f"- **Visit recall:** {s['visits_detected']}/{s['visits']} runs raised a correct person event.",
        f"- **Time to first event:** median {fmt_s(s['median_first_event_s'])} of clip time after the person appears.",
        f"- **Stayed awake while the person was there:** median {100 * s['median_coverage']:.0f}% of the visit; "
        f"the whole visit in {s['full_coverage_runs']}/{s['visits']} runs, worst run {100 * s['min_coverage']:.0f}%.",
        f"- **False events:** {s['false_events']} on the clip; {s['empty_scene_events']} on the empty scene "
        f"({s['empty_scene_npu_jobs']} AI-chip checks without a single event).",
        f"- **AI chip load:** median {s['median_npu_pct']:.0f}% of captured frames reached the AI chip; "
        f"{s['median_gated_pct']:.0f}% stopped at the motion gate.",
        f"- **Uploaded:** median {s['median_uploaded_bytes']} bytes per visit, versus "
        f"{s['median_baseline_mb']:.1f} MB of 2 Mbit/s video for the time the camera was awake "
        f"({s['median_saved_pct']:.3f}% less).",
        f"- **Memory:** peak {s['max_heap_kb']} KB of 512 KB, {s['late_allocs']} allocations after start-up.",
        "",
        "## Limits of this evaluation",
        "",
        "- One labelled clip with one visit: this shows the pipeline works end to end, not a recall figure for the detector in general.",
        "- Runs differ because frame timing on a PC varies; when the man bends and turns away (around 11-16 s of the clip) the detector "
        "can lose him and the camera then sleeps early.",
        "- With a 10 s cooldown, a long visit raises one event about every 10 s; a product would likely alert once per visit.",
        "- The detector and NPU timing are simulated on a PC; the 75 ms NPU latency is a modelled value, not a measurement on camera hardware.",
        "- The video baseline is an assumption (2 Mbit/s 1080p H.264 while awake); 24/7 streaming would make the saving larger.",
        "",
    ]
    return "\n".join(lines)


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--runs", type=int, default=3)
    p.add_argument("--seconds", type=int, default=40, help="device seconds per clip run")
    args = p.parse_args()

    labels = json.loads((EVAL / "labels.json").read_text())["clips"]
    clip_runs = []
    for name, label in labels.items():
        if not (EVAL / "clips" / name).exists():
            print(f"skipping {name}: not in eval/clips/")
            continue
        for i in range(1, args.runs + 1):
            r = evaluate_clip(name, label, args.seconds, i)
            print(f"{name} #{i}: detected={r['detected']} first={fmt_s(r['first_event_clip_s'])} "
                  f"coverage={100 * r['coverage']:.0f}% false={r['false_events']} uploaded={r['uploaded_bytes']} B", flush=True)
            clip_runs.append(r)

    empty_runs = []
    for i in range(1, max(1, args.runs // 2) + 1):
        r = evaluate_empty_scene(20, i)
        print(f"empty scene #{i}: events={r['events']} npu_jobs={r['npu_jobs']}", flush=True)
        empty_runs.append(r)

    if not clip_runs:
        raise SystemExit("no clips found in eval/clips/")

    firsts = [r["first_event_clip_s"] for r in clip_runs if r["first_event_clip_s"] is not None]
    summary = {
        "visits": len(clip_runs),
        "visits_detected": sum(r["detected"] for r in clip_runs),
        "median_first_event_s": statistics.median(firsts) if firsts else None,
        "median_coverage": statistics.median(r["coverage"] for r in clip_runs),
        "min_coverage": min(r["coverage"] for r in clip_runs),
        "full_coverage_runs": sum(r["coverage"] >= 0.999 for r in clip_runs),
        "false_events": sum(r["false_events"] for r in clip_runs),
        "empty_scene_events": sum(r["events"] for r in empty_runs),
        "empty_scene_npu_jobs": sum(r["npu_jobs"] for r in empty_runs),
        "median_npu_pct": statistics.median(r["frames"]["npu_pct"] for r in clip_runs),
        "median_gated_pct": statistics.median(r["frames"]["gated_pct"] for r in clip_runs),
        "median_uploaded_bytes": int(statistics.median(r["uploaded_bytes"] for r in clip_runs)),
        "median_baseline_mb": statistics.median(r["bandwidth"]["streaming_baseline_bytes"] for r in clip_runs) / 1e6,
        "median_saved_pct": statistics.median(r["bandwidth"]["saved_vs_streaming_pct"] for r in clip_runs),
        "max_heap_kb": max(r["heap_peak_kb"] for r in clip_runs),
        "late_allocs": sum(r["late_allocs"] for r in clip_runs),
    }
    results = {"generated": datetime.now().strftime("%Y-%m-%d %H:%M"), "clip_runs": clip_runs,
               "empty_runs": empty_runs, "summary": summary}

    (EVAL / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    (EVAL / "results.md").write_text(write_report(results))
    print(f"\nwrote {EVAL / 'results.md'}")


if __name__ == "__main__":
    main()

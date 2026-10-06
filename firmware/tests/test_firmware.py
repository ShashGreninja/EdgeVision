"""Functional tests: run the real firmware through scenarios and check behaviour.

    .venv\\Scripts\\python -m unittest discover -s firmware/tests -p "test_*.py" -v
or  firmware\\test.ps1   (builds first, then runs unit + functional tests)

Each test starts the firmware for a few seconds of its own clock and reads
its final telemetry JSON, its [event] lines and, where a fake Wi-Fi module is
attached, what it published. The full output of every run is kept in
%LOCALAPPDATA%\\edgevision\\build\\test-logs\\<test id>.log. The clip test is
skipped if eval/clips has no clip; the detector tests need the models in
models/.
"""

from __future__ import annotations

import subprocess
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.harness import ROOT, FakeModule, env, free_port, run_firmware, send_udp, unit_test_exe, wait_until  # noqa: E402

CLIP = ROOT / "eval" / "clips" / "testclip.mp4"
HAVE_MODEL = (ROOT / "models" / "object_detection_nanodet_2022nov.onnx").exists()


def fault_at(port: int, seconds: float, name: str):
    return (seconds, lambda: send_udp(port, f"FAULT {name}"))


class FirmwareTest(unittest.TestCase):
    def run_fw(self, args, seconds, **kwargs):
        return run_firmware(args, seconds, log_name=self.id(), **kwargs)

    def module(self, online: bool = True) -> FakeModule:
        m = FakeModule(online=online)
        self.addCleanup(m.close)
        return m


class MemoryAndBoot(FirmwareTest):
    def test_stays_idle_and_within_ram_budget_without_motion(self):
        run = self.run_fw([], seconds=3)
        self.assertEqual(run.returncode, 0)
        d = run.done
        self.assertEqual(d["state"], "idle")
        self.assertEqual(d["camera"]["wakeups"], 0)
        self.assertEqual(d["camera"]["captured"], 0)
        self.assertLessEqual(d["heap"]["peak_kb"], 512)
        self.assertEqual(d["heap"]["late_allocs"], 0)

    def test_frame_pool_unit_tests_pass(self):
        res = subprocess.run([str(unit_test_exe())], env=env(), capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout)
        self.assertIn("0 failures", res.stdout)

    def test_illegal_buffer_moves_are_caught(self):
        for mode in ("--illegal-move", "--illegal-free"):
            res = subprocess.run([str(unit_test_exe()), mode], env=env(), capture_output=True, text=True, timeout=60)
            self.assertEqual(res.returncode, 2, f"{mode}: {res.stdout}")
            self.assertIn("assert failed", res.stdout)


class WakeAndSleep(FirmwareTest):
    def test_motion_wakes_camera_and_quiet_puts_it_back_to_sleep(self):
        run = self.run_fw(["--auto-motion", "60"], seconds=9)
        d = run.done
        self.assertEqual(d["camera"]["wakeups"], 1)
        self.assertEqual(d["state"], "idle")
        self.assertTrue(run.has("[camera] no activity for 5 s"))
        # Awake ~5 s after the single motion event, at ~30 fps.
        self.assertGreater(d["camera"]["captured"], 100)
        self.assertLess(d["camera"]["captured"], 220)

    def test_udp_motion_from_gateway_wakes_camera(self):
        port = free_port()
        run = self.run_fw([], seconds=4, udp_port=port,
                          actions=[(1.5, lambda: send_udp(port, "MOTION ring human test-door"))])
        self.assertEqual(run.done["camera"]["wakeups"], 1)
        self.assertTrue(run.has("motion pin raised (ring human test-door)"))


class Governor(FirmwareTest):
    def test_governor_holds_frame_cost_to_the_budget(self):
        # The synthetic scene always moves, so every admitted frame is a full one.
        run = self.run_fw(["--auto-motion", "2"], seconds=8)
        g = run.done["governor"]
        self.assertGreater(g["dropped_total"], 0)
        self.assertTrue(70 <= g["est_ms"] <= 110, g)
        self.assertLessEqual(run.done["npu"]["jobs"], 13)   # ~1000 ms / 90 ms per frame
        self.assertEqual(run.done["motion"]["busy"], 0)     # nothing lost at random

    def test_without_governor_frames_are_lost_at_random_instead(self):
        run = self.run_fw(["--auto-motion", "2", "--no-governor"], seconds=8)
        self.assertEqual(run.done["governor"]["dropped_total"], 0)
        self.assertGreater(run.done["motion"]["busy"], 0)


@unittest.skipUnless(HAVE_MODEL, "detector model not downloaded")
class Detection(FirmwareTest):
    @unittest.skipUnless(CLIP.exists(), "no eval/clips/testclip.mp4")
    def test_person_on_doorstep_raises_one_person_event(self):
        run = self.run_fw(["--auto-motion", "120"], seconds=10, clip=str(CLIP))
        names = run.event_names()
        self.assertIn("person_detected", names)
        self.assertEqual(names.count("person_detected"), 1, names)  # 10 s cooldown
        self.assertTrue(all(n == "person_detected" for n in names), names)
        self.assertGreaterEqual(run.events[0]["confidence"], 0.5)

    def test_no_person_no_event(self):
        run = self.run_fw(["--auto-motion", "2"], seconds=10)
        self.assertEqual(run.done["events"]["total"], 0)
        self.assertGreater(run.done["npu"]["jobs_total"], 50)  # it did look


class Faults(FirmwareTest):
    def test_corrupt_frames_are_caught_by_crc_and_dropped(self):
        port = free_port()
        run = self.run_fw(["--auto-motion", "2"], seconds=5, udp_port=port, actions=[fault_at(port, 2, "frame_corrupt")])
        self.assertEqual(run.done["camera"]["corrupt"], 5)

    def test_lost_sensor_is_reset_with_backoff_and_recovers(self):
        port = free_port()
        run = self.run_fw(["--auto-motion", "2"], seconds=10, udp_port=port, module=self.module(),
                          actions=[fault_at(port, 2, "camera_disconnect")])
        d = run.done
        self.assertEqual(d["camera"]["sensor_lost"], 1)
        self.assertGreaterEqual(d["camera"]["sensor_resets"], 2)
        self.assertTrue(run.has("[camera] sensor recovered"))
        self.assertGreater(d["camera"]["fps"], 20)  # streaming again at the end

    def test_hung_npu_is_recovered_by_the_watchdog(self):
        port = free_port()
        run = self.run_fw(["--auto-motion", "2"], seconds=9, udp_port=port, module=self.module(),
                          actions=[fault_at(port, 2, "inference_hang")])
        d = run.done
        self.assertTrue(run.has("[watchdog] InferenceTask silent"))
        self.assertEqual(d["npu"]["restarts"], 1)
        self.assertGreater(d["npu"]["jobs"], 0)          # inferring again in the last second
        self.assertEqual(d["heap"]["late_allocs"], 0)    # restart reused its memory

    def test_memory_pressure_degrades_then_restores(self):
        port = free_port()
        module = self.module()
        run = self.run_fw(["--auto-motion", "2"], seconds=10, udp_port=port, module=module,
                          actions=[fault_at(port, 2, "mem_pressure")])
        pools = [t["pool"] for t in module.telemetry]
        self.assertTrue(any("X" in p for p in pools), pools)
        self.assertNotIn("X", run.done["pool"])
        self.assertGreater(run.done["camera"]["fps"], 20)

    def test_cpu_overload_makes_the_governor_drop_more(self):
        port = free_port()
        module = self.module()
        self.run_fw(["--auto-motion", "2"], seconds=12, udp_port=port, module=module,
                    actions=[fault_at(port, 2, "cpu_overload")])
        est = [t["governor"]["est_ms"] for t in module.telemetry]
        self.assertGreater(max(est), 200, est)   # costs x4 during the fault
        self.assertLess(est[-1], 120, est)       # back to normal afterwards


class Network(FirmwareTest):
    def test_events_queue_while_offline_and_flush_in_order(self):
        port = free_port()
        module = self.module(online=False)
        queued_while_offline = []
        actions = [fault_at(port, 1.5 + 0.4 * i, "selftest") for i in range(3)]
        actions.append((4.0, lambda: queued_while_offline.append(module.latest_telemetry()["network"]["queued"])))
        actions.append((4.5, lambda: module.set_online(True)))
        run = self.run_fw([], seconds=8, udp_port=port, module=module, actions=actions)
        self.assertEqual(queued_while_offline, [3])
        self.assertEqual([p["id"] for p in module.published], [1, 2, 3])
        self.assertTrue(all(p["payload"]["event"] == "selftest" for p in module.published))
        self.assertEqual(run.done["network"]["queued"], 0)
        self.assertEqual(run.done["network"]["published"], 3)

    def test_full_store_drops_the_oldest_events(self):
        port = free_port()
        module = self.module(online=False)
        actions = [fault_at(port, 1.0 + 0.25 * i, "selftest") for i in range(18)]
        actions.append((7.0, lambda: module.set_online(True)))
        run = self.run_fw([], seconds=12, udp_port=port, module=module, actions=actions)
        self.assertEqual(run.done["network"]["dropped"], 2)
        self.assertEqual(run.done["network"]["published"], 16)
        self.assertEqual(module.published[0]["id"], 3)  # events 1 and 2 were the oldest

    def test_published_event_matches_the_documented_schema(self):
        port = free_port()
        module = self.module()
        self.run_fw(["--device-id", "cam-test"], seconds=4, udp_port=port, module=module,
                    actions=[fault_at(port, 1.5, "selftest")])
        self.assertTrue(wait_until(lambda: module.published, 1))
        msg = module.published[0]
        self.assertEqual(msg["topic"], "edgevision/cam-test/events")
        for key in ("device", "event", "confidence", "zone", "seq", "box", "uptime_ms", "id"):
            self.assertIn(key, msg["payload"])


if __name__ == "__main__":
    unittest.main()

"""Unit tests for the Ring webhook gateway (python -m unittest discover gateway)."""

import json
import socket
import unittest

from ring_gateway import Dedup, Gateway, motion_command, sign, verify_signature
from send_test_webhook import motion_payload

KEY = "test-signing-key"


class SignatureTests(unittest.TestCase):
    def test_valid_signature_accepted(self):
        body = b'{"a":1}'
        self.assertTrue(verify_signature(KEY, body, sign(KEY, body)))

    def test_wrong_key_rejected(self):
        body = b'{"a":1}'
        self.assertFalse(verify_signature(KEY, body, sign("other", body)))

    def test_reserialised_body_rejected(self):
        # The signature covers the exact bytes; re-serialising JSON changes them.
        body = b'{"a": 1}'
        self.assertFalse(verify_signature(KEY, json.dumps(json.loads(body), separators=(",", ":")).encode(), sign(KEY, body)))

    def test_missing_header_or_key_rejected(self):
        self.assertFalse(verify_signature(KEY, b"{}", None))
        self.assertFalse(verify_signature("", b"{}", sign(KEY, b"{}")))


class PayloadTests(unittest.TestCase):
    def test_motion_event_becomes_command(self):
        cmd = motion_command(motion_payload("door-1", "human"))
        self.assertEqual(cmd, "MOTION ring human door-1")

    def test_other_event_types_ignored(self):
        payload = motion_payload("door-1", "human")
        payload["data"]["type"] = "device_added"
        self.assertIsNone(motion_command(payload))

    def test_dedup(self):
        d = Dedup(capacity=2)
        self.assertTrue(d.first_time("a"))
        self.assertFalse(d.first_time("a"))
        d.first_time("b")
        d.first_time("c")  # evicts "a"
        self.assertTrue(d.first_time("a"))


class GatewayTests(unittest.TestCase):
    def setUp(self):
        self.firmware = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.firmware.bind(("127.0.0.1", 0))
        self.firmware.settimeout(1)
        self.gateway = Gateway(KEY, self.firmware.getsockname()[1], device_filter=None)

    def tearDown(self):
        self.firmware.close()

    def test_signed_motion_reaches_firmware_once(self):
        body = json.dumps(motion_payload("door-1", "human")).encode()
        self.assertEqual(self.gateway.handle(body, sign(KEY, body)), (200, {"status": "processed"}))
        self.assertEqual(self.firmware.recv(256), b"MOTION ring human door-1")
        self.assertEqual(self.gateway.handle(body, sign(KEY, body)), (200, {"status": "duplicate"}))

    def test_bad_signature_never_reaches_firmware(self):
        body = json.dumps(motion_payload("door-1", "human")).encode()
        code, _ = self.gateway.handle(body, sign("wrong", body))
        self.assertEqual(code, 401)
        with self.assertRaises(socket.timeout):
            self.firmware.settimeout(0.2)
            self.firmware.recv(256)


if __name__ == "__main__":
    unittest.main()

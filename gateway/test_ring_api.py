"""Ring API contract and motion forwarding tests; no Ring credentials required."""

import io
import json
import unittest
import urllib.error
from unittest.mock import Mock, patch

from ring_api import RingAPI, RingAPIError, MotionHistory


class RingAPITests(unittest.TestCase):
    def response(self, data):
        response = Mock()
        response.read.return_value = json.dumps(data).encode()
        response.__enter__ = Mock(return_value=response)
        response.__exit__ = Mock(return_value=False)
        return response

    def test_devices_use_documented_auth_headers_and_timeout(self):
        api = RingAPI("private-token", "hardware-uuid")
        with patch.object(api.opener, "open", return_value=self.response({"data": [{"id": "door-1"}]})) as request:
            self.assertEqual(api.devices(), [{"id": "door-1"}])
        req = request.call_args.args[0]
        headers = {key.lower(): value for key, value in req.header_items()}
        self.assertEqual(req.full_url, "https://api.amazonvision.com/v1/devices")
        self.assertEqual(headers["authorization"], "Bearer private-token")
        self.assertEqual(headers["hardware_id"], "hardware-uuid")
        self.assertEqual(headers["user-agent"], "EdgeVision/1.0")
        self.assertEqual(request.call_args.kwargs["timeout"], 15)

    def test_history_device_id_cannot_change_request_path(self):
        api = RingAPI("private-token", "hardware-uuid")
        with patch.object(api.opener, "open", return_value=self.response({"data": []})) as request:
            self.assertEqual(api.history("door/1?token=bad"), [])
        self.assertEqual(request.call_args.args[0].full_url,
                         "https://api.amazonvision.com/v1/history/devices/door%2F1%3Ftoken%3Dbad/events?event_types=motion")

    def test_playground_history_includes_live_view(self):
        api = RingAPI("private-token", "hardware-uuid")
        with patch.object(api.opener, "open", return_value=self.response({"data": []})) as request:
            api.history("door-1", playground=True)
        self.assertTrue(request.call_args.args[0].full_url.endswith("?event_types=motion,on_demand"))

    def test_error_does_not_expose_token_or_response_body(self):
        api = RingAPI("private-token", "hardware-uuid")
        error = urllib.error.HTTPError("https://api.amazonvision.com/v1/devices", 401, "Unauthorized", {}, io.BytesIO(b"private-token"))
        with patch.object(api.opener, "open", side_effect=error):
            with self.assertRaisesRegex(RingAPIError, "expired") as result:
                api.devices()
        self.assertNotIn("private-token", str(result.exception))

    def test_redirects_do_not_forward_credentials(self):
        api = RingAPI("private-token", "hardware-uuid")
        redirect = next(handler for handler in api.opener.handlers
                        if hasattr(handler, "redirect_request"))
        with self.assertRaises(urllib.error.HTTPError) as result:
            redirect.redirect_request(Mock(), None, 302, "Found", {}, "https://other.example/")
        result.exception.close()

    def test_malformed_response_is_reported(self):
        api = RingAPI("private-token", "hardware-uuid")
        with patch.object(api.opener, "open", return_value=self.response({"data": {}})):
            with self.assertRaises(RingAPIError):
                api.devices()


class MotionHistoryTests(unittest.TestCase):
    def event(self, event_id, event_type="motion.human"):
        return {"id": event_id, "attributes": {"event_type": event_type}}

    def test_seeded_history_not_replayed_and_new_motion_forwarded_once(self):
        history = MotionHistory("door-1")
        old = self.event("old")
        history.seed([old])
        events = [self.event("new"), old]
        self.assertEqual(history.commands(events), ["MOTION ring human door-1"])
        self.assertEqual(history.commands(events), [])

    def test_non_motion_and_missing_ids_ignored(self):
        history = MotionHistory("door-1")
        self.assertEqual(history.commands([self.event("ding", "ding"), self.event("")]), [])

    def test_live_view_only_wakes_firmware_in_explicit_playground_mode(self):
        event = self.event("live-view", "on_demand")
        self.assertEqual(MotionHistory("door-1").commands([event]), [])
        history = MotionHistory("door-1", playground=True)
        self.assertEqual(history.commands([event]), ["MOTION ring playground_live_view door-1"])
        self.assertEqual(history.commands([event]), [])


if __name__ == "__main__":
    unittest.main()

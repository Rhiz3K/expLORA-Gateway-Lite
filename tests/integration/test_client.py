"""Host-side regression checks for the integration client (no gateway access)."""

import contextlib
import io
import sys
import unittest
from unittest.mock import Mock, patch

import requests

from gateway_auth import ensure_login
import test_gateway


class ClientTests(unittest.TestCase):
    def test_login_requires_cookie_as_well_as_redirect(self):
        session = requests.Session()
        session.post = Mock(return_value=Mock(status_code=302))
        with self.assertRaises(RuntimeError):
            ensure_login(session, "http://gateway", "test-password")

    def test_login_replaces_expired_cookie(self):
        session = requests.Session()
        session.cookies.set("explora_session", "expired", domain="gateway", path="/")

        def login(*args, **kwargs):
            self.assertEqual(len(session.cookies), 0)
            self.assertFalse(kwargs["allow_redirects"])
            session.cookies.set("explora_session", "fresh", domain="gateway", path="/")
            return Mock(status_code=303)

        session.post = Mock(side_effect=login)
        ensure_login(session, "http://gateway/", "test-password")
        self.assertEqual(session.cookies.get("explora_session"), "fresh")

    def test_failed_login_is_rejected(self):
        session = requests.Session()
        session.post = Mock(return_value=Mock(status_code=401))
        with self.assertRaises(RuntimeError):
            ensure_login(session, "http://gateway", "wrong-password")

    def test_unreachable_gateway_never_provisions_without_opt_in(self):
        cfg = test_gateway.Config(ip="192.0.2.1", admin_password="test-password")
        with patch.object(sys, "argv", ["test_gateway.py", "--config", "unused.ini"]), \
             patch.object(test_gateway, "load_config", return_value=cfg), \
             patch.object(test_gateway, "_http_alive", return_value=False), \
             patch.object(test_gateway, "provision_gateway") as provision, \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(test_gateway.main(), 2)
        provision.assert_not_called()

    def test_manifest_check_waits_for_queued_result(self):
        runner = Mock()
        runner.get.side_effect = [
            Mock(status_code=202),
            Mock(status_code=200, json=lambda: {
                "version": "1.2.1", "currentVersion": "1.2.0", "installable": False,
            }),
        ]
        with patch.object(test_gateway.time, "sleep"):
            self.assertEqual(
                test_gateway.t_firmware_manifest_check(runner),
                "normalized, installable=False",
            )
        self.assertEqual(runner.get.call_count, 2)


if __name__ == "__main__":
    unittest.main()

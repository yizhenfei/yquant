"""Exercise native strategy/gateway processes through isolated CTP SDK APIs.

The fixture supplies broker reports and account query results, with no mock
quote injection into the trading client and no external account connection.
Set YQUANT_BUILD_DIR to a native CMake build to enable process tests.
"""

from contextlib import redirect_stderr, redirect_stdout
from decimal import Decimal
import errno
import io
import json
import os
from pathlib import Path
import signal
import stat
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest import mock


PROJECT = Path(__file__).resolve().parents[2]
BUILD = Path(os.environ.get("YQUANT_BUILD_DIR", PROJECT / "build/debug"))
NATIVE_READY = all((BUILD / "bin" / name).is_file() for name in
                   ("yquant-marketd", "yquant-tradingd", "yquant-strategyd"))
FIXTURE = Path(__file__).with_name("fake_ctp_sdk.py")


class CleanupFailureTest(unittest.TestCase):
    """Cleanup errors must retain failed final manifests and reap owned children."""

    def exercise_failure(self, group_signal=False, read_log=False):
        scripts = str(PROJECT / "scripts")
        if scripts not in sys.path:
            sys.path.insert(0, scripts)
        import run_simnow_strategy as workflow

        helper = '''import pathlib,signal,sys,time
name,run=sys.argv[1],pathlib.Path(sys.argv[2])
stopping=False
def stop(*args):
 global stopping
 stopping=True
signal.signal(signal.SIGTERM,stop)
print('ready',flush=True)
while not stopping:
 if name=='trader_bridge' and (run/'gateway_stopped').exists(): break
 if name=='bridge' and (run/'stop_requested').exists(): break
 time.sleep(.005)
if name=='tradingd': (run/'gateway_stopped').touch()
if name=='trader_bridge': print('ctp_trader_drained',flush=True)
'''
        managers = []
        original_signal = os.killpg
        original_output = workflow.SimNowProcesses.output
        injected = False

        def signal_group(pid, number):
            nonlocal injected
            if group_signal and not injected:
                injected = True
                raise OSError(errno.EIO, "injected group shutdown failure")
            return original_signal(pid, number)

        def output(manager, name):
            if (read_log and name == "trader_bridge" and
                    all(child.poll() is not None for child in manager.children.values())):
                raise OSError(errno.EIO, "injected trader log read failure")
            return original_output(manager, name)

        def run_live(args, run, manifest, manager, *_unused):
            managers.append(manager)
            for name in ("trader_bridge", "bridge", "marketd", "tradingd", "strategyd"):
                manager.start(name, [sys.executable, "-B", "-u", "-c", helper, name, str(run)])
                manager.wait_marker(name, "ready", 3)
            raise workflow.RunError("controlled startup failure")

        with tempfile.TemporaryDirectory(prefix="yquant-cleanup-failure-") as directory:
            run = Path(directory) / "run"
            args = SimpleNamespace(mode="live", run_dir=run, connect_timeout_seconds=0.2)
            stdout, stderr = io.StringIO(), io.StringIO()
            try:
                with (mock.patch.dict(os.environ, {}, clear=True),
                      mock.patch.object(workflow, "parse_arguments", return_value=args),
                      mock.patch.object(workflow, "require_artifacts", return_value=(Path("unused"), Path("unused"))),
                      mock.patch.object(workflow, "prepare", return_value=(run / "instruments.json", None)),
                      mock.patch.object(workflow, "run_live", side_effect=run_live),
                      mock.patch.object(workflow.SimNowProcesses, "output", output),
                      mock.patch.object(os, "killpg", side_effect=signal_group),
                      redirect_stdout(stdout), redirect_stderr(stderr)):
                    result = workflow.main()
            finally:
                for manager in managers:
                    if any(child.poll() is None for child in manager.children.values()):
                        manager.cleanup()
            diagnostics = f"returncode={result}\n{stdout.getvalue()}\n{stderr.getvalue()}"
            manifest = json.loads((run / "manifest.json").read_text())
            self.assertEqual(result, 1, diagnostics)
            self.assertFalse(manifest["success"], diagnostics)
            self.assertIn("process_returncodes", manifest, diagnostics)
            self.assertEqual(set(manifest["process_returncodes"]),
                             {"strategyd", "tradingd", "trader_bridge", "marketd", "bridge"}, diagnostics)
            self.assertTrue(all(code == 0 for code in manifest["process_returncodes"].values()), diagnostics)
            expected = "injected group shutdown failure" if group_signal else "injected trader log read failure"
            self.assertIn(expected, manifest["error"], diagnostics)
            if group_signal:
                self.assertTrue(injected)

    def test_group_shutdown_error_still_reaps_all_children_and_writes_final_manifest(self):
        self.exercise_failure(group_signal=True)

    def test_trader_log_read_error_still_writes_final_manifest(self):
        self.exercise_failure(read_log=True)


@unittest.skipUnless(NATIVE_READY, "set YQUANT_BUILD_DIR to a built native tree")
class SimNowWorkflowTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="yquant-simnow-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        package = self.root / "openctp_ctp"
        package.mkdir()
        (package / "__init__.py").write_text("from . import mdapi, tdapi\n")
        for name in ("mdapi", "tdapi"):
            (package / (name + ".py")).write_text("from fake_ctp_sdk import *\n")
        (self.root / "fake_ctp_sdk.py").write_bytes(FIXTURE.read_bytes())
        self.audit = self.root / "sdk_audit.jsonl"
        self.environment = dict(os.environ, PYTHONPATH=str(self.root),
                                YQUANT_CTP_BROKER_ID="9999", YQUANT_CTP_USER_ID="123456",
                                YQUANT_CTP_PASSWORD="test-simnow-password",
                                YQUANT_FAKE_CTP_AUDIT=str(self.audit))
        for name in ("YQUANT_FAKE_CTP_FAILURE", "YQUANT_FAKE_CTP_ANONYMOUS_MD"):
            self.environment.pop(name, None)

    def arguments(self, run, *extra):
        return [sys.executable, str(PROJECT / "scripts/run_simnow_strategy.py"), "live",
                "--build-dir", str(BUILD), "--run-dir", str(run),
                "--catalog", str(PROJECT / "examples/ctp/instruments.json"),
                "--venue", "DCE", "--symbol", "a2701", "--duration-seconds", "1.2",
                "--connect-timeout-seconds", "3", "--query-interval-seconds", "0.01", *extra]

    def command(self, run, *extra):
        return subprocess.run(self.arguments(run, *extra), cwd=PROJECT,
                              env=self.environment, capture_output=True, text=True, timeout=40)

    def diagnostics(self, result, run):
        output = f"returncode={result.returncode}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        for path in run.glob("*.log"):
            output += "\n" + path.name + ": " + path.read_text(errors="replace")
        return output

    def events(self):
        return [json.loads(line) for line in self.audit.read_text().splitlines()]

    def assert_succeeded(self, result, run):
        diagnostics = self.diagnostics(result, run)
        self.assertEqual(result.returncode, 0, diagnostics)
        manifest = json.loads((run / "manifest.json").read_text())
        self.assertTrue(manifest["success"], diagnostics)
        self.assertEqual(manifest["execution"], "ctp_simnow")
        for field in ("trader_ready", "received_valid_quote", "buy_report_received",
                      "sell_report_received", "trader_drained"):
            self.assertTrue(manifest[field], diagnostics)
        self.assertTrue(all(code == 0 for code in manifest["process_returncodes"].values()), diagnostics)
        self.assertEqual(manifest["shutdown_order"],
                         ["strategyd", "tradingd", "trader_bridge", "marketd", "bridge"])
        self.assertEqual(stat.S_IMODE(run.stat().st_mode), 0o700)
        for path in run.glob("*.json"):
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600, str(path))
            self.assertNotIn("test-simnow-password", path.read_text())
        broker = json.loads((run / "ctp_trading.json").read_text())
        self.assertEqual(broker["adapter"], "ctp")
        self.assertNotIn("initial_cash", broker)
        self.assertNotIn("initial_futures_positions", broker)
        self.assertEqual(len(broker["token"]), 64)
        self.assertNotIn(broker["token"], diagnostics)
        self.assertNotIn(broker["token"], (run / "manifest.json").read_text())
        return manifest

    def test_broker_reports_open_close_cancel_and_authoritative_account(self):
        run = self.root / "live"
        self.assert_succeeded(self.command(run), run)
        events = self.events()
        self.assertTrue({"td_authenticate", "td_login", "td_settlement"} <=
                        {event["event"] for event in events})
        self.assertEqual({event["kind"] for event in events if event["event"] == "td_query"},
                         {"TradingAccount", "InvestorPosition", "InvestorPositionDetail", "Order", "Trade"})
        fills = [event for event in events if event["event"] == "td_fill"]
        self.assertEqual({(event["side"], event["effect"]) for event in fills},
                         {("0", "0"), ("1", "0"), ("0", "1"), ("1", "1")})
        self.assertGreaterEqual(len([event for event in events if event["event"] == "td_cancel"]), 2)
        accounts = [event for event in events if event["event"] == "td_account"]
        self.assertTrue(any(event["long_today"] and event["short_today"] for event in accounts))
        self.assertEqual(accounts[-1]["reserved"], 0)
        expected = int(Decimal(str(accounts[-1]["balance"])) * 1000000)
        output = (run / "strategyd.stdout.log").read_text()
        self.assertRegex(output, rf"balance={expected}\b")
        self.assertIn("commission=5000000", output)
        self.assertIn("futures_bbo: both sides submitted", output)
        for marker in ("reserved=0", "frozen_qty=0", "slots=0"):
            self.assertIn(marker, output)
        released = [event for event in events if event["event"] == "td_release"]
        self.assertEqual(released[-1]["active"], 0)
        self.assertLess(next(i for i, event in enumerate(events) if event["event"] == "td_release"),
                        next(i for i, event in enumerate(events) if event["event"] == "md_release"))

    def test_anonymous_market_data_keeps_authenticated_trader(self):
        self.environment["YQUANT_FAKE_CTP_ANONYMOUS_MD"] = "1"
        run = self.root / "anonymous-md"
        manifest = self.assert_succeeded(self.command(run, "--anonymous-md"), run)
        self.assertEqual(manifest["source"]["auth_mode"], "anonymous")
        self.assertEqual(manifest["trading"]["auth_mode"], "credentials")
        self.assertTrue(next(event for event in self.events() if event["event"] == "md_login")["anonymous"])

    def test_missing_trader_credentials_cannot_use_anonymous_market_data_as_fallback(self):
        self.environment.pop("YQUANT_CTP_USER_ID")
        self.environment.pop("YQUANT_CTP_PASSWORD")
        run = self.root / "missing-account"
        result = self.command(run, "--anonymous-md")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("YQUANT_CTP_USER_ID", result.stderr)
        manifest = json.loads((run / "manifest.json").read_text())
        self.assertFalse(manifest["success"])
        self.assertEqual(manifest["process_returncodes"], {})

    def test_login_failure_cannot_report_success(self):
        self.environment["YQUANT_FAKE_CTP_FAILURE"] = "login"
        run = self.root / "login-failed"
        result = self.command(run)
        self.assertNotEqual(result.returncode, 0)
        manifest = json.loads((run / "manifest.json").read_text())
        self.assertFalse(manifest["success"])
        self.assertFalse(manifest["trader_drained"])
        self.assertTrue(all(code is not None for code in manifest["process_returncodes"].values()))

    def test_unknown_order_send_and_disconnect_fail_with_all_children_reaped(self):
        for failure in ("unknown_send", "disconnect"):
            with self.subTest(failure=failure):
                self.environment["YQUANT_FAKE_CTP_FAILURE"] = failure
                run = self.root / failure
                result = self.command(run)
                diagnostics = self.diagnostics(result, run)
                self.assertNotEqual(result.returncode, 0, diagnostics)
                manifest = json.loads((run / "manifest.json").read_text())
                self.assertIn("process_returncodes", manifest, diagnostics)
                self.assertFalse(manifest["success"])
                self.assertFalse(manifest["trader_drained"])
                self.assertEqual(set(manifest["process_returncodes"]),
                                 {"strategyd", "tradingd", "trader_bridge", "marketd", "bridge"},
                                 diagnostics)
                self.assertTrue(all(code is not None for code in manifest["process_returncodes"].values()),
                                diagnostics)
                self.assertRegex(diagnostics, r"uncertain|unconfirmed|disconnected|timed out")

    def test_only_simnow_endpoints_and_new_run_directories_are_accepted(self):
        run = self.root / "existing"
        run.mkdir()
        sentinel = run / "manifest.json"
        sentinel.write_text("keep")
        self.assertNotEqual(self.command(run).returncode, 0)
        self.assertEqual(sentinel.read_text(), "keep")
        for argument in ("--md-front", "--td-front"):
            result = self.command(self.root / "invalid-endpoint", argument, "tcp://127.0.0.1:1234")
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((self.root / "invalid-endpoint").exists())

    def test_signal_drains_orders_and_reaps_processes(self):
        run = self.root / "signal"
        process = subprocess.Popen(self.arguments(run, "--duration-seconds", "30"), cwd=PROJECT,
                                   env=self.environment, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True)
        try:
            deadline = time.monotonic() + 10
            while True:
                path = run / "trader_bridge.stdout.log"
                if path.exists() and "ctp_trader_sell_accepted" in path.read_text():
                    break
                if process.poll() is not None or time.monotonic() >= deadline:
                    self.fail("workflow never reached confirmed broker orders")
                time.sleep(0.02)
            process.send_signal(signal.SIGTERM)
            stdout, stderr = process.communicate(timeout=30)
        finally:
            if process.poll() is None:
                process.terminate()
                process.communicate(timeout=30)
        self.assertEqual(process.returncode, 143, stdout + stderr)
        manifest = json.loads((run / "manifest.json").read_text())
        self.assertFalse(manifest["success"])
        self.assertTrue(manifest["trader_drained"], stdout + stderr)
        self.assertTrue(all(code == 0 for code in manifest["process_returncodes"].values()), stdout + stderr)


if __name__ == "__main__":
    unittest.main()

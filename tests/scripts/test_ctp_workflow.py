"""Drive native capture/replay processes through an isolated test SDK.

The controlled SDK verifies the live integration boundary without credentials
or an external connection. It does not constitute real-provider acceptance.
Set YQUANT_BUILD_DIR to a built CMake tree to enable these process tests.
"""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


PROJECT = Path(__file__).resolve().parents[2]
BUILD = Path(os.environ.get("YQUANT_BUILD_DIR", PROJECT / "build/debug"))
NATIVE_READY = all((BUILD / "bin" / name).is_file() for name in
                   ("yquant-marketd", "yquant-tradingd", "yquant-strategyd"))

# This module lives only in a temporary PYTHONPATH owned by each process test.
TEST_SDK = '''
from datetime import datetime, timezone, timedelta
import os
import threading
from types import SimpleNamespace

class CThostFtdcMdSpi:
    pass

class CThostFtdcReqUserLoginField:
    pass

class CThostFtdcMdApi:
    @staticmethod
    def CreateFtdcMdApi(*args):
        return CThostFtdcMdApi()

    def __init__(self):
        self.stopping = threading.Event()
        self.thread = None

    def RegisterSpi(self, spi):
        self.spi = spi

    def RegisterFront(self, front):
        pass

    def Init(self):
        self.spi.OnFrontConnected()

    def ReqUserLogin(self, request, request_id):
        fields = (request.BrokerID, request.UserID, request.Password)
        if os.environ.get("YQUANT_FAKE_CTP_AUTH") == "anonymous":
            assert fields == ("", "", "")
        else:
            assert fields == ("test-broker", "test-user", "test-secret")
        code = 7 if os.environ.get("YQUANT_FAKE_CTP_ERROR") == "login" else 0
        self.spi.OnRspUserLogin(SimpleNamespace(TradingDay="20261022"),
                                SimpleNamespace(ErrorID=code), request_id, True)
        return 0

    def SubscribeMarketData(self, symbols, count):
        assert count == 1 and isinstance(symbols[0], bytes)
        self.symbol = symbols[0].decode("ascii")
        self.spi.OnRspSubMarketData(SimpleNamespace(InstrumentID=self.symbol),
                                    SimpleNamespace(ErrorID=0), 2, True)
        self.thread = threading.Thread(target=self.emit, daemon=True)
        self.thread.start()
        return 0

    def emit(self):
        index = 0
        while not self.stopping.wait(0.05):
            now = datetime.now(timezone(timedelta(hours=8)))
            bid = (3662, 3665, 3659, 3663)[index % 4]
            quote = SimpleNamespace(InstrumentID=self.symbol, ExchangeID="DCE",
                ActionDay=now.strftime("%Y%m%d"), TradingDay=now.strftime("%Y%m%d"),
                UpdateTime=now.strftime("%H:%M:%S"),
                UpdateMillisec=now.microsecond // 1000,
                BidPrice1=float(bid), BidVolume1=70,
                AskPrice1=float(bid + 2), AskVolume1=53)
            self.spi.OnRtnDepthMarketData(quote)
            index += 1

    def Release(self):
        self.stopping.set()
        if self.thread is not None:
            self.thread.join(timeout=1)
'''


@unittest.skipUnless(NATIVE_READY, "set YQUANT_BUILD_DIR to a built native tree")
class CtpWorkflowTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="yquant-ctp-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        package = self.root / "openctp_ctp"
        package.mkdir()
        (package / "__init__.py").write_text("from . import mdapi\n")
        (package / "mdapi.py").write_text(TEST_SDK)
        self.environment = dict(os.environ, PYTHONPATH=str(self.root),
                                YQUANT_CTP_BROKER_ID="test-broker",
                                YQUANT_CTP_USER_ID="test-user",
                                YQUANT_CTP_PASSWORD="test-secret")

    def command(self, mode, run, *extra):
        arguments = [sys.executable, str(PROJECT / "scripts/run_ctp_strategy.py"),
                     mode, "--build-dir", str(BUILD), "--run-dir", str(run)]
        if mode != "replay":
            arguments += ["--front", "tcp://127.0.0.1:12345", "--catalog",
                          str(PROJECT / "examples/ctp/instruments.json"),
                          "--venue", "DCE", "--symbol", "a2701",
                          "--duration-seconds", "0.35",
                          "--connect-timeout-seconds", "3"]
        result = subprocess.run(arguments + list(extra), cwd=PROJECT,
                                env=self.environment, capture_output=True,
                                text=True, timeout=25)
        return result

    def assert_succeeded(self, result, run):
        diagnostics = result.stdout + result.stderr
        for log in run.glob("*.stderr.log"):
            diagnostics += "\n" + log.name + ": " + log.read_text(errors="replace")
        self.assertEqual(result.returncode, 0, diagnostics)
        manifest = json.loads((run / "manifest.json").read_text())
        self.assertTrue(manifest["success"], diagnostics)
        self.assertTrue(manifest["received_valid_quote"])
        self.assertTrue(all(value == 0 for value in manifest["process_returncodes"].values()))
        for path in run.glob("*.json"):
            self.assertNotIn("test-secret", path.read_text())
        return manifest

    def test_capture_and_original_speed_replay_finish_and_preserve_values(self):
        captured, replayed = self.root / "captured", self.root / "replayed"
        captured_manifest = self.assert_succeeded(self.command("record", captured), captured)
        self.assertEqual(captured_manifest["source"]["auth_mode"], "credentials")
        manifest = self.assert_succeeded(self.command("replay", replayed,
                                                     "--capture-dir", str(captured)), replayed)
        rows = (replayed / "replay.csv").read_text().splitlines()[1:]
        self.assertGreaterEqual(len(rows), 5)
        self.assertEqual(manifest["replay_metadata"]["rows"], len(rows))
        for row in rows:
            fields = [int(value) for value in row.split(",")]
            self.assertEqual(fields[3], 70)
            self.assertEqual(fields[5], 53)
            self.assertEqual(fields[4] - fields[2], 2)
            self.assertEqual(fields[6], 15)
        output = (replayed / "strategyd.stdout.log").read_text()
        self.assertIn("futures_bbo: stopped", output)
        self.assertIn("futures_bbo: both sides submitted", output)
        self.assertIn("reserved=0", output)
        self.assertIn("frozen_qty=0", output)
        self.assertIn("slots=0", output)

    def test_live_simulation_drains_before_market_and_gateway_stop(self):
        run = self.root / "live"
        manifest = self.assert_succeeded(self.command("live", run), run)
        self.assertEqual(manifest["execution"], "simulated_futures")
        output = (run / "strategyd.stdout.log").read_text()
        self.assertIn("futures_bbo: stopped", output)
        self.assertIn("futures_bbo: both sides submitted", output)
        self.assertIn("reserved=0", output)
        self.assertIn("slots=0", output)
        self.assertGreater(manifest["recorded_events"], 0)

    def test_anonymous_capture_and_replay_need_no_credentials(self):
        for name in ("YQUANT_CTP_BROKER_ID", "YQUANT_CTP_USER_ID", "YQUANT_CTP_PASSWORD"):
            self.environment.pop(name, None)
        self.environment["YQUANT_FAKE_CTP_AUTH"] = "anonymous"
        captured = self.root / "anonymous-capture"
        replayed = self.root / "anonymous-replay"
        manifest = self.assert_succeeded(self.command("record", captured, "--anonymous"), captured)
        self.assertEqual(manifest["source"]["auth_mode"], "anonymous")
        manifest = self.assert_succeeded(self.command("replay", replayed,
                                                     "--capture-dir", str(captured)), replayed)
        self.assertEqual(manifest["source"]["auth_mode"], "anonymous")
        self.assertTrue(manifest["strategy_submitted_quotes"])

    def test_default_login_requires_credentials(self):
        for name in ("YQUANT_CTP_BROKER_ID", "YQUANT_CTP_USER_ID", "YQUANT_CTP_PASSWORD"):
            self.environment.pop(name, None)
        run = self.root / "missing-credentials"
        result = self.command("record", run)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("YQUANT_CTP_BROKER_ID", result.stderr)
        self.assertFalse(json.loads((run / "manifest.json").read_text())["success"])

    def test_login_failure_is_terminal_and_cannot_be_replayed(self):
        self.environment["YQUANT_FAKE_CTP_ERROR"] = "login"
        run = self.root / "failed"
        result = self.command("record", run)
        self.assertNotEqual(result.returncode, 0)
        manifest = json.loads((run / "manifest.json").read_text())
        self.assertFalse(manifest["success"])
        self.assertFalse(manifest["received_valid_quote"])
        replayed = self.root / "failed-replay"
        result = self.command("replay", replayed, "--capture-dir", str(run))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("successful CTP capture", result.stderr)

    def test_existing_directory_is_not_modified(self):
        run = self.root / "existing"
        run.mkdir()
        sentinel = run / "manifest.json"
        sentinel.write_text("keep")
        result = self.command("record", run)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(sentinel.read_text(), "keep")


if __name__ == "__main__":
    unittest.main()

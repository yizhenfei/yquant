"""Exercise the external verifier with controlled process output and exit codes."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

VERIFIER = Path(sys.argv.pop(1)).resolve()


class MultiStrategyVerifierTest(unittest.TestCase):
    def run_probe(self, receipts, *, code=0, remaining=0, duration=1, grouped=True):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            config = root / "monod.conf"
            config.write_text(f"run_seconds={duration}\n" + (
                "strategy.10.11=a.conf\nstrategy.20.11=b.conf\n" if grouped else
                "strategy=a.conf\ncaller_service_id=10\n"))
            output = ("monod_started services=2 strategies=2 account=7\n" + receipts
                      + f"monod_stopped canceled=2 remaining={remaining} commission=0 realized=0\n")
            binary = root / "monod"
            binary.write_text(f"#!{sys.executable}\nprint({output!r}, end='')\nraise SystemExit({code})\n")
            binary.chmod(0o700)
            args = [sys.executable, str(VERIFIER), str(config), "--binary", str(binary)]
            if grouped:
                args.append("--require-all-strategies")
            return subprocess.run(args, capture_output=True, text=True, timeout=10)

    @staticmethod
    def receipt(caller):
        return f"monod_canceled caller={caller} strategy=11 sequence=1 request=1 filled=0\n"

    def test_requires_each_caller_even_when_strategy_ids_match(self):
        result = self.run_probe(self.receipt(10) + self.receipt(20))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("simnow_multi_strategy_verified instances=2", result.stdout)
        result = self.run_probe(self.receipt(10) * 2)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("(20, 11)", result.stderr)

    def test_cannot_pass_on_failed_process_or_remaining_orders(self):
        receipts = self.receipt(10) + self.receipt(20)
        self.assertNotEqual(self.run_probe(receipts, code=1).returncode, 0)
        self.assertNotEqual(self.run_probe(receipts, remaining=1).returncode, 0)

    def test_unbounded_config_rejected_and_legacy_mode_preserved(self):
        result = self.run_probe("", duration=0)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("positive run_seconds", result.stderr)
        self.assertEqual(self.run_probe("", grouped=False).returncode, 0)


if __name__ == "__main__":
    unittest.main()

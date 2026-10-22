"""Offline contract and failure-path tests for the read-only CTP bridge."""

import contextlib
import datetime as dt
import errno
import importlib.util
import io
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock


PROJECT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "ctp_quote_bridge", PROJECT / "scripts/ctp_quote_bridge.py")
bridge = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = bridge
SPEC.loader.exec_module(bridge)


def instrument(decimals=0, tick=1):
    return bridge.Instrument("DCE", "a2701", 0x0000000580004E75,
                             decimals, tick, 1)


def raw_quote(**changes):
    result = dict(InstrumentID="a2701", ExchangeID="DCE",
                  ActionDay="20261023", TradingDay="20261026",
                  UpdateTime="21:01:02", UpdateMillisec=321,
                  BidPrice1=3600.0, BidVolume1=10,
                  AskPrice1=3601.0, AskVolume1=20)
    result.update(changes)
    return result


class FakeApi:
    def __init__(self):
        self.login_result = self.subscription_result = 0
        self.login = self.subscription = None

    def ReqUserLogin(self, request, request_id):
        self.login = request, request_id
        return self.login_result

    def SubscribeMarketData(self, symbols, count):
        self.subscription = symbols, count
        return self.subscription_result


def session():
    api = FakeApi()
    mdapi = SimpleNamespace(CThostFtdcReqUserLoginField=SimpleNamespace)
    state = bridge.Session(mdapi, api, instrument(), 2,
                           ("broker", "user", "sensitive-password"))
    return state, api


class MappingTest(unittest.TestCase):
    def test_stable_venue_and_product_encoding_matches_cpp_literals(self):
        self.assertEqual(bridge.instrument_id("DCE", "a2701"),
                         0x0000000580004E75)
        self.assertEqual(bridge.instrument_id("CZCE", "AP701"),
                         0x00000006800AC2BD)
        self.assertEqual(bridge.instrument_id("CFFEX", "IF2703"),
                         0x00000007803E4E77)
        for venue, symbol in (("DCE", "A2701"), ("DCE", "a2700"),
                              ("DCE", "a2713"), ("CZCE", "AP2701"),
                              ("CZCE", "ap701"), ("SSE", "600000"),
                              ("DCE", "a2701\n"), ("SHFE", "cu主力")):
            with self.subTest(venue=venue, symbol=symbol):
                with self.assertRaises(bridge.BridgeError):
                    bridge.instrument_id(venue, symbol)

    def test_frame_layout_matches_quote_codec_exactly(self):
        quote = bridge.normalize_quote(raw_quote(), instrument(), 2, 17)
        frame = quote.encode()
        self.assertEqual(len(frame), 80)
        self.assertEqual(frame[:8], b"YQQ1\x01\x00\x50\x00")
        self.assertEqual(struct.unpack_from("<I", frame, 8)[0], 2)
        self.assertEqual(frame[12], 15)
        self.assertEqual(frame[13:16], b"\0" * 3)
        self.assertEqual(struct.unpack_from("<QQQqqqq", frame, 16),
                         (0x0000000580004E75, 17, 1792760462321000000,
                          3600, 10, 3601, 20))
        self.assertEqual(frame[72:], b"\0" * 8)

    def test_integer_prices_and_step_constraints(self):
        quote = bridge.normalize_quote(
            raw_quote(BidPrice1=3500.2, AskPrice1=3500.4),
            instrument(decimals=1, tick=2), 1, 1)
        self.assertEqual((quote.bid_price, quote.ask_price), (35002, 35004))
        for price in (3500.21, 3500.3, 1e30):
            with self.subTest(price=price):
                with self.assertRaises(bridge.BridgeError):
                    bridge.normalize_quote(raw_quote(BidPrice1=price),
                                           instrument(1, 2), 1, 1)

    def test_missing_sides_are_zero_and_cannot_trade(self):
        for price, quantity in ((sys.float_info.max, 10), (float("nan"), 10),
                                (float("inf"), 10), (0, 10), (-1, 10),
                                (3600.0, 0)):
            with self.subTest(price=price, quantity=quantity):
                quote = bridge.normalize_quote(
                    raw_quote(BidPrice1=price, BidVolume1=quantity),
                    instrument(), 2, 1)
                self.assertEqual((quote.bid_price, quote.bid_qty), (0, 0))
                self.assertEqual(quote.flags, 10)
        for raw in (raw_quote(BidVolume1=-1), raw_quote(BidVolume1=True),
                    raw_quote(AskVolume1=1.5)):
            with self.assertRaises(bridge.BridgeError):
                bridge.normalize_quote(raw, instrument(), 2, 1)

    def test_locked_and_crossed_snapshots_preserve_prices_without_tradable(self):
        for bid in (3601.0, 3602.0):
            quote = bridge.normalize_quote(raw_quote(BidPrice1=bid),
                                           instrument(), 2, 1)
            self.assertEqual(quote.flags, 11)
            self.assertEqual(quote.bid_price, int(bid))

    def test_event_time_uses_action_day_across_weekend_night_session(self):
        expected = dt.datetime(2026, 10, 23, 13, 1, 2, 321000,
                               tzinfo=dt.timezone.utc)
        actual = bridge.event_time_ns(raw_quote())
        self.assertEqual(actual, int(expected.timestamp()) * 1000000000 + 321000000)
        for changes in ({"ActionDay": ""}, {"ActionDay": "20260230"},
                        {"UpdateTime": "24:00:00"}, {"UpdateMillisec": 1000},
                        {"UpdateMillisec": True}, {"ActionDay": None}):
            quote = bridge.normalize_quote(raw_quote(**changes), instrument(), 2, 1)
            self.assertEqual(quote.event_time_ns, 0)
            self.assertEqual(quote.flags, 7)

    def test_unexpected_instrument_exchange_and_id_ranges_fail(self):
        for changes in ({"InstrumentID": "a2703"}, {"ExchangeID": "SHFE"}):
            with self.assertRaises(bridge.BridgeError):
                bridge.normalize_quote(raw_quote(**changes), instrument(), 2, 1)
        for source, sequence in ((0, 1), (1 << 32, 1), (1, 0), (1, 1 << 64)):
            with self.assertRaises(bridge.BridgeError):
                bridge.normalize_quote(raw_quote(), instrument(), source, sequence)
        quote = bridge.normalize_quote(raw_quote(ExchangeID=""), instrument(), 2, 1)
        self.assertEqual(quote.instrument_id, instrument().instrument_id)

    def test_runtime_rejects_cached_future_and_undated_quote_freshness(self):
        quote = bridge.normalize_quote(raw_quote(), instrument(), 2, 1)
        bound = 5000000000
        for now in (quote.event_time_ns + bound + 1,
                    quote.event_time_ns - bound - 1):
            gated = bridge.freshness_gate(quote, now, bound)
            self.assertEqual(gated.flags, 11)
            self.assertEqual((gated.bid_price, gated.ask_price), (3600, 3601))
            self.assertEqual(gated.event_time_ns, quote.event_time_ns)
        self.assertEqual(bridge.freshness_gate(quote,
                                             quote.event_time_ns + bound,
                                             bound), quote)
        undated = bridge.normalize_quote(raw_quote(ActionDay=""), instrument(), 2, 1)
        self.assertEqual(bridge.freshness_gate(undated, 1, bound).flags, 3)


class CatalogTest(unittest.TestCase):
    def test_shipped_catalog_has_complete_historical_contract(self):
        selected = bridge.load_instrument(PROJECT / "examples/instruments.json",
                                          "DCE", "a1805")
        self.assertEqual(selected.instrument_id, 0x0000000580004AF5)
        self.assertEqual((selected.price_decimals, selected.price_tick,
                          selected.quantity_step), (0, 1, 1))

    def test_duplicate_identity_missing_spec_and_wrong_types_fail(self):
        document = json.loads((PROJECT / "examples/instruments.json").read_text())
        item = next(x for x in document["instruments"] if x["symbol"] == "a1805")
        variants = [{"instruments": [item, item]},
                    {"instruments": [{k: v for k, v in item.items()
                                      if k != "contract_multiplier"}]},
                    {"instruments": [dict(item, price_decimals=True)]},
                    {"instruments": [dict(item, margin_rate_ppm=0)]}]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "catalog.json"
            for variant in variants:
                path.write_text(json.dumps(variant))
                with self.assertRaises(bridge.BridgeError):
                    bridge.load_instrument(path, "DCE", "a1805")
            path.write_text('{"instruments": [], "instruments": []}')
            with self.assertRaisesRegex(bridge.BridgeError, "duplicate"):
                bridge.load_instrument(path, "DCE", "a1805")


class SessionTest(unittest.TestCase):
    def test_handshake_uses_credentials_and_encoded_single_contract(self):
        state, api = session()
        state.process(("connected", ()))
        request, request_id = api.login
        self.assertEqual((request.BrokerID, request.UserID, request.Password),
                         ("broker", "user", "sensitive-password"))
        self.assertEqual(request_id, 1)
        with contextlib.redirect_stdout(io.StringIO()) as output:
            state.process(("login", (0, True, True)))
        self.assertEqual(output.getvalue(), "CTP login succeeded\n")
        self.assertEqual(api.subscription, ([b"a2701"], 1))
        state.process(("subscribed", (0, "a2701", True)))
        self.assertTrue(state.subscribed)
        first = state.process(("quote", (raw_quote(),)))
        second = state.process(("quote", (raw_quote(ActionDay=""),)))
        self.assertEqual((first.sequence, second.sequence), (1, 2))

    def test_login_and_subscription_failures_and_reconnect_are_terminal(self):
        for event in (("login", (3, True, True)),
                      ("login", (0, False, True)),
                      ("subscribed", (7, "a2701", True)),
                      ("subscribed", (0, "a2703", True)),
                      ("quote", (raw_quote(),))):
            state, _ = session()
            with self.assertRaises(bridge.BridgeError):
                state.process(event)
        state, api = session()
        state.process(("connected", ()))
        with self.assertRaisesRegex(bridge.BridgeError, "reconnect"):
            state.process(("connected", ()))
        state, api = session()
        api.login_result = -1
        with self.assertRaisesRegex(bridge.BridgeError, "login request"):
            state.process(("connected", ()))
        state, api = session()
        state.process(("connected", ()))
        api.subscription_result = -3
        with contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(bridge.BridgeError, "subscription request"):
                state.process(("login", (0, True, True)))

    def test_queue_overflow_is_not_a_silent_sequence_gap(self):
        callbacks = bridge.CallbackQueue(1)
        callbacks.push("connected")
        callbacks.push("quote", raw_quote())
        self.assertIn("overflow", callbacks.error)
        self.assertEqual(callbacks.events.qsize(), 1)
        callbacks.fail("later failure")
        self.assertIn("overflow", callbacks.error)

    def test_spi_copies_borrowed_data_and_fails_on_disconnect(self):
        callbacks = bridge.CallbackQueue()
        spi = bridge.create_spi(SimpleNamespace(CThostFtdcMdSpi=object), callbacks)
        borrowed = SimpleNamespace(**raw_quote())
        spi.OnRtnDepthMarketData(borrowed)
        borrowed.BidPrice1 = 999
        event = callbacks.events.get_nowait()
        self.assertEqual(event[1][0]["BidPrice1"], 3600)
        spi.OnFrontDisconnected(4097)
        self.assertIn("4097", callbacks.error)
        spi.OnFrontConnected()
        self.assertTrue(callbacks.events.empty())
        callbacks.closing = True
        spi.OnFrontDisconnected(8)
        self.assertIn("4097", callbacks.error)

    def test_spi_broker_errors_and_malformed_borrowed_data_are_terminal(self):
        for operation, expected in (
                (lambda spi: spi.OnRspError(SimpleNamespace(ErrorID=7), 1, True),
                 "id=7"),
                (lambda spi: spi.OnHeartBeatWarning(30), "heartbeat"),
                (lambda spi: spi.OnRtnDepthMarketData(SimpleNamespace()),
                 "callback failed")):
            callbacks = bridge.CallbackQueue()
            spi = bridge.create_spi(SimpleNamespace(CThostFtdcMdSpi=object), callbacks)
            operation(spi)
            self.assertIn(expected, callbacks.error)

    def test_sequence_exhaustion_does_not_reuse_a_value(self):
        state, _ = session()
        state.logged_in = True
        state.sequence = (1 << 64) - 1
        with self.assertRaisesRegex(bridge.BridgeError, "exhausted"):
            state.process(("quote", (raw_quote(),)))
        self.assertEqual(state.sequence, (1 << 64) - 1)

    def test_first_snapshot_locks_trading_day_without_inventing_event_date(self):
        state, _ = session()
        state.logged_in = True
        first = state.process(("quote", (raw_quote(),)))
        self.assertEqual(state.trading_day, "20261026")
        self.assertEqual(first.event_time_ns, 1792760462321000000)
        with self.assertRaisesRegex(bridge.BridgeError, "TradingDay changed"):
            state.process(("quote", (raw_quote(TradingDay="20261027"),)))
        self.assertEqual(state.trading_day, "20261026")
        self.assertEqual(state.sequence, 1)

    def test_missing_or_invalid_trading_day_is_terminal_before_sequencing(self):
        for day in ("", "20260230", "2026-10-26", "00000000", None, 20261026):
            with self.subTest(day=day):
                state, _ = session()
                state.logged_in = True
                with self.assertRaisesRegex(bridge.BridgeError, "invalid TradingDay"):
                    state.process(("quote", (raw_quote(TradingDay=day),)))
                self.assertIsNone(state.trading_day)
                self.assertEqual(state.sequence, 0)


class TransportTest(unittest.TestCase):
    def test_socket_sends_full_frame_then_first_quote_marker(self):
        state, _ = session()
        state.logged_in = state.subscribed = True
        callbacks = bridge.CallbackQueue()
        callbacks.push("quote", raw_quote())
        left, right = socket.socketpair()
        with left, right, contextlib.redirect_stdout(io.StringIO()) as output:
            left.setblocking(False)
            with mock.patch.object(bridge.time, "time_ns", return_value=1792760462321000000):
                count = bridge.pump(left, state, callbacks, 0, 1, 1, lambda: False)
            frame = right.recv(80)
        self.assertEqual(count, 1)
        self.assertEqual(len(frame), 80)
        self.assertEqual(frame, bridge.normalize_quote(raw_quote(), instrument(), 2, 1).encode())
        self.assertIn("CTP first quote:", output.getvalue())
        self.assertNotIn("sensitive-password", output.getvalue())

    def test_consumer_disconnect_and_unexpected_data_fail(self):
        for disconnect in (False, True):
            state, _ = session()
            callbacks = bridge.CallbackQueue()
            left, right = socket.socketpair()
            with left, right:
                left.setblocking(False)
                if disconnect:
                    right.close()
                else:
                    right.send(b"unexpected")
                with self.assertRaises(bridge.BridgeError):
                    bridge.pump(left, state, callbacks, 0, 0, 1, lambda: False)

    def test_handshake_timeout_and_no_quote_capture_fail(self):
        for subscribed, duration in ((False, 0), (True, 0.001)):
            state, _ = session()
            state.subscribed = subscribed
            left, right = socket.socketpair()
            with left, right:
                left.setblocking(False)
                with self.assertRaises(bridge.BridgeError):
                    bridge.pump(left, state, bridge.CallbackQueue(), duration,
                                0, 0.001, lambda: False)

    def test_partial_writes_retain_frame_bytes(self):
        class ShortWriter:
            def __init__(self):
                self.written = bytearray()

            def send(self, data):
                self.written.extend(data[:3])
                return min(3, len(data))

        state, _ = session()
        state.logged_in = state.subscribed = True
        callbacks = bridge.CallbackQueue()
        callbacks.push("quote", raw_quote())
        writer = ShortWriter()
        with mock.patch.object(bridge.select, "select", return_value=([], [writer], [])):
            with contextlib.redirect_stdout(io.StringIO()), \
                    mock.patch.object(bridge.time, "time_ns", return_value=1792760462321000000):
                bridge.pump(writer, state, callbacks, 0, 1, 1, lambda: False)
        self.assertEqual(bytes(writer.written),
                         bridge.normalize_quote(raw_quote(), instrument(), 2, 1).encode())

    def test_cached_quote_is_recordable_but_does_not_emit_valid_marker(self):
        state, _ = session()
        state.logged_in = state.subscribed = True
        callbacks = bridge.CallbackQueue()
        callbacks.push("quote", raw_quote())
        left, right = socket.socketpair()
        with left, right, contextlib.redirect_stdout(io.StringIO()) as output:
            left.setblocking(False)
            with mock.patch.object(bridge.time, "time_ns", return_value=1792760472321000000):
                with self.assertRaisesRegex(bridge.BridgeError, "fresh two-sided"):
                    bridge.pump(left, state, callbacks, 0, 1, 1, lambda: False)
            frame = right.recv(80)
        self.assertEqual(frame[12], 11)
        self.assertIn("CTP first snapshot", output.getvalue())
        self.assertNotIn("CTP first quote", output.getvalue())

    def test_backpressure_timeout_does_not_drop_or_renumber_frames(self):
        state, _ = session()
        state.logged_in = state.subscribed = True
        callbacks = bridge.CallbackQueue()
        callbacks.push("quote", raw_quote())
        callbacks.push("quote", raw_quote())
        left, right = socket.socketpair()
        with left, right, \
                mock.patch.object(bridge.select, "select", return_value=([], [], [])), \
                mock.patch.object(bridge.time, "monotonic", side_effect=[0, 0, 2]), \
                mock.patch.object(bridge.time, "time_ns", return_value=1792760462321000000):
            with self.assertRaisesRegex(bridge.BridgeError, "backpressure"):
                bridge.pump(left, state, callbacks, 0, 0, 1, lambda: False)
        self.assertEqual(state.sequence, 1)
        self.assertEqual(callbacks.events.qsize(), 1)

    def test_consumer_eof_requires_an_explicit_run_owned_stop_marker(self):
        state, _ = session()
        state.logged_in = state.subscribed = True
        callbacks = bridge.CallbackQueue()
        callbacks.push("quote", raw_quote())

        class ClosingConsumer:
            def recv(self, *_args):
                return b""

            def send(self, frame):
                return len(frame)

        consumer = ClosingConsumer()
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / "stop_requested"
            with self.assertRaisesRegex(bridge.BridgeError, "disconnected"):
                bridge._peer_state(consumer, marker)
            marker.touch()
            with contextlib.redirect_stdout(io.StringIO()), \
                    mock.patch.object(bridge.time, "time_ns", return_value=1792760462321000000), \
                    mock.patch.object(bridge.select, "select", side_effect=[
                        ([], [consumer], []), ([consumer], [], [])]):
                self.assertEqual(bridge.pump(consumer, state, callbacks, 0, 0, 1,
                                             lambda: False, stop_request_file=marker), 1)

    def test_receive_reset_requires_an_explicit_run_owned_stop_marker(self):
        class ResetConsumer:
            def recv(self, *_args):
                raise ConnectionResetError(errno.ECONNRESET, "consumer reset")

            def send(self, frame):
                return len(frame)

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            regular = root / "stop_requested"
            regular.touch()
            not_file = root / "directory"
            not_file.mkdir()
            for marker in (None, root / "missing", not_file, regular):
                with self.subTest(marker=marker):
                    state, _ = session()
                    state.logged_in = state.subscribed = True
                    callbacks = bridge.CallbackQueue()
                    callbacks.push("quote", raw_quote())
                    consumer = ResetConsumer()
                    with contextlib.redirect_stdout(io.StringIO()) as output, \
                            mock.patch.object(bridge.time, "time_ns", return_value=1792760462321000000), \
                            mock.patch.object(bridge.select, "select", side_effect=[
                                ([], [consumer], []), ([consumer], [], [])]):
                        if marker == regular:
                            count = bridge.pump(consumer, state, callbacks, 0, 0, 1,
                                                lambda: False, stop_request_file=marker)
                            self.assertEqual(count, 1)
                            self.assertIn("stopped: quotes=1 fresh_quotes=1", output.getvalue())
                        else:
                            with self.assertRaises(ConnectionResetError):
                                bridge.pump(consumer, state, callbacks, 0, 0, 1,
                                            lambda: False, stop_request_file=marker)

    def test_send_reset_only_allows_requested_stop_and_never_counts_partial_frame(self):
        cases = ((ConnectionResetError, False, False),
                 (ConnectionResetError, True, False),
                 (BrokenPipeError, False, False),
                 (BrokenPipeError, True, False),
                 (OSError, True, False),
                 (ConnectionResetError, True, True))
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / "stop_requested"
            for error_type, requested, upstream_error in cases:
                with self.subTest(error=error_type, requested=requested, upstream=upstream_error):
                    if marker.exists():
                        marker.unlink()
                    if requested:
                        marker.touch()
                    state, _ = session()
                    state.logged_in = state.subscribed = True
                    callbacks = bridge.CallbackQueue()
                    callbacks.push("quote", raw_quote())
                    callbacks.push("quote", raw_quote())

                    class ResetWriter:
                        def __init__(self):
                            self.written = bytearray()

                        def send(self, frame):
                            if not self.written:
                                self.written.extend(frame)
                                return len(frame)
                            if len(self.written) == 80:
                                self.written.extend(frame[:3])
                                return 3
                            if upstream_error:
                                callbacks.fail("upstream disconnected")
                            raise error_type("consumer write failed")

                    writer = ResetWriter()
                    with contextlib.redirect_stdout(io.StringIO()) as output, \
                            mock.patch.object(bridge.time, "time_ns", return_value=1792760462321000000), \
                            mock.patch.object(bridge.select, "select", return_value=([], [writer], [])):
                        if requested and error_type is not OSError and not upstream_error:
                            count = bridge.pump(writer, state, callbacks, 0, 0, 1,
                                                lambda: False, stop_request_file=marker)
                            self.assertEqual(count, 1)
                            self.assertIn("stopped: quotes=1 fresh_quotes=1", output.getvalue())
                        else:
                            expected = bridge.BridgeError if upstream_error else error_type
                            with self.assertRaises(expected):
                                bridge.pump(writer, state, callbacks, 0, 0, 1,
                                            lambda: False, stop_request_file=marker)
                    self.assertEqual(len(writer.written), 83)
                    self.assertEqual(state.sequence, 2)

    def test_requested_reset_does_not_hide_empty_capture_or_other_receive_errors(self):
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / "stop_requested"
            marker.touch()
            consumer = mock.Mock()
            consumer.recv.side_effect = ConnectionResetError(errno.ECONNRESET, "consumer reset")
            state, _ = session()
            state.logged_in = state.subscribed = True
            with mock.patch.object(bridge.select, "select", return_value=([consumer], [], [])):
                with self.assertRaisesRegex(bridge.BridgeError, "fresh two-sided"):
                    bridge.pump(consumer, state, bridge.CallbackQueue(), 0, 0, 1,
                                lambda: False, stop_request_file=marker)
            consumer.recv.side_effect = OSError(errno.EIO, "I/O failure")
            with self.assertRaises(OSError):
                bridge._peer_state(consumer, marker)
            consumer.recv.side_effect = None
            consumer.recv.return_value = b"unexpected"
            with self.assertRaisesRegex(bridge.BridgeError, "sent data"):
                bridge._peer_state(consumer, marker)

    def test_help_works_without_importing_native_sdk(self):
        result = subprocess.run([sys.executable, str(PROJECT / "scripts/ctp_quote_bridge.py"),
                                 "--help"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--front", result.stdout)
        self.assertIn("--catalog", result.stdout)


class LifecycleTest(unittest.TestCase):
    def test_sdk_starts_after_consumer_connection_and_releases_on_every_outcome(self):
        for fail, anonymous in ((False, False), (True, False), (False, True)):
            with self.subTest(disconnect=fail, anonymous=anonymous):
                order = []

                class FakeConnection:
                    def __enter__(self):
                        return self

                    def __exit__(self, *_args):
                        order.append("consumer_closed")

                    def setblocking(self, _value):
                        pass

                    def setsockopt(self, *_args):
                        pass

                    def send(self, frame):
                        order.append("frame_sent")
                        return len(frame)

                connection = FakeConnection()

                class FakeListener(FakeConnection):
                    def bind(self, address):
                        self.address = address

                    def listen(self, backlog):
                        self.backlog = backlog

                    def settimeout(self, _timeout):
                        pass

                    def accept(self):
                        order.append("consumer_connected")
                        return connection, ("127.0.0.1", 23456)

                    def close(self):
                        order.append("listener_closed")

                class NativeApi(FakeApi):
                    def RegisterSpi(self, spi):
                        self.spi = spi

                    def RegisterFront(self, front):
                        self.front = front

                    def Init(self):
                        order.append("sdk_initialized")
                        if fail:
                            self.spi.OnFrontDisconnected(4097)
                        else:
                            self.spi.OnFrontConnected()

                    def ReqUserLogin(self, request, request_id):
                        super().ReqUserLogin(request, request_id)
                        self.spi.OnRspUserLogin(SimpleNamespace(), None, request_id, True)
                        return 0

                    def SubscribeMarketData(self, symbols, count):
                        super().SubscribeMarketData(symbols, count)
                        self.spi.OnRspSubMarketData(SimpleNamespace(InstrumentID="a1805"),
                                                    None, 2, True)
                        self.spi.OnRtnDepthMarketData(SimpleNamespace(
                            **raw_quote(InstrumentID="a1805")))
                        return 0

                    def Release(self):
                        order.append("sdk_released")
                        self.spi.OnFrontDisconnected(99)

                native = NativeApi()
                mdapi = SimpleNamespace(
                    CThostFtdcMdSpi=object,
                    CThostFtdcReqUserLoginField=SimpleNamespace,
                    CThostFtdcMdApi=SimpleNamespace(CreateFtdcMdApi=lambda _flow: native))
                args = bridge.parse_args([
                    "--front", "tcp://broker.example:12345",
                    "--catalog", str(PROJECT / "examples/instruments.json"),
                    "--venue", "DCE", "--symbol", "a1805", "--max-quotes", "1"] +
                    (["--anonymous"] if anonymous else []))
                env = {"YQUANT_CTP_BROKER_ID": "broker", "YQUANT_CTP_USER_ID": "user",
                       "YQUANT_CTP_PASSWORD": "never-print-this"}
                with mock.patch.dict(os.environ, env), \
                        mock.patch.dict(sys.modules, {"openctp_ctp": SimpleNamespace(mdapi=mdapi)}), \
                        mock.patch.object(bridge.socket, "socket", return_value=FakeListener()), \
                        mock.patch.object(bridge.select, "select", return_value=([], [connection], [])), \
                        mock.patch.object(bridge.time, "time_ns", return_value=1792760462321000000), \
                        contextlib.redirect_stdout(io.StringIO()) as output:
                    if fail:
                        with self.assertRaisesRegex(bridge.BridgeError, "4097"):
                            bridge.run(args, lambda: False)
                    else:
                        self.assertEqual(bridge.run(args, lambda: False), 1)
                self.assertLess(order.index("consumer_connected"),
                                order.index("sdk_initialized"))
                self.assertIn("sdk_released", order)
                self.assertNotIn("never-print-this", output.getvalue())
                if not fail:
                    request, _ = native.login
                    self.assertEqual((request.BrokerID, request.UserID, request.Password),
                                     ("", "", "") if anonymous else
                                     ("broker", "user", "never-print-this"))
                    self.assertEqual(native.front, "tcp://broker.example:12345")
                    self.assertEqual(native.subscription, ([b"a1805"], 1))
                    self.assertIn("CTP first quote", output.getvalue())


if __name__ == "__main__":
    unittest.main()

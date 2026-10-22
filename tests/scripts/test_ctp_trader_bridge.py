"""Offline CTP state-machine tests; no trading connection is created."""

from collections import deque
from copy import deepcopy
import os
from pathlib import Path
import socket
import subprocess
import sys
import threading
from types import SimpleNamespace
import unittest
from unittest.mock import patch

SCRIPTS = Path(__file__).resolve().parents[2] / "scripts"
sys.path.insert(0, str(SCRIPTS))
import ctp_trader_bridge as bridge
import ctp_trading_wire as wire
from ctp_quote_bridge import Instrument, instrument_id


class Clock:
    def __init__(self):
        self.now = 0.0

    def __call__(self):
        return self.now

    def advance(self, seconds=1.01):
        self.now += seconds


class FakeSdk:
    """Structures only; no SDK API object or network exists in these tests."""
    def __getattr__(self, name):
        if name.startswith("CThostFtdc"):
            return SimpleNamespace
        raise AttributeError(name)


class FakeApi:
    def __init__(self):
        self.calls = []
        self.returns = {}
        self.on_call = None

    def __getattr__(self, name):
        def invoke(*args):
            self.calls.append((name, deepcopy(vars(args[0])), args[1:]))
            if self.on_call is not None:
                self.on_call(name, *args)
            results = self.returns.get(name, [])
            return results.pop(0) if results else 0
        return invoke


def instrument(venue="SHFE"):
    symbol = "rb2701" if venue == "SHFE" else "TF2612"
    return Instrument(venue, symbol, instrument_id(venue, symbol), 2, 1, 1)


def account_row():
    # The reserved sum is intentionally different from equity-margin-available:
    # a broker-managed snapshot must retain the broker's own monetary values.
    return {"CurrencyID": "CNY", "Balance": 10000.5, "PositionProfit": 20.25,
            "Available": 9000, "CurrMargin": 1000, "FrozenMargin": 50,
            "FrozenCash": 2, "FrozenCommission": 1.5, "CloseProfit": 7,
            "Commission": 3.25}


def empty_rows():
    return {"TradingAccount": [account_row()], "InvestorPosition": [],
            "InvestorPositionDetail": [], "Order": [], "Trade": []}


def session(venue="SHFE", ready=True):
    clock, api = Clock(), FakeApi()
    result = bridge.TraderSession(FakeSdk(), api, instrument(venue), 10,
                                  bridge.Credentials("9999", "123456", "private-password"),
                                  clock=clock)
    if ready:
        result.ready, result.phase = True, "ready"
        result.trading_day, result.front_id, result.session_id = "20261022", 7, -33
        result.order_ref = 100
        result.account = bridge.broker_account([account_row()], [], [], result.instrument,
                                               10, result.trading_day, 1)
        result.revision = 1
    return result, api, clock


def request(s, effect=1, quantity=2):
    return {"instrument_id": s.instrument.instrument_id, "side": 1,
            "limit_price": 10025, "quantity": quantity, "position_effect": effect}


def native_order(s, status="3", traded=0, ref="101", **changes):
    row = {"InstrumentID": s.instrument.symbol, "ExchangeID": s.instrument.venue,
           "FrontID": 7, "SessionID": -33, "OrderRef": "        " + ref,
           "OrderSysID": "         923", "OrderStatus": status,
           "OrderSubmitStatus": "3", "VolumeTraded": traded}
    row.update(changes)
    return row


def native_trade(s, trade_id="t1", volume=1, effect="0", ref="101"):
    return {"TradingDay": "20261022", "ExchangeID": s.instrument.venue,
            "InstrumentID": s.instrument.symbol, "OrderRef": ref,
            "OrderSysID": "         923", "TradeID": trade_id,
            "OffsetFlag": effect, "Direction": "0", "Price": 100.25,
            "Volume": volume, "TradeDate": "20261022", "TradeTime": "13:00:01"}


def finish_query_cycle(s, clock, rows=None):
    rows = empty_rows() if rows is None else rows
    for _ in range(30):
        if s.cycle is None:
            return
        clock.advance()
        s.poll()
        if s.cycle is None:
            return
        inflight = s.cycle["inflight"]
        if inflight is None:
            continue
        kind, rid = inflight
        batch = rows[kind]
        if batch:
            for index, row in enumerate(batch):
                s.handle_event("OnRspQry" + kind, deepcopy(row), None, rid, index == len(batch) - 1)
        else:
            s.handle_event("OnRspQry" + kind, None, None, rid, True)
    raise AssertionError("query cycle did not complete")


class BrokerAccountTests(unittest.TestCase):
    def test_real_broker_money_retained_without_simulator_identity(self):
        s, _, _ = session()
        f = s.account["futures"]
        self.assertEqual(f["equity"], 10000500000)
        self.assertEqual(f["balance"], 9980250000)
        self.assertEqual(f["reserved"], 53500000)
        self.assertEqual(f["realized_pnl"], 7000000)
        self.assertNotEqual(f["available"], f["equity"] - f["margin"] - f["reserved"])
        wire.encode_message({"type": wire.ACCOUNT, "generation": 2,
                             "sequence": 1, "account": s.account})

    def test_real_lot_open_notional_and_exact_shfe_buckets(self):
        ins = instrument()
        positions = [dict(InstrumentID=ins.symbol, ExchangeID="SHFE", Position=2,
                          PosiDirection="2", PositionDate="1", TodayPosition=2,
                          ShortFrozen=1, LongFrozen=0, UseMargin=20, PositionProfit=3),
                     dict(InstrumentID=ins.symbol, ExchangeID="SHFE", Position=3,
                          PosiDirection="3", PositionDate="2", TodayPosition=0,
                          ShortFrozen=0, LongFrozen=2, UseMargin=30, PositionProfit=-1)]
        details = [dict(InstrumentID=ins.symbol, Volume=2, Direction="0",
                        OpenDate="20261022", OpenPrice=100.25),
                   dict(InstrumentID=ins.symbol, Volume=3, Direction="1",
                        OpenDate="20261021", OpenPrice=99.5)]
        a = bridge.broker_account([account_row()], positions, details, ins, 10, "20261022", 9)
        p = a["futures_positions"][0]
        self.assertEqual(p["long_today"], dict(quantity=2, frozen_qty=1, open_notional=2005000000))
        self.assertEqual(p["short_yesterday"], dict(quantity=3, frozen_qty=2, open_notional=2985000000))
        self.assertEqual(p["margin"], 50000000)
        self.assertEqual(p["unrealized_pnl"], 2000000)

    def test_generic_mixed_day_freeze_remains_unallocated(self):
        ins = instrument("CFFEX")
        positions = [dict(InstrumentID=ins.symbol, Position=5, TodayPosition=2,
                          PosiDirection="2", ShortFrozen=3)]
        details = [dict(InstrumentID=ins.symbol, Volume=2, Direction="0", OpenDate="20261022", OpenPrice=100),
                   dict(InstrumentID=ins.symbol, Volume=3, Direction="0", OpenDate="20261021", OpenPrice=90)]
        a = bridge.broker_account([account_row()], positions, details, ins, 10, "20261022", 1)
        p = a["futures_positions"][0]
        self.assertEqual(p["unallocated_long_frozen"], 3)
        self.assertEqual(p["long_today"]["frozen_qty"], 0)
        self.assertEqual(p["long_yesterday"]["frozen_qty"], 0)
        wire.encode_message(dict(type=wire.ACCOUNT, generation=1, sequence=1, account=a))

    def test_other_nonzero_position_or_missing_detail_is_rejected(self):
        ins = instrument()
        with self.assertRaisesRegex(bridge.BridgeError, "expand"):
            bridge.broker_account([account_row()], [dict(InstrumentID="ag2701", Position=1)], [], ins, 10, "20261022", 1)
        row = dict(InstrumentID=ins.symbol, Position=1, PosiDirection="2", PositionDate="1")
        with self.assertRaisesRegex(bridge.BridgeError, "summary/detail"):
            bridge.broker_account([account_row()], [row], [], ins, 10, "20261022", 1)


class SessionTests(unittest.TestCase):
    def test_auth_login_settlement_and_all_queries_gate_readiness(self):
        s, api, clock = session(ready=False)
        s.handle_event("OnFrontConnected")
        self.assertEqual(api.calls[-1][0], "ReqAuthenticate")
        self.assertEqual(api.calls[-1][1]["AppID"], "simnow_client_test")
        with patch.object(sys, "platform", "darwin"):
            s.handle_event("OnRspAuthenticate", {}, None, 1, True)
        self.assertEqual(api.calls[-1][0], "ReqUserLogin")
        self.assertEqual(api.calls[-1][2][-2:], (0, ""))
        s.handle_event("OnRspUserLogin", dict(TradingDay="20261022", FrontID=7,
                                            SessionID=-33, MaxOrderRef="100"), None, 2, True)
        self.assertEqual(api.calls[-1][0], "ReqSettlementInfoConfirm")
        self.assertFalse(s.ready)
        s.handle_event("OnRspSettlementInfoConfirm", {}, None, 3, True)
        finish_query_cycle(s, clock)
        self.assertTrue(s.ready)
        names = [call[0] for call in api.calls[-5:]]
        self.assertEqual(names, ["ReqQry" + name for name in bridge.QUERY_KINDS])

    def test_query_retry_only_for_readonly_local_flow_control(self):
        s, api, clock = session()
        api.returns["ReqQryTradingAccount"] = [-2, 0]
        s._begin_snapshot()
        s.poll()
        self.assertIsNone(s.cycle["inflight"])
        s.poll()
        self.assertEqual(len(api.calls), 1)
        clock.advance()
        s.poll()
        self.assertEqual(s.cycle["inflight"][0], "TradingAccount")
        self.assertEqual(len(api.calls), 2)

    def test_query_waits_for_final_callback_including_empty_final(self):
        s, api, clock = session()
        s._begin_snapshot()
        s.poll()
        kind, rid = s.cycle["inflight"]
        s.handle_event("OnRspQry" + kind, account_row(), None, rid, False)
        clock.advance(5)
        s.poll()
        self.assertEqual(len(api.calls), 1)
        s.handle_event("OnRspQry" + kind, None, None, rid, True)
        s.poll()
        self.assertEqual(api.calls[-1][0], "ReqQryInvestorPosition")

    def test_startup_unowned_working_order_fails(self):
        s, _, clock = session(ready=False)
        s.trading_day, s.front_id, s.session_id = "20261022", 7, -33
        s._begin_snapshot()
        rows = empty_rows()
        rows["Order"] = [native_order(s)]
        with self.assertRaisesRegex(bridge.BridgeError, "unowned active"):
            finish_query_cycle(s, clock, rows)

    def test_limit_flags_and_exchange_specific_generic_close(self):
        for venue, effect, native in (("SHFE", 1, "0"), ("SHFE", 2, "3"),
                                      ("SHFE", 3, "4"), ("CFFEX", 4, "1")):
            with self.subTest(venue=venue, effect=effect):
                s, api, _ = session(venue)
                s.submit(1, request(s, effect))
                fields = api.calls[-1][1]
                self.assertEqual(fields["CombOffsetFlag"], native)
                self.assertEqual(fields["OrderRef"], "101")
                self.assertEqual(fields["LimitPrice"], 100.25)
                self.assertEqual(fields["TimeCondition"], "3")
        for venue, effect in (("SHFE", 4), ("CFFEX", 2), ("CFFEX", 3)):
            s, api, _ = session(venue)
            s.submit(1, request(s, effect))
            self.assertEqual(api.calls, [])
            self.assertEqual(s.pending_reports[-1]["kind"], 1)

    def test_terminal_order_waits_for_unique_trade_details(self):
        s, _, _ = session()
        s.submit(1, request(s))
        s.handle_event("OnRtnOrder", native_order(s, "0", 2))
        self.assertEqual([m["kind"] for m in s.pending_reports], [0])
        self.assertEqual(s.orders[1].state, 1)
        s.handle_event("OnRtnTrade", native_trade(s))
        s.handle_event("OnRtnTrade", native_trade(s, "t2"))
        s.handle_event("OnRtnTrade", native_trade(s, "t2"))
        self.assertEqual([m["kind"] for m in s.pending_reports], [0, 2, 2])
        self.assertEqual(s.orders[1].cumulative, 2)
        self.assertEqual(s.pending_reports[-1]["order"]["state"], 2)

    def test_partial_fill_before_cancellation_preserves_cancel_id(self):
        s, api, _ = session()
        s.submit(1, request(s))
        s.handle_event("OnRtnOrder", native_order(s))
        s.cancel(1, 71)
        self.assertEqual(api.calls[-1][1]["OrderSysID"], "         923")
        s.handle_event("OnRtnOrder", native_order(s, "5", 1))
        self.assertEqual(s.orders[1].state, 1)
        s.handle_event("OnRtnTrade", native_trade(s))
        updates = list(s.pending_reports)
        self.assertEqual([m["kind"] for m in updates], [0, 2, 3])
        self.assertEqual(updates[-1]["cancel_request_id"], 71)
        self.assertEqual(updates[-1]["order"]["cumulative_filled_qty"], 1)
        self.assertEqual(updates[-1]["order"]["leaves_qty"], 0)

    def test_fill_cancel_rejection_race_keeps_filled_state(self):
        s, api, _ = session()
        s.submit(1, request(s, quantity=1))
        s.handle_event("OnRtnOrder", native_order(s))
        s.cancel(1, 41)
        rid = api.calls[-1][2][0]
        s.handle_event("OnRtnTrade", native_trade(s))
        self.assertTrue(s.orders[1].cancel_pending)
        s.handle_event("OnRspOrderAction", dict(OrderRef="101"),
                       dict(ErrorID=26, ErrorMsg="already filled"), rid, True)
        s.handle_event("OnErrRtnOrderAction", dict(OrderRef="101"),
                       dict(ErrorID=26, ErrorMsg="duplicate rejection"))
        self.assertEqual([m["kind"] for m in s.pending_reports], [0, 2, 4])
        final = s.pending_reports[-1]
        self.assertEqual(final["order"]["state"], 2)
        self.assertFalse(final["order"]["cancel_pending"])
        self.assertEqual(final["cancel_request_id"], 41)

    def test_generic_trade_requires_generic_request(self):
        s, _, _ = session("CFFEX")
        s.submit(1, request(s, effect=4, quantity=1))
        s.handle_event("OnRtnOrder", native_order(s))
        s.handle_event("OnRtnTrade", native_trade(s, effect="1"))
        self.assertEqual(s.pending_reports[-1]["fill"]["position_effect"], 4)
        s, _, _ = session()
        s.submit(1, request(s, effect=2))
        s.handle_event("OnRtnOrder", native_order(s))
        with self.assertRaisesRegex(bridge.BridgeError, "offset"):
            s.handle_event("OnRtnTrade", native_trade(s, effect="1"))

    def test_trade_before_order_waits_for_verified_system_identity(self):
        s, _, _ = session()
        s.submit(1, request(s, quantity=1))
        trade = native_trade(s)
        s.handle_event("OnRtnTrade", trade)
        s.handle_event("OnRtnTrade", trade)
        self.assertFalse(s.pending_reports)
        self.assertEqual(len(s.orders[1].delayed_trades), 1)
        s.handle_event("OnRtnOrder", native_order(s, "0", 1))
        self.assertEqual([m["kind"] for m in s.pending_reports], [0, 2])
        self.assertEqual(s.orders[1].state, 2)
        self.assertIsNone(s.orders[1].unresolved_since)

    def test_other_session_same_ref_trade_does_not_update_owned_order(self):
        s, _, _ = session()
        s.submit(1, request(s))
        s.handle_event("OnRtnOrder", native_order(s))
        trade = native_trade(s)
        trade["OrderSysID"] = "       OTHER"
        with self.assertRaisesRegex(bridge.BridgeError, "different broker"):
            s.handle_event("OnRtnTrade", trade)
        self.assertEqual(s.orders[1].cumulative, 0)

    def test_old_cancel_error_cannot_reject_a_new_attempt(self):
        s, api, _ = session()
        s.submit(1, request(s))
        s.handle_event("OnRtnOrder", native_order(s))
        s.cancel(1, 10)
        old_rid = api.calls[-1][2][0]
        error = dict(ErrorID=26, ErrorMsg="cancel rejected")
        s.handle_event("OnRspOrderAction", dict(OrderRef="101"), error, old_rid, True)
        self.assertNotIn(old_rid, s.cancel_requests)
        s.cancel(1, 11)
        new_rid = api.calls[-1][2][0]
        s.handle_event("OnRspOrderAction", dict(OrderRef="101"), error, old_rid, True)
        s.handle_event("OnErrRtnOrderAction", dict(OrderRef="101", OrderActionRef=old_rid), error)
        self.assertTrue(s.orders[1].cancel_pending)
        self.assertEqual(s.orders[1].cancel_id, 11)
        s.handle_event("OnErrRtnOrderAction", dict(OrderRef="101", OrderActionRef=new_rid), error)
        self.assertFalse(s.orders[1].cancel_pending)

    def test_snapshot_restarts_have_one_total_deadline(self):
        s, _, clock = session()
        s._begin_snapshot()
        deadline = s.snapshot_deadline
        clock.advance(30)
        s._changed()
        s._begin_snapshot()
        self.assertEqual(s.snapshot_deadline, deadline)
        clock.advance(31)
        with self.assertRaisesRegex(bridge.BridgeError, "snapshot timed out"):
            s.poll()

    def test_query_not_ready_response_has_bounded_backoff(self):
        s, api, clock = session()
        s._begin_snapshot()
        s.poll()
        rid = s.cycle["inflight"][1]
        s.handle_event("OnRspError", dict(ErrorID=90, ErrorMsg="CTP:查询未就绪，请稍后重试"), rid, True)
        self.assertIsNone(s.cycle["inflight"])
        s.poll()
        self.assertEqual(len(api.calls), 1)
        clock.advance()
        s.poll()
        self.assertEqual(len(api.calls), 2)
        self.assertNotEqual(s.cycle["inflight"][1], rid)

    def test_accept_markers_require_real_queue_or_trade_confirmation(self):
        s, _, _ = session()
        markers = []
        s.marker = markers.append
        s.submit(1, request(s))
        self.assertEqual(markers, [])
        s.handle_event("OnRtnOrder", native_order(s, "a"))
        self.assertEqual(markers, [])
        s.handle_event("OnRtnOrder", native_order(s))
        s.handle_event("OnRtnOrder", native_order(s))
        self.assertEqual(markers, ["ctp_trader_buy_accepted"])

    def test_long_broker_reason_is_explicitly_truncated_on_wire(self):
        s, _, clock = session()
        s.submit(1, request(s))
        s.handle_event("OnRspOrderInsert", dict(OrderRef="101"),
                       dict(ErrorID=31, ErrorMsg="拒绝原因" * 100), 1, True)
        s._begin_snapshot()
        finish_query_cycle(s, clock)
        update = next(m for m in s.messages if m["type"] == wire.ORDER_UPDATE)
        self.assertEqual(len(update["reason"]["text"]), 128)
        self.assertTrue(update["reason"]["truncated"])
        wire.encode_message(dict(update, generation=1, sequence=1))

    def test_send_failure_is_rejected_without_side_effect_retry(self):
        s, api, clock = session()
        api.returns["ReqOrderInsert"] = [-3]
        s.submit(1, request(s))
        self.assertEqual(s.orders[1].state, 4)
        clock.advance()
        s.poll()
        self.assertEqual(sum(call[0] == "ReqOrderInsert" for call in api.calls), 1)

    def test_callback_owns_borrowed_rows_and_overflow_is_terminal(self):
        s, _, _ = session()
        row = SimpleNamespace(Volume=2, InstrumentID="rb2701")
        s.callback("OnRtnTrade", row)
        row.Volume = 99
        self.assertEqual(s.callbacks.get_nowait()[1][0]["Volume"], 2)
        s.callbacks = __import__("queue").Queue(1)
        s.callback("OnFrontConnected")
        s.callback("OnFrontConnected")
        with self.assertRaisesRegex(bridge.BridgeError, "overflow"):
            s.poll()

    def test_stop_waits_for_broker_cancellation_and_snapshot(self):
        s, _, clock = session()
        s.submit(1, request(s))
        s.handle_event("OnRtnOrder", native_order(s))
        s.stop()
        self.assertFalse(s.drained)
        s.handle_event("OnRtnOrder", native_order(s, "5"))
        s.poll()
        finish_query_cycle(s, clock)
        s.poll()
        self.assertTrue(s.drained)
        self.assertEqual(s.pending_reports, deque())
        updates = [m for m in s.messages if m["type"] == wire.ORDER_UPDATE]
        self.assertEqual(updates[-1]["cancel_request_id"], 0)
        self.assertTrue(all(m["account"]["broker_managed"] for m in updates))

    def test_disconnect_and_missing_terminal_trade_never_fake_success(self):
        s, _, clock = session()
        with self.assertRaisesRegex(bridge.BridgeError, "disconnected"):
            s.handle_event("OnFrontDisconnected", 0x1001)
        s.submit(1, request(s))
        s.handle_event("OnRtnOrder", native_order(s, "0", 2))
        s.stop()
        clock.advance(61)
        with self.assertRaisesRegex(bridge.BridgeError, "drain timed out"):
            s.poll()
        self.assertFalse(s.drained)


class EntryPointTests(unittest.TestCase):
    def test_help_does_not_require_sdk_or_credentials(self):
        result = subprocess.run([sys.executable, "-B", str(SCRIPTS / "ctp_trader_bridge.py"), "--help"],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--query-interval-seconds", result.stdout)
        self.assertEqual(result.stderr, "")

    def test_credentials_require_private_user_and_simnow_broker(self):
        with patch.dict(os.environ, {}, clear=True):
            with self.assertRaisesRegex(bridge.BridgeError, "YQUANT_CTP_USER_ID"):
                bridge.Credentials.from_environment()
        with patch.dict(os.environ, {"YQUANT_CTP_BROKER_ID": "1001"}, clear=True):
            with self.assertRaisesRegex(bridge.BridgeError, "9999"):
                bridge.Credentials.from_environment()
        self.assertNotIn("private-password", repr(bridge.Credentials("9999", "123456", "private-password")))


class SocketTests(unittest.TestCase):
    """Exercise the actual framed socket loop using only the fake trader API."""

    def start_server(self, s):
        listener = socket.socket()
        self.addCleanup(listener.close)
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.setblocking(False)
        errors = []
        def run():
            try:
                bridge.serve(listener, s, s.instrument, b"private-token", threading.Event())
            except Exception as error:
                errors.append(error)
        thread = threading.Thread(target=run, daemon=True)
        thread.start()
        client = socket.create_connection(listener.getsockname(), timeout=2)
        client.settimeout(2)
        self.addCleanup(client.close)
        self.addCleanup(thread.join, 2)
        return client, thread, errors

    def receive(self, client):
        def exact(count):
            data = bytearray()
            while len(data) < count:
                chunk = client.recv(count - len(data))
                if not chunk:
                    raise AssertionError("server closed before a complete frame")
                data.extend(chunk)
            return bytes(data)
        header = exact(wire.HEADER_SIZE)
        size = wire.frame_size(header)
        self.assertGreaterEqual(size, wire.HEADER_SIZE)
        return wire.decode_message(header + exact(size - wire.HEADER_SIZE))

    def hello(self, client, s):
        client.sendall(wire.encode_message(dict(type=wire.HELLO, generation=0, sequence=1,
                       client_id=3, token=b"private-token", instruments=[dict(
                           id=s.instrument.instrument_id, price_decimals=s.instrument.price_decimals)])))
        welcome = self.receive(client)
        self.assertEqual(welcome["type"], wire.WELCOME)
        self.assertTrue(welcome["account"]["broker_managed"])
        return welcome["generation"]

    def test_inflight_query_does_not_delay_new_or_cancel(self):
        s, api, _ = session()
        s._begin_snapshot()
        canceled = threading.Event()
        api.on_call = lambda name, *_: canceled.set() if name == "ReqOrderAction" else None
        with patch.object(bridge, "marker", lambda _: None):
            client, thread, errors = self.start_server(s)
            generation = self.hello(client, s)
            client.sendall(wire.encode_message(dict(type=wire.NEW_ORDER, generation=generation,
                           sequence=2, order_id=1, request=request(s))) +
                           wire.encode_message(dict(type=wire.CANCEL_ORDER, generation=generation,
                           sequence=3, order_id=1, cancel_id=8, broker_order_id=b"")))
            self.assertTrue(canceled.wait(1), "cancel was blocked by the in-flight query")
            self.assertEqual(s.cycle["inflight"][0], "TradingAccount")
            self.assertEqual([call[0] for call in api.calls],
                             ["ReqQryTradingAccount", "ReqOrderInsert", "ReqOrderAction"])
            client.close()
            thread.join(2)
        self.assertFalse(thread.is_alive())
        self.assertIsInstance(errors[0], bridge.BridgeError)

    def test_stop_drains_after_a_real_framed_account_refresh(self):
        s, api, _ = session()
        s.clock = __import__("time").monotonic
        s.query_interval = 0.01
        def respond(name, req, rid, *unused):
            if name.startswith("ReqQry"):
                row = SimpleNamespace(**account_row()) if name == "ReqQryTradingAccount" else None
                s.callback("OnRspQry" + name[6:], row, None, rid, True)
        api.on_call = respond
        with patch.object(bridge, "marker", lambda _: None):
            client, thread, errors = self.start_server(s)
            generation = self.hello(client, s)
            client.sendall(wire.encode_message(dict(type=wire.STOP, generation=generation, sequence=2)))
            messages = []
            while not messages or messages[-1]["type"] != wire.DRAINED:
                messages.append(self.receive(client))
            thread.join(2)
        self.assertFalse(thread.is_alive())
        self.assertEqual(errors, [])
        self.assertEqual([m["sequence"] for m in messages], list(range(2, len(messages) + 2)))
        self.assertIn(wire.ACCOUNT, [m["type"] for m in messages])
        self.assertTrue(s.drained)

    def test_stale_client_sequence_is_terminal(self):
        s, _, _ = session()
        with patch.object(bridge, "marker", lambda _: None):
            client, thread, errors = self.start_server(s)
            generation = self.hello(client, s)
            client.sendall(wire.encode_message(dict(type=wire.HEARTBEAT, generation=generation, sequence=3)))
            thread.join(2)
        self.assertFalse(thread.is_alive())
        self.assertRegex(str(errors[0]), "sequence")


if __name__ == "__main__":
    unittest.main()

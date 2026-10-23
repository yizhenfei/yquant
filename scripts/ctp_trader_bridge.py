#!/usr/bin/env python3
"""Connect one local YQuant client to the SimNow simulated CTP trader.

The SDK callback thread only copies bounded events. The owner thread performs
requests, account queries, normalization and nonblocking socket I/O. Broker
queries are authoritative: this bridge does not calculate fees or mark to
market. A lost connection ends the session without resubmitting uncertain
orders. Only the explicit SimNow endpoints below are accepted.
Order reports wait for a completed broker snapshot before publication; queries
are serial and this can delay reports. Once ready, new/cancel/stop requests do
not wait for an in-flight snapshot, so cancellation is not delayed by queries.

API references: https://github.com/Jedore/ctp.examples/tree/main/openctp-ctp
"""

import argparse
from collections import deque
from copy import deepcopy
from dataclasses import dataclass, field
from decimal import Decimal, InvalidOperation, ROUND_HALF_EVEN
import hashlib
import hmac
import json
import os
from pathlib import Path
import queue
import re
import secrets
import select
import signal
import socket
import sys
import tempfile
import threading
import time

from ctp_quote_bridge import BridgeError, INT64_MAX, load_instrument
import ctp_trading_wire as wire


# These are SimNow's published first-set and API-test trader fronts. A
# production broker address cannot be supplied through this simulated bridge.
SIMNOW_FRONTS = frozenset(
    f"tcp://182.254.243.31:{port}" for port in (30001, 30002, 30003, 40001))
CALLBACK_CAPACITY = 4096
MAX_ORDERS = 4096
MAX_QUERY_ROWS = 100000
MAX_TRADE_KEYS = 100000
MAX_OUTGOING_FRAMES = 64
QUERY_KINDS = ("TradingAccount", "InvestorPosition", "InvestorPositionDetail",
               "Order", "Trade")
EFFECT_TO_CTP = {1: "0", 2: "3", 3: "4", 4: "1"}
CTP_TO_EFFECT = {value: key for key, value in EFFECT_TO_CTP.items()}
TERMINAL = frozenset((2, 3, 4))  # Filled, Canceled, Rejected in model/order.h.


def text(value):
    """Copy a native text field, preserving significant leading spaces."""
    if isinstance(value, bytes):
        value = value.decode("gb18030", errors="replace")
    if not isinstance(value, str):
        raise BridgeError("invalid CTP text field")
    return value.rstrip("\0")


def integer(value, name, minimum=0, maximum=INT64_MAX):
    if type(value) is not int or not minimum <= value <= maximum:
        raise BridgeError(f"invalid {name}")
    return value


def decimal(value, name):
    try:
        result = Decimal(str(value))
    except (InvalidOperation, ValueError):
        raise BridgeError(f"invalid {name}") from None
    if not result.is_finite() or abs(result) >= Decimal("1e308"):
        raise BridgeError(f"invalid {name}")
    return result


def money(value, name="broker money"):
    """Round broker currency values to the shared micro-yuan unit."""
    result = int((decimal(value, name) * 1000000).to_integral_value(
        rounding=ROUND_HALF_EVEN))
    if not -INT64_MAX <= result <= INT64_MAX:
        raise BridgeError(f"{name} exceeds the monetary range")
    return result


def price(value, instrument):
    scaled = decimal(value, "trade price") * 10 ** instrument.price_decimals
    rounded = scaled.to_integral_value(rounding=ROUND_HALF_EVEN)
    # Native doubles can contain a small binary representation error.
    if abs(scaled - rounded) > Decimal("0.000001"):
        raise BridgeError("CTP price violates catalog precision")
    result = int(rounded)
    if result <= 0 or result > INT64_MAX or result % instrument.price_tick:
        raise BridgeError("CTP price violates catalog tick")
    return result


def native_copy(value):
    """Own callback fields before CTP reuses its borrowed structure."""
    if value is None:
        return None
    return {name: getattr(value, name) for name in dir(value)
            if name[:1].isupper() and "Password" not in name
            and name not in ("AuthCode",)}


@dataclass(frozen=True)
class Credentials:
    broker_id: str
    user_id: str
    password: str = field(repr=False)
    app_id: str = "simnow_client_test"
    auth_code: str = field(default="0000000000000000", repr=False)

    @classmethod
    def from_environment(cls):
        broker = os.environ.get("YQUANT_CTP_BROKER_ID", "9999")
        user = os.environ.get("YQUANT_CTP_USER_ID", "")
        password = os.environ.get("YQUANT_CTP_PASSWORD", "")
        app = os.environ.get("YQUANT_CTP_APP_ID", "simnow_client_test")
        auth = os.environ.get("YQUANT_CTP_AUTH_CODE", "0000000000000000")
        if broker != "9999":
            raise BridgeError("the SimNow bridge requires BrokerID 9999")
        if not re.fullmatch(r"[0-9]{1,16}", user) or not password:
            raise BridgeError("set YQUANT_CTP_USER_ID and YQUANT_CTP_PASSWORD")
        for name, value, limit in (("password", password, 40),
                                   ("AppID", app, 32), ("AuthCode", auth, 16)):
            if not value or "\0" in value or len(value.encode("ascii")) > limit:
                raise BridgeError(f"invalid SimNow {name}")
        return cls(broker, user, password, app, auth)


def trade_key(row):
    return tuple(text(row.get(name, "")) for name in
                 ("TradingDay", "ExchangeID", "OrderSysID", "TradeID", "Direction"))


def empty_bucket():
    return {"quantity": 0, "frozen_qty": 0, "open_notional": 0}


def broker_account(account_rows, positions, details, instrument, multiplier,
                   trading_day, revision):
    """Build four real position buckets from broker summary and lot detail.

    Missing/ambiguous position detail fails instead of inventing a FIFO book.
    Non-SHFE/INE frozen closes remain direction totals instead of inventing
    day allocation. Other nonzero contracts require a larger dictionary.
    """
    cny = [row for row in account_rows if text(row.get("CurrencyID", "CNY")) == "CNY"]
    if len(cny) != 1:
        raise BridgeError("expected exactly one CNY broker account")
    raw = cny[0]
    equity = money(raw["Balance"])
    upl = money(raw["PositionProfit"])
    futures = {
        "balance": equity - upl, "equity": equity,
        "available": money(raw["Available"]),
        "margin": money(raw["CurrMargin"]),
        "reserved": sum(money(raw.get(key, 0)) for key in
                        ("FrozenMargin", "FrozenCash", "FrozenCommission")),
        "realized_pnl": money(raw["CloseProfit"]),
        "unrealized_pnl": upl, "commission": money(raw["Commission"])}
    if any(futures[key] < 0 for key in ("margin", "reserved", "commission")):
        raise BridgeError("negative broker margin, reservation or commission")
    result = {"instrument_id": instrument.instrument_id,
              **{name: empty_bucket() for name in
                 ("long_today", "long_yesterday", "short_today", "short_yesterday")},
              "margin": 0, "unrealized_pnl": 0,
              "unallocated_long_frozen": 0, "unallocated_short_frozen": 0}
    for row in positions:
        qty = integer(row["Position"], "position quantity")
        frozen = max(integer(row.get("LongFrozen", 0), "long frozen"),
                     integer(row.get("ShortFrozen", 0), "short frozen"))
        if qty == 0 and frozen == 0:
            continue
        if text(row["InstrumentID"]) != instrument.symbol:
            raise BridgeError("account has another position; expand the runtime instrument dictionary")
        if text(row.get("ExchangeID", instrument.venue)) not in ("", instrument.venue):
            raise BridgeError("position exchange differs from the catalog")
        if text(row.get("HedgeFlag", "1")) != "1":
            raise BridgeError("non-speculation positions are not supported")
        direction = text(row["PosiDirection"])
        if direction not in ("2", "3"):
            raise BridgeError("unsupported broker position direction")
        prefix = "long" if direction == "2" else "short"
        frozen = integer(row.get("ShortFrozen" if direction == "2" else "LongFrozen", 0),
                         "position frozen quantity")
        today = integer(row.get("TodayPosition", 0), "today position")
        if instrument.venue in ("SHFE", "INE"):
            date = text(row["PositionDate"])
            if date not in ("1", "2"):
                raise BridgeError("unknown broker position date")
            bucket = result[prefix + ("_today" if date == "1" else "_yesterday")]
            bucket["quantity"] += qty
            bucket["frozen_qty"] += frozen
        else:
            if today > qty:
                raise BridgeError("broker today quantity exceeds total position")
            yesterday = qty - today
            result[prefix + "_today"]["quantity"] += today
            result[prefix + "_yesterday"]["quantity"] += yesterday
            result["unallocated_" + prefix + "_frozen"] += frozen
        result["margin"] += money(row.get("UseMargin", 0))
        result["unrealized_pnl"] += money(row.get("PositionProfit", 0))
    detail_quantities = {name: 0 for name in
                         ("long_today", "long_yesterday", "short_today", "short_yesterday")}
    for row in details:
        qty = integer(row["Volume"], "position detail quantity")
        if not qty:
            continue
        if text(row["InstrumentID"]) != instrument.symbol:
            raise BridgeError("account has another lot detail; expand the runtime instrument dictionary")
        if text(row.get("HedgeFlag", "1")) != "1":
            raise BridgeError("non-speculation position detail is not supported")
        direction = text(row["Direction"])
        if direction not in ("0", "1"):
            raise BridgeError("unknown position detail direction")
        opened = text(row["OpenDate"])
        if not re.fullmatch(r"[0-9]{8}", opened) or opened > trading_day:
            raise BridgeError("invalid lot opening date")
        name = ("long" if direction == "0" else "short") + (
            "_today" if opened == trading_day else "_yesterday")
        detail_quantities[name] += qty
        result[name]["open_notional"] += money(
            decimal(row["OpenPrice"], "lot open price") * qty * multiplier,
            "lot open notional")
    for name, qty in detail_quantities.items():
        bucket = result[name]
        if qty != bucket["quantity"]:
            raise BridgeError("broker position summary/detail changed during the snapshot")
        if bucket["frozen_qty"] > qty or bucket["open_notional"] < 0:
            raise BridgeError("invalid broker position bucket")
    for direction in ("long", "short"):
        total = sum(result[direction + "_" + day]["quantity"] for day in ("today", "yesterday"))
        frozen = sum(result[direction + "_" + day]["frozen_qty"] for day in ("today", "yesterday"))
        if frozen + result["unallocated_" + direction + "_frozen"] > total:
            raise BridgeError("broker frozen quantity exceeds directional holdings")
    return {"revision": revision, "type": 1, "broker_managed": True,
            "futures": futures, "futures_positions": [result]}


@dataclass
class Order:
    client_id: int
    request: dict
    order_ref: str
    broker_id: str
    native_ref: str = ""
    system_id: str = ""
    accepted: bool = False
    cumulative: int = 0
    state: int = 0
    report_sequence: int = 0
    cancel_pending: bool = False
    cancel_id: int = 0
    terminal_row: dict = None
    reported_traded: int = 0
    delayed_trades: list = field(default_factory=list)
    created_at: float = 0.0
    cancel_started: float = 0.0
    cancel_native_id: int = 0
    unresolved_since: float = None


class TraderSession:
    """Owner-thread CTP state machine, independently testable with a fake API."""

    def __init__(self, sdk, api, instrument, multiplier, credentials,
                 query_interval=1.0, timeout=60.0, marker=None, clock=time.monotonic):
        self.sdk, self.api = sdk, api
        self.instrument, self.multiplier = instrument, multiplier
        self.credentials = credentials
        self.query_interval, self.timeout = query_interval, timeout
        self.marker = marker or (lambda _: None)
        self.clock = clock
        self.callbacks = queue.Queue(CALLBACK_CAPACITY)
        self.callback_overflow = threading.Event()
        self.phase = "connecting"
        self.deadline = clock() + timeout
        self.request_id = 0
        self.order_ref = 0
        self.trading_day = ""
        self.front_id = self.session_id = 0
        self.ready = False
        self.stopping = False
        self.drained = False
        self.account = None
        self.revision = 0
        self.orders = {}
        self.refs = {}
        self.cancel_requests = {}
        self.seen_cancel_ids = set()
        self.seen_trades = set()
        self.pending_reports = deque()
        self.messages = deque()
        self.dirty_epoch = 0
        self.cycle = None
        self.last_query = -float("inf")
        self.next_refresh = float("inf")
        self.snapshot_retries = 0
        self.snapshot_deadline = None
        self.accepted_sides = set()

    def callback(self, name, *args):
        """Called by the SDK thread; never waits on network or queue space."""
        try:
            owned = tuple(native_copy(arg) if arg is not None and not isinstance(
                arg, (int, bool, str, bytes)) else arg for arg in args)
            self.callbacks.put_nowait((name, owned))
        except Exception:
            self.callback_overflow.set()

    def _next_request(self):
        self.request_id += 1
        if self.request_id > 2147483647:
            raise BridgeError("CTP request sequence exhausted")
        return self.request_id

    def _request(self, structure, method, fields, login=False):
        req = getattr(self.sdk, structure)()
        for name, value in fields.items():
            setattr(req, name, value)
        request_id = self._next_request()
        args = (req, request_id, 0, "") if login and sys.platform == "darwin" else (req, request_id)
        return getattr(self.api, method)(*args), request_id

    def _identity(self):
        return {"BrokerID": self.credentials.broker_id,
                "InvestorID": self.credentials.user_id}

    def _begin_snapshot(self):
        if self.snapshot_deadline is None:
            self.snapshot_deadline = self.clock() + self.timeout
        self.cycle = {"index": 0, "inflight": None, "rows": {},
                      "epoch": self.dirty_epoch, "started": self.clock(),
                      "retry_count": 0, "next_send": self.clock()}

    def _changed(self):
        self.dirty_epoch += 1
        self.next_refresh = self.clock()

    def poll(self):
        now = self.clock()
        if self.callback_overflow.is_set():
            raise BridgeError("CTP callback queue overflow; broker order state is uncertain")
        for _ in range(CALLBACK_CAPACITY):
            try:
                name, args = self.callbacks.get_nowait()
            except queue.Empty:
                break
            self.handle_event(name, *args)
        if not self.ready and now > self.deadline:
            raise BridgeError("SimNow authentication/account initialization timed out")
        if self.stopping and now > self.deadline and not self.drained:
            raise BridgeError("SimNow drain timed out; orders may remain live")
        for order in self.orders.values():
            if not order.accepted and order.state not in TERMINAL and now - order.created_at > self.timeout:
                raise BridgeError("broker order acknowledgment timed out; order state is uncertain")
            if order.cancel_pending and now - order.cancel_started > self.timeout:
                raise BridgeError("broker cancellation timed out; order state is uncertain")
            if order.unresolved_since is not None and now - order.unresolved_since > self.timeout:
                raise BridgeError("broker terminal/trade details did not reconcile")
        if self.cycle and now > self.snapshot_deadline:
            raise BridgeError("broker snapshot timed out; orders may remain live")
        if not self.cycle and self.ready and now >= self.next_refresh:
            self._begin_snapshot()
        if (self.cycle and self.cycle["inflight"] is None
                and now >= max(self.last_query + self.query_interval, self.cycle["next_send"])):
            self._send_query()
        if self.stopping and self.ready and not self.cycle and not self.pending_reports:
            if all(order.state in TERMINAL and not order.cancel_pending for order in self.orders.values()):
                self.drained = True

    def _send_query(self):
        kind = QUERY_KINDS[self.cycle["index"]]
        ret, rid = self._request("CThostFtdcQry" + kind + "Field", "ReqQry" + kind, self._identity())
        self.last_query = self.clock()
        if ret in (-2, -3):
            self._query_backoff()
            return  # A read-only query has no trading side effect.
        if ret != 0:
            raise BridgeError(f"CTP {kind} query send failed: {ret}")
        self.cycle["rows"][kind] = []
        self.cycle["inflight"] = (kind, rid)

    def _query_backoff(self):
        self.cycle["inflight"] = None
        self.cycle["retry_count"] = min(self.cycle["retry_count"] + 1, 6)
        self.cycle["next_send"] = self.clock() + min(
            8.0, max(self.query_interval, 0.1) * 2 ** (self.cycle["retry_count"] - 1))

    @staticmethod
    def _query_not_ready(info):
        diagnostic = text((info or {}).get("ErrorMsg", "")).lower()
        return "查询未就绪" in diagnostic or "query not ready" in diagnostic

    def handle_event(self, name, *args):
        """Consume already owned callback values on the owner thread."""
        c = self.credentials
        if name == "OnFrontDisconnected":
            raise BridgeError("SimNow trader disconnected; orders may remain live")
        if name == "OnFrontConnected":
            if self.phase != "connecting":
                raise BridgeError("unexpected SimNow reconnect; reconciliation is required")
            ret, _ = self._request("CThostFtdcReqAuthenticateField", "ReqAuthenticate",
                                   {"BrokerID": c.broker_id, "UserID": c.user_id,
                                    "AppID": c.app_id, "AuthCode": c.auth_code})
            if ret:
                raise BridgeError(f"SimNow authentication send failed: {ret}")
            self.phase = "authenticating"
            return
        if name in ("OnRspAuthenticate", "OnRspUserLogin", "OnRspSettlementInfoConfirm"):
            row, info, _, last = args
            if info and info.get("ErrorID", 0):
                raise BridgeError(f"SimNow {self.phase} failed: {info['ErrorID']}")
            if not last:
                return
            if name == "OnRspAuthenticate":
                if self.phase != "authenticating":
                    raise BridgeError("unexpected authentication response")
                ret, _ = self._request("CThostFtdcReqUserLoginField", "ReqUserLogin",
                                       {"BrokerID": c.broker_id, "UserID": c.user_id,
                                        "Password": c.password}, login=True)
                self.phase = "logging_in"
            elif name == "OnRspUserLogin":
                if self.phase != "logging_in" or not row:
                    raise BridgeError("unexpected or empty login response")
                self.trading_day = text(row["TradingDay"])
                if not re.fullmatch(r"[0-9]{8}", self.trading_day):
                    raise BridgeError("invalid CTP trading day")
                self.front_id = integer(row["FrontID"], "front ID", 1)
                self.session_id = integer(row["SessionID"], "session ID", -2147483648, 2147483647)
                ref = text(row.get("MaxOrderRef", "")).strip() or "0"
                if not ref.isascii() or not ref.isdigit():
                    raise BridgeError("invalid CTP MaxOrderRef")
                self.order_ref = int(ref)
                ret, _ = self._request("CThostFtdcSettlementInfoConfirmField", "ReqSettlementInfoConfirm", self._identity())
                self.phase = "confirming_settlement"
            else:
                if self.phase != "confirming_settlement":
                    raise BridgeError("unexpected settlement response")
                self.phase = "querying"
                self._begin_snapshot()
                return
            if ret:
                raise BridgeError(f"SimNow {self.phase} send failed: {ret}")
            return
        if name.startswith("OnRspQry"):
            self._query_response(name[8:], *args)
            return
        if name == "OnRspError":
            info, rid, _ = args
            if self.cycle and self.cycle["inflight"] and self.cycle["inflight"][1] == rid:
                if self._query_not_ready(info):
                    self._query_backoff()
                    return
                raise BridgeError(f"CTP query failed: {(info or {}).get('ErrorID', -1)}")
            raise BridgeError(f"CTP asynchronous error: {(info or {}).get('ErrorID', -1)}")
        if name == "OnRtnOrder":
            self._on_order(args[0])
        elif name == "OnRtnTrade":
            if self.ready:
                self._on_trade(args[0])
            else:
                self._changed()
        elif name in ("OnRspOrderInsert", "OnErrRtnOrderInsert"):
            row, info = args[:2]
            if info and info.get("ErrorID", 0):
                order = self._find_order(row)
                if order is None:
                    raise BridgeError("rejection for an unowned broker order")
                self._reject(order, info["ErrorID"], text(info.get("ErrorMsg", "CTP rejected order")))
        elif name in ("OnRspOrderAction", "OnErrRtnOrderAction"):
            row, info = args[:2]
            if info and info.get("ErrorID", 0):
                rid = args[2] if name == "OnRspOrderAction" else (row or {}).get("OrderActionRef", 0)
                identity = self.cancel_requests.get(rid)
                if identity is None:
                    # A duplicate error for an old attempt cannot reject a
                    # newer cancellation of the same original order.
                    order = self._find_order(row)
                    if not rid and order is not None and order.cancel_pending:
                        raise BridgeError("uncorrelated broker cancel error; cancellation is uncertain")
                    return
                order = self.orders[identity]
                if order.cancel_pending and order.cancel_native_id == rid:
                    cancel_id = self._clear_cancel(order)
                    self._emit(order, 4, cancel_id=cancel_id, code=info["ErrorID"],
                               reason=text(info.get("ErrorMsg", "CTP rejected cancellation")))

    def _query_response(self, kind, row, info, rid, last):
        if not self.cycle or self.cycle["inflight"] != (kind, rid):
            raise BridgeError("unsolicited or mismatched CTP query response")
        if info and info.get("ErrorID", 0):
            if self._query_not_ready(info):
                self._query_backoff()
                return
            raise BridgeError(f"CTP {kind} query failed: {info['ErrorID']}")
        if row is not None:
            rows = self.cycle["rows"][kind]
            if len(rows) >= MAX_QUERY_ROWS:
                raise BridgeError("broker query row limit exceeded")
            rows.append(row)
        if last:
            self.cycle["inflight"] = None
            self.cycle["retry_count"] = 0
            self.cycle["index"] += 1
            if self.cycle["index"] == len(QUERY_KINDS):
                self._finish_snapshot()

    def _finish_snapshot(self):
        cycle = self.cycle
        rows = cycle["rows"]
        for row in rows["Order"]:
            self._on_order(row, queried=True)
        if self.ready:
            for row in rows["Trade"]:
                if trade_key(row) not in self.seen_trades:
                    self._on_trade(row)
        # A report received during this query cycle may be newer than its
        # account query. Refresh again before attaching a full view, retaining
        # the original snapshot deadline across retries.
        if cycle["epoch"] != self.dirty_epoch:
            self._begin_snapshot()
            return
        try:
            account = broker_account(rows["TradingAccount"], rows["InvestorPosition"],
                                     rows["InvestorPositionDetail"], self.instrument,
                                     self.multiplier, self.trading_day, self.revision + 1)
        except BridgeError as error:
            if "changed during" in str(error) and self.snapshot_retries < 3:
                self.snapshot_retries += 1
                self._begin_snapshot()
                return
            raise
        self.snapshot_retries = 0
        if not self.ready:
            self.seen_trades.update(trade_key(row) for row in rows["Trade"])
            if len(self.seen_trades) > MAX_TRADE_KEYS:
                raise BridgeError("broker trade history exceeds the session bound")
        self.account = account
        self.revision = account["revision"]
        self.cycle = None
        self.snapshot_deadline = None
        self.next_refresh = self.clock() + self.query_interval
        while self.pending_reports:
            message = self.pending_reports.popleft()
            message["account"] = deepcopy(account)
            self.messages.append(message)
        if not self.ready:
            self.ready = True
            self.phase = "ready"
            self.marker(f"ctp_trader_ready trading_day={self.trading_day}")
        else:
            self.messages.append({"type": wire.ACCOUNT, "account": deepcopy(account)})

    def _find_order(self, row):
        if not row:
            return None
        ref = text(row.get("OrderRef", "")).strip()
        order = self.refs.get(ref)
        if order is None:
            return None
        if "FrontID" in row and (row["FrontID"] != self.front_id or row["SessionID"] != self.session_id):
            return None
        return order

    def _ensure_accepted(self, order, market_confirmed=False):
        if not order.accepted and order.state not in TERMINAL:
            order.accepted, order.state = True, 1
            self._emit(order, 0)
        side = order.request["side"]
        if market_confirmed and side not in self.accepted_sides:
            self.accepted_sides.add(side)
            self.marker("ctp_trader_buy_accepted" if side == 1 else "ctp_trader_sell_accepted")

    def _on_order(self, row, queried=False):
        if row is None:
            return
        if text(row.get("InstrumentID", "")) != self.instrument.symbol:
            raise BridgeError("broker order contract differs from the submitted order")
        if text(row.get("ExchangeID", "")) != self.instrument.venue:
            raise BridgeError("broker order exchange differs from the submitted order")
        status = text(row["OrderStatus"])
        order = self._find_order(row)
        if order is None:
            if status not in ("0", "2", "4", "5"):
                raise BridgeError("account has an unowned active order; reconcile it before starting")
            return
        order.native_ref = text(row["OrderRef"])
        system_id = text(row.get("OrderSysID", ""))
        if system_id.strip():
            if order.system_id and order.system_id != system_id:
                raise BridgeError("broker order system ID changed")
            order.system_id = system_id
            delayed, order.delayed_trades = order.delayed_trades, []
            for trade in delayed:
                self._on_trade(trade)
            if order.terminal_row is None or order.cumulative >= order.reported_traded:
                order.unresolved_since = None
        traded = integer(row.get("VolumeTraded", 0), "broker cumulative fill")
        if traded > order.request["quantity"]:
            raise BridgeError("broker cumulative fill exceeds order quantity")
        order.reported_traded = max(order.reported_traded, traded)
        if text(row.get("OrderSubmitStatus", "")) == "4":
            if order.state != 4:
                self._reject(order, -1, text(row.get("StatusMsg", "CTP order rejected")))
            return
        if status == "a":
            return
        if status not in ("0", "1", "2", "3", "4", "5"):
            raise BridgeError("unsupported CTP order status")
        if order.state not in TERMINAL:
            self._ensure_accepted(order, market_confirmed=status in ("0", "1", "3"))
        if status in ("0", "2", "4", "5"):
            order.terminal_row = row
            if order.cumulative < order.reported_traded and order.unresolved_since is None:
                order.unresolved_since = self.clock()
            self._finish_terminal(order)
        if not queried:
            self._changed()

    def _finish_terminal(self, order):
        row = order.terminal_row
        if row is None or order.cumulative < order.reported_traded:
            return
        order.unresolved_since = None
        status = text(row["OrderStatus"])
        if status == "0":
            if order.cumulative != order.request["quantity"]:
                return  # Await unique trade details, never invent fills.
            order.state = 2
            return
        if order.state in (3, 4):
            return
        if order.cumulative == order.request["quantity"]:
            order.state = 2
            return
        cancel_id = self._clear_cancel(order)
        order.state = 3
        self._emit(order, 3, cancel_id=cancel_id)

    def _on_trade(self, row):
        key = trade_key(row)
        if key in self.seen_trades:
            return
        if len(self.seen_trades) >= MAX_TRADE_KEYS:
            raise BridgeError("CTP trade identity cache exhausted")
        order = self._find_order(row)
        if order is None:
            raise BridgeError("unowned trade changed the broker account")
        if text(row.get("InstrumentID", "")) != self.instrument.symbol:
            raise BridgeError("trade contract differs from the submitted order")
        if text(row.get("ExchangeID", "")) != self.instrument.venue:
            raise BridgeError("trade exchange differs from the submitted order")
        system_id = text(row.get("OrderSysID", ""))
        if not system_id.strip():
            raise BridgeError("trade has no broker system identity")
        if not order.system_id:
            if key not in {trade_key(trade) for trade in order.delayed_trades}:
                if len(order.delayed_trades) >= CALLBACK_CAPACITY:
                    raise BridgeError("unresolved trade identity queue exceeded its bound")
                order.delayed_trades.append(deepcopy(row))
                if order.unresolved_since is None:
                    order.unresolved_since = self.clock()
                self._changed()
            return  # TradeField lacks FrontID/SessionID: await OnRtnOrder.
        if system_id != order.system_id:
            raise BridgeError("trade belongs to a different broker system order")
        effect = CTP_TO_EFFECT.get(text(row["OffsetFlag"]))
        if effect is None or effect != order.request["position_effect"]:
            raise BridgeError("CTP trade offset differs from the explicit submitted effect")
        side = {"0": 1, "1": 2}.get(text(row["Direction"]))
        if side != order.request["side"]:
            raise BridgeError("CTP trade direction differs from the submitted order")
        quantity = integer(row["Volume"], "trade volume", 1)
        if order.cumulative + quantity > order.request["quantity"]:
            raise BridgeError("unique trades exceed submitted quantity")
        if order.state in (3, 4):
            raise BridgeError("new trade arrived after a reconciled terminal state")
        self._ensure_accepted(order, market_confirmed=True)
        self.seen_trades.add(key)
        order.cumulative += quantity
        order.state = 2 if order.cumulative == order.request["quantity"] else 1
        execution = hashlib.sha256("\0".join(key).encode("utf-8")).hexdigest()
        fill = {"client_order_id": order.client_id, "adapter_order_id": order.broker_id,
                "execution_id": execution, "instrument_id": self.instrument.instrument_id,
                "side": side, "price": price(row["Price"], self.instrument),
                "quantity": quantity, "event_time_ns": 0,
                "receive_time_ns": time.monotonic_ns(), "has_event_time": False,
                "position_effect": effect}
        self._emit(order, 2, fill=fill)
        self._finish_terminal(order)

    def _snapshot(self, order):
        return {"client_order_id": order.client_id, "adapter_order_id": order.broker_id,
                "instrument_id": self.instrument.instrument_id,
                "side": order.request["side"], "limit_price": order.request["limit_price"],
                "original_qty": order.request["quantity"],
                "cumulative_filled_qty": order.cumulative,
                "leaves_qty": 0 if order.state in TERMINAL else order.request["quantity"] - order.cumulative,
                "state": order.state, "cancel_pending": order.cancel_pending,
                "cancel_request_id": order.cancel_id, "report_sequence": order.report_sequence,
                "receive_time_ns": time.monotonic_ns(),
                "position_effect": order.request["position_effect"]}

    def _emit(self, order, kind, cancel_id=0, code=0, reason="", fill=None):
        if len(self.pending_reports) >= CALLBACK_CAPACITY:
            raise BridgeError("broker report backlog exceeded its bound")
        order.report_sequence += 1
        reason_bytes = reason.encode("utf-8")
        message = {"type": wire.ORDER_UPDATE, "notify_order": True,
                   "order": self._snapshot(order), "kind": kind,
                   "cancel_request_id": cancel_id,
                   "reason": {"code": code, "text": reason_bytes[:128],
                              "truncated": len(reason_bytes) > 128}}
        if fill is not None:
            message["fill"] = fill
        self.pending_reports.append(message)
        self._changed()

    def _reject(self, order, code, reason):
        if order.state == 4:
            return
        if order.cumulative:
            raise BridgeError("CTP rejected an order with confirmed executions")
        order.state = 4
        cancel_id = self._clear_cancel(order)
        self._emit(order, 1, code=code, reason=reason)
        if cancel_id:
            self._emit(order, 4, cancel_id=cancel_id, code=code, reason=reason)

    def submit(self, client_id, request):
        if not self.ready or self.stopping:
            raise BridgeError("new order outside the ready simulated session")
        if client_id in self.orders or len(self.orders) >= MAX_ORDERS:
            raise BridgeError("duplicate order ID or order capacity exhausted")
        self.order_ref += 1
        ref = str(self.order_ref)
        if len(ref) > 12:
            raise BridgeError("CTP order reference exhausted")
        broker_id = f"{self.trading_day}/{self.front_id}/{self.session_id}/{ref}"
        order = Order(client_id, deepcopy(request), ref, broker_id, created_at=self.clock())
        self.orders[client_id], self.refs[ref] = order, order
        effect = request["position_effect"]
        supported = (1, 2, 3) if self.instrument.venue in ("SHFE", "INE") else (1, 4)
        error = None
        if request["instrument_id"] != self.instrument.instrument_id:
            error = "unconfigured instrument"
        elif effect not in supported:
            error = "position effect is not supported by this exchange"
        elif request["quantity"] % self.instrument.quantity_step or request["limit_price"] % self.instrument.price_tick:
            error = "order violates catalog price or quantity step"
        if error:
            self._reject(order, -1, error)
            return
        fields = {**self._identity(), "UserID": self.credentials.user_id,
                  "ExchangeID": self.instrument.venue, "InstrumentID": self.instrument.symbol,
                  "OrderRef": ref, "Direction": "0" if request["side"] == 1 else "1",
                  "CombOffsetFlag": EFFECT_TO_CTP[effect], "CombHedgeFlag": "1",
                  "OrderPriceType": "2", "LimitPrice": request["limit_price"] / 10 ** self.instrument.price_decimals,
                  "VolumeTotalOriginal": request["quantity"], "TimeCondition": "3",
                  "VolumeCondition": "1", "MinVolume": 1, "ContingentCondition": "1",
                  "StopPrice": 0.0, "ForceCloseReason": "0", "IsAutoSuspend": 0,
                  "IsSwapOrder": 0, "UserForceClose": 0}
        ret, _ = self._request("CThostFtdcInputOrderField", "ReqOrderInsert", fields)
        if ret:
            self._reject(order, ret, "CTP order request was not sent")

    def cancel(self, client_id, cancel_id, broker_id=b""):
        order = self.orders.get(client_id)
        if order is None:
            raise BridgeError("cancel references an unknown local order")
        if cancel_id:
            if cancel_id in self.seen_cancel_ids or len(self.seen_cancel_ids) >= MAX_ORDERS:
                raise BridgeError("duplicate cancel ID or cancel capacity exhausted")
            self.seen_cancel_ids.add(cancel_id)
        supplied = broker_id.decode("utf-8") if isinstance(broker_id, bytes) else broker_id
        if supplied and supplied != order.broker_id:
            raise BridgeError("cancel broker identity mismatch")
        if order.cancel_pending:
            raise BridgeError("multiple cancel requests for the same order")
        if order.state in TERMINAL:
            self._emit(order, 4, cancel_id=cancel_id, code=-1, reason="broker order is terminal")
            return
        order.cancel_pending, order.cancel_id = True, cancel_id
        order.cancel_started = self.clock()
        fields = {**self._identity(), "UserID": self.credentials.user_id,
                  "ExchangeID": self.instrument.venue, "InstrumentID": self.instrument.symbol,
                  "ActionFlag": "0", "OrderActionRef": self.request_id + 1}
        if order.system_id:
            fields["OrderSysID"] = order.system_id
        else:
            fields.update(FrontID=self.front_id, SessionID=self.session_id,
                          OrderRef=order.native_ref or order.order_ref)
        ret, rid = self._request("CThostFtdcInputOrderActionField", "ReqOrderAction", fields)
        self.cancel_requests[rid] = client_id
        order.cancel_native_id = rid
        if ret:
            self._clear_cancel(order)
            self._emit(order, 4, cancel_id=cancel_id, code=ret,
                       reason="CTP cancel request was not sent")
            if self.stopping:
                raise BridgeError("drain cancellation was not sent; orders may remain live")

    def _clear_cancel(self, order):
        cancel_id = order.cancel_id
        self.cancel_requests.pop(order.cancel_native_id, None)
        order.cancel_pending, order.cancel_id, order.cancel_native_id = False, 0, 0
        return cancel_id

    def stop(self):
        if self.stopping:
            return
        self.stopping = True
        self.deadline = self.clock() + self.timeout
        for order in self.orders.values():
            if order.state not in TERMINAL and not order.cancel_pending:
                self.cancel(order.client_id, 0)
        self._changed()


def make_spi(sdk, session):
    """Keep native callback objects alive for the complete API lifetime."""
    names = ("OnFrontConnected", "OnFrontDisconnected", "OnRspAuthenticate",
             "OnRspUserLogin", "OnRspSettlementInfoConfirm", "OnRspError",
             "OnRtnOrder", "OnRtnTrade", "OnRspOrderInsert", "OnErrRtnOrderInsert",
             "OnRspOrderAction", "OnErrRtnOrderAction") + tuple(
                 "OnRspQry" + kind for kind in QUERY_KINDS)
    def callback(name):
        def invoke(self, *args):
            session.callback(name, *args)
        return invoke
    cls = type("SimNowTraderSpi", (sdk.CThostFtdcTraderSpi,),
               {name: callback(name) for name in names})
    return cls()


def marker(message):
    print(message, flush=True)


def run(args):
    if args.front not in SIMNOW_FRONTS:
        raise BridgeError("--front must be a published SimNow trader endpoint")
    if not 1 <= args.listen_port <= 65535 or not 0 < args.connect_timeout_seconds <= 3600:
        raise BridgeError("invalid listen port or connection timeout")
    if not 0.01 <= args.query_interval_seconds <= 60:
        raise BridgeError("query interval must be in 0.01..60 seconds")
    credentials = Credentials.from_environment()
    token = os.environ.get("YQUANT_CTP_BRIDGE_TOKEN", "").encode("utf-8")
    if not 1 <= len(token) <= 64 or b"\0" in token:
        raise BridgeError("set YQUANT_CTP_BRIDGE_TOKEN to 1..64 non-NUL bytes")
    instrument = load_instrument(args.catalog, args.venue, args.symbol)
    with Path(args.catalog).open(encoding="utf-8") as source:
        entry = next(item for item in json.load(source)["instruments"]
                     if item["venue"] == args.venue and item["symbol"] == args.symbol)
    multiplier = entry["contract_multiplier"]
    try:
        from openctp_ctp import tdapi
    except ImportError:
        raise BridgeError("install the platform-matched openctp-ctp package") from None
    interrupted = threading.Event()
    previous = {}
    for sig in (signal.SIGINT, signal.SIGTERM):
        previous[sig] = signal.signal(sig, lambda *_: interrupted.set())
    api = None
    try:
        with tempfile.TemporaryDirectory(prefix="yquant-ctp-trader-") as flow, socket.socket() as listener:
            listener.bind(("127.0.0.1", args.listen_port))
            listener.listen(1)
            listener.setblocking(False)
            marker(f"ctp_trader_listening port={args.listen_port}")
            api = tdapi.CThostFtdcTraderApi.CreateFtdcTraderApi(flow + os.sep)
            try:
                session = TraderSession(tdapi, api, instrument, multiplier, credentials,
                                        args.query_interval_seconds, args.connect_timeout_seconds, marker)
                spi = make_spi(tdapi, session)
                api.RegisterSpi(spi)
                api.RegisterFront(args.front)
                api.SubscribePrivateTopic(tdapi.THOST_TERT_QUICK)
                api.SubscribePublicTopic(tdapi.THOST_TERT_QUICK)
                api.Init()
                serve(listener, session, instrument, token, interrupted)
            finally:
                api.Release()
                api = None
    finally:
        if api is not None:
            api.Release()
        for sig, handler in previous.items():
            signal.signal(sig, handler)


def serve(listener, session, instrument, token, interrupted):
    """Serve exactly one authenticated localhost client without callback I/O."""
    connection = None
    incoming, outgoing = bytearray(), bytearray()
    commands = deque()
    generation = secrets.randbits(64) or 1
    expected_sequence = 1
    send_sequence = 0
    hello = welcomed = False
    last_received = last_heartbeat = time.monotonic()
    specs = [{"id": instrument.instrument_id, "price_decimals": instrument.price_decimals}]
    def send(message):
        nonlocal send_sequence
        send_sequence += 1
        message.update(generation=generation, sequence=send_sequence)
        frame = wire.encode_message(message)
        if len(outgoing) + len(frame) > MAX_OUTGOING_FRAMES * wire.MAX_FRAME_SIZE:
            raise BridgeError("local trading client backpressure exceeded its bound")
        outgoing.extend(frame)
    try:
        while True:
            session.poll()
            if interrupted.is_set():
                if not session.ready:
                    raise BridgeError("interrupted before SimNow initialization completed")
                session.stop()
            if connection is None:
                readable, _, _ = select.select([listener], [], [], 0.02)
                if readable:
                    connection, _ = listener.accept()
                    connection.setblocking(False)
                    last_received = time.monotonic()
                elif session.ready and time.monotonic() - last_received > session.timeout:
                    raise BridgeError("local client connection timed out")
                continue
            if hello and session.ready and not welcomed:
                send({"type": wire.WELCOME, "instruments": specs, "account": deepcopy(session.account)})
                welcomed = True
                marker("ctp_trader_connected")
            while session.messages:
                message = session.messages.popleft()
                if welcomed:
                    send(message)
            if welcomed and commands:
                command = commands.popleft()
                if command["type"] == wire.NEW_ORDER:
                    session.submit(command["client_order_id"], command["request"])
                elif command["type"] == wire.CANCEL_ORDER:
                    session.cancel(command["client_order_id"], command["cancel_id"], command["adapter_order_id"])
                else:
                    session.stop()
            if welcomed and session.drained:
                send({"type": wire.DRAINED})
                # Flush a bounded final frame queue; no SDK callback waits here.
                deadline = time.monotonic() + session.timeout
                while outgoing:
                    if time.monotonic() > deadline:
                        raise BridgeError("local client did not receive drain confirmation")
                    _, writable, _ = select.select([], [connection], [], 0.02)
                    if writable:
                        sent = connection.send(outgoing)
                        if not sent:
                            raise BridgeError("local trading client disconnected")
                        del outgoing[:sent]
                marker("ctp_trader_drained")
                return
            now = time.monotonic()
            if now - last_received > session.timeout:
                raise BridgeError("local trading client heartbeat timed out; broker state is uncertain")
            if welcomed and now - last_heartbeat >= 1:
                send({"type": wire.HEARTBEAT})
                last_heartbeat = now
            readable, writable, _ = select.select([connection], [connection] if outgoing else [], [], 0.02)
            if writable:
                sent = connection.send(outgoing)
                if not sent:
                    raise BridgeError("local trading client disconnected; broker state is uncertain")
                del outgoing[:sent]
            if not readable:
                continue
            data = connection.recv(4096)
            if not data:
                raise BridgeError("local trading client disconnected; broker state is uncertain")
            last_received = time.monotonic()
            incoming.extend(data)
            if len(incoming) > wire.MAX_FRAME_SIZE * 2:
                raise BridgeError("local client frame buffer exceeded its bound")
            while len(incoming) >= wire.HEADER_SIZE:
                size = wire.frame_size(bytes(incoming[:wire.HEADER_SIZE]))
                if not size:
                    raise BridgeError("invalid trading protocol header")
                if len(incoming) < size:
                    break
                message = wire.decode_message(bytes(incoming[:size]))
                del incoming[:size]
                if message["sequence"] != expected_sequence:
                    raise BridgeError("trading client sequence is not contiguous")
                expected_sequence += 1
                if not hello:
                    if (message["type"] != wire.HELLO or message["generation"] != 0
                            or not hmac.compare_digest(message["token"], token)
                            or message["instruments"] != specs):
                        raise BridgeError("local client handshake rejected")
                    hello = True
                    continue
                if not welcomed or message["generation"] != generation:
                    raise BridgeError("local client generation or handshake mismatch")
                if message["type"] == wire.HEARTBEAT:
                    continue
                if message["type"] not in (wire.NEW_ORDER, wire.CANCEL_ORDER, wire.STOP):
                    raise BridgeError("unsupported trading client message")
                if len(commands) >= MAX_OUTGOING_FRAMES:
                    raise BridgeError("local command queue exceeded its bound")
                commands.append(message)
    finally:
        if connection is not None:
            connection.close()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--front", required=True)
    parser.add_argument("--catalog", required=True)
    parser.add_argument("--venue", required=True)
    parser.add_argument("--symbol", required=True)
    parser.add_argument("--listen-port", type=int, required=True)
    parser.add_argument("--connect-timeout-seconds", type=float, default=60)
    parser.add_argument("--query-interval-seconds", type=float, default=1)
    args = parser.parse_args(argv)
    try:
        run(args)
        return 0
    except Exception as error:
        diagnostic = str(error)
        for name in ("YQUANT_CTP_PASSWORD", "YQUANT_CTP_AUTH_CODE", "YQUANT_CTP_BRIDGE_TOKEN"):
            value = os.environ.get(name)
            if value:
                diagnostic = diagnostic.replace(value, "[redacted]")
        print("ctp_trader_error: " + diagnostic, file=sys.stderr, flush=True)
        return 1


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Read one CTP futures BBO and publish YQuant's TCP QuoteFrame protocol.

Only the market-data API is used. SDK callbacks copy borrowed fields into a
bounded queue; the owning thread performs all socket I/O. Disconnects and data
loss end this session rather than reconnecting with an ambiguous sequence.
`tradable` means that this bridge has positive, uncrossed two-sided quotes; the
CTP depth callback does not establish the exchange's current trading status.

API usage follows the package maintainer's examples:
https://github.com/Jedore/ctp.examples/blob/main/openctp-ctp/base_mdapi.py
https://github.com/Jedore/ctp.examples/blob/main/openctp-ctp/SubscribeMarketData.py
"""

import argparse
from dataclasses import dataclass, replace
import datetime as dt
from decimal import Decimal, InvalidOperation
import json
import math
import os
from pathlib import Path
import queue
import re
import select
import signal
import socket
import struct
import sys
import tempfile
import time


# These values are persisted by identifiers.h and quote_codec.cc.
VENUES = {"SHFE": 4, "DCE": 5, "CZCE": 6, "CFFEX": 7, "INE": 8, "GFEX": 9}
FRAME = struct.Struct("<IHHIB3xQQQqqqq8x")
FRAME_MAGIC = 0x31515159
FRAME_VERSION = 1
UINT64_MAX = (1 << 64) - 1
INT64_MAX = (1 << 63) - 1
SHANGHAI = dt.timezone(dt.timedelta(hours=8), "Asia/Shanghai")
EPOCH = dt.datetime(1970, 1, 1, tzinfo=dt.timezone.utc)
# One bounded burst of copied callbacks; there is no unbounded send backlog.
DEFAULT_QUEUE_CAPACITY = 8192
# Cached login snapshots and unknown calendar dates cannot enable quoting.
DEFAULT_MAX_EVENT_AGE_SECONDS = 5.0
RAW_FIELDS = ("InstrumentID", "ExchangeID", "ActionDay", "TradingDay",
              "UpdateTime", "UpdateMillisec", "BidPrice1", "BidVolume1",
              "AskPrice1", "AskVolume1")


class BridgeError(Exception):
    """A terminal configuration, source or transport error."""


def _integer(value, name, minimum=0, maximum=INT64_MAX):
    if type(value) is not int or not minimum <= value <= maximum:
        raise BridgeError(f"{name} must be an integer in {minimum}..{maximum}")
    return value


def instrument_id(venue, symbol):
    """Match MakeInstrumentId exactly, including venue-specific case/months."""
    if venue not in VENUES or not isinstance(symbol, str):
        raise BridgeError("a supported futures venue and canonical symbol are required")
    upper = venue in ("CZCE", "CFFEX")
    letters = "[A-Z]" if upper else "[a-z]"
    months = 3 if venue == "CZCE" else 4
    match = re.fullmatch(f"({letters}{{1,3}})([0-9]{{{months}}})", symbol)
    if not match or not 1 <= int(match[2]) % 100 <= 12:
        raise BridgeError("invalid canonical futures symbol for venue")
    product = 0
    for letter in match[1]:
        product = product * 27 + ord(letter.upper()) - ord("A") + 1
    month = int(match[2]) + (0 if venue == "CZCE" else 1000)
    return (VENUES[venue] << 32) | 0x80000000 | (product << 14) | month


@dataclass(frozen=True)
class Instrument:
    """Owned identity and units resolved from the shared instrument catalog."""

    venue: str
    symbol: str
    instrument_id: int
    price_decimals: int
    price_tick: int
    quantity_step: int


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise BridgeError(f"duplicate catalog field: {key}")
        result[key] = value
    return result


def load_instrument(catalog, venue, symbol):
    """Load one explicit futures contract; never infer specs or a main contract."""
    identity = instrument_id(venue, symbol)
    with Path(catalog).open(encoding="utf-8") as source:
        document = json.load(source, object_pairs_hook=_unique_object)
    if not isinstance(document, dict) or set(document) != {"instruments"}:
        raise BridgeError("catalog must contain only an instruments array")
    entries = document["instruments"]
    if not isinstance(entries, list) or not all(isinstance(x, dict) for x in entries):
        raise BridgeError("catalog instruments must be objects")
    matches = [x for x in entries if x.get("venue") == venue
               and x.get("symbol") == symbol]
    if len(matches) != 1:
        raise BridgeError("catalog must contain exactly one matching venue/symbol")
    item = matches[0]
    required = {"venue", "symbol", "price_decimals", "instrument_type",
                "price_tick", "quantity_step", "contract_multiplier",
                "margin_rate_ppm", "open_fee_per_lot",
                "close_today_fee_per_lot", "close_yesterday_fee_per_lot"}
    if set(item) != required or item["instrument_type"] != "futures":
        raise BridgeError("selected catalog entry must provide complete futures specifications")
    decimals = _integer(item["price_decimals"], "price_decimals", 0, 6)
    tick = _integer(item["price_tick"], "price_tick", 1)
    step = _integer(item["quantity_step"], "quantity_step", 1)
    _integer(item["contract_multiplier"], "contract_multiplier", 1)
    _integer(item["margin_rate_ppm"], "margin_rate_ppm", 1, 1000000)
    for field in ("open_fee_per_lot", "close_today_fee_per_lot",
                  "close_yesterday_fee_per_lot"):
        _integer(item[field], field)
    return Instrument(venue, symbol, identity, decimals, tick, step)


def _text(value):
    if isinstance(value, bytes):
        value = value.decode("ascii")
    if not isinstance(value, str):
        raise BridgeError("CTP text field has an unexpected type")
    return value.rstrip("\0")


def event_time_ns(raw):
    """Use calendar ActionDay only; TradingDay cannot date a night-session tick.

    Missing/malformed ActionDay, time or milliseconds leave event time unknown.
    In particular, no current date, login trading day or night-session heuristic
    is substituted for a calendar date the upstream did not supply.
    """
    try:
        day, clock = _text(raw.get("ActionDay", "")), _text(raw.get("UpdateTime", ""))
        millis = raw.get("UpdateMillisec")
        if (not re.fullmatch(r"[0-9]{8}", day)
                or not re.fullmatch(r"[0-9]{2}:[0-9]{2}:[0-9]{2}", clock)
                or type(millis) is not int or not 0 <= millis <= 999):
            return 0
        stamp = dt.datetime.strptime(day + clock, "%Y%m%d%H:%M:%S")
        delta = stamp.replace(tzinfo=SHANGHAI).astimezone(dt.timezone.utc) - EPOCH
        result = (delta.days * 86400 + delta.seconds) * 1000000000 + millis * 1000000
        return result if 0 < result <= UINT64_MAX else 0
    except (ValueError, TypeError, UnicodeError, BridgeError):
        return 0


def _side(raw, prefix, instrument):
    quantity = _integer(raw[prefix + "Volume1"], prefix + "Volume1")
    if quantity == 0:
        return 0, 0
    try:
        price = Decimal(str(raw[prefix + "Price1"]))
    except (InvalidOperation, ValueError):
        raise BridgeError(f"invalid CTP {prefix}Price1") from None
    # CTP uses DBL_MAX to denote an absent price. Other nonfinite/nonpositive
    # quotes also cannot describe a present domestic futures side.
    if not price.is_finite() or price <= 0 or price >= Decimal("1e308"):
        return 0, 0
    encoded = price * (10 ** instrument.price_decimals)
    if encoded != encoded.to_integral_value() or encoded > INT64_MAX:
        raise BridgeError(f"{prefix}Price1 cannot be represented at catalog precision")
    value = int(encoded)
    if value % instrument.price_tick or quantity % instrument.quantity_step:
        raise BridgeError(f"{prefix} side violates catalog price/quantity steps")
    return value, quantity


@dataclass(frozen=True)
class Quote:
    """Normalized, self-contained fields for the padding-free quote wire frame."""

    source_id: int
    instrument_id: int
    sequence: int
    event_time_ns: int
    bid_price: int
    bid_qty: int
    ask_price: int
    ask_qty: int
    flags: int

    def encode(self):
        return FRAME.pack(FRAME_MAGIC, FRAME_VERSION, FRAME.size,
                          self.source_id, self.flags, self.instrument_id,
                          self.sequence, self.event_time_ns, self.bid_price,
                          self.bid_qty, self.ask_price, self.ask_qty)


def normalize_quote(raw, instrument, source_id, sequence):
    """Map one copied CTP snapshot without filling missing sides or timestamps."""
    _integer(source_id, "source_id", 1, (1 << 32) - 1)
    _integer(sequence, "sequence", 1, UINT64_MAX)
    if _text(raw.get("InstrumentID", "")) != instrument.symbol:
        raise BridgeError("CTP returned a different contract from the subscription")
    exchange = _text(raw.get("ExchangeID", ""))
    if exchange and exchange != instrument.venue:
        raise BridgeError("CTP returned a different exchange from the catalog")
    bid, bid_qty = _side(raw, "Bid", instrument)
    ask, ask_qty = _side(raw, "Ask", instrument)
    event = event_time_ns(raw)
    flags = (1 if bid_qty else 0) | (2 if ask_qty else 0)
    if bid_qty and ask_qty and bid < ask:
        flags |= 4
    if event:
        flags |= 8
    return Quote(source_id, instrument.instrument_id, sequence, event,
                 bid, bid_qty, ask, ask_qty, flags)


def freshness_gate(quote, now_ns, max_age_ns):
    """Disable quoting on stale/future/undated snapshots without losing BBO.

    The event timestamp and all observed values remain available for recording.
    The same bound applies to past and future clock skew; no event timestamp is
    synthesized from reception time to make an unknown date appear fresh.
    """
    if not quote.flags & 8 or abs(now_ns - quote.event_time_ns) > max_age_ns:
        return replace(quote, flags=quote.flags & ~4)
    return quote


class CallbackQueue:
    """Copies SDK-owned data and preserves the first terminal callback error.

    The CTP callback thread only enqueues; the owner drains and calls the SDK.
    Overflow is terminal, so a dropped callback never becomes a silent gap.
    """

    def __init__(self, capacity=DEFAULT_QUEUE_CAPACITY):
        self.events = queue.Queue(capacity)
        self.error = None
        self.closing = False

    def fail(self, message):
        if not self.closing and self.error is None:
            self.error = message

    def push(self, kind, *values):
        if self.closing or self.error is not None:
            return
        try:
            self.events.put_nowait((kind, values))
        except queue.Full:
            self.fail("CTP callback queue overflow; capture is incomplete")


def create_spi(mdapi, callbacks):
    """Build the optional SDK subclass without importing native modules offline."""
    class MarketSpi(mdapi.CThostFtdcMdSpi):
        def OnFrontConnected(self):
            callbacks.push("connected")

        def OnFrontDisconnected(self, reason):
            callbacks.fail(f"CTP front disconnected: reason={reason}")

        def OnHeartBeatWarning(self, lapse):
            callbacks.fail(f"CTP heartbeat warning: seconds={lapse}")

        def OnRspError(self, info, request_id, is_last):
            callbacks.fail(f"CTP response error: id={getattr(info, 'ErrorID', -1)}")

        def OnRspUserLogin(self, response, info, request_id, is_last):
            callbacks.push("login", getattr(info, "ErrorID", 0),
                           response is not None, bool(is_last))

        def OnRspSubMarketData(self, response, info, request_id, is_last):
            try:
                symbol = _text(response.InstrumentID) if response is not None else ""
                callbacks.push("subscribed", getattr(info, "ErrorID", 0),
                               symbol, bool(is_last))
            except Exception as error:
                callbacks.fail(f"CTP subscription callback failed: {type(error).__name__}")

        def OnRtnDepthMarketData(self, response):
            try:
                # Every field is copied before returning the borrowed SWIG object.
                raw = {name: getattr(response, name) for name in RAW_FIELDS}
                callbacks.push("quote", raw)
            except Exception as error:
                callbacks.fail(f"CTP quote callback failed: {type(error).__name__}")

    return MarketSpi()


class Session:
    """Main-thread CTP handshake and strictly increasing session sequence."""

    def __init__(self, mdapi, api, instrument, source_id, credentials):
        self.mdapi, self.api = mdapi, api
        self.instrument, self.source_id = instrument, source_id
        self.credentials = credentials
        self.connected = self.logged_in = self.subscribed = False
        self.sequence = 0
        # The simulated ledger does not settle or roll today/yesterday buckets.
        # Scope this entire session to the first snapshot's explicit trading day.
        self.trading_day = None

    def process(self, event):
        kind, values = event
        if kind == "connected":
            if self.connected:
                raise BridgeError("CTP attempted to reconnect within this session")
            self.connected = True
            request = self.mdapi.CThostFtdcReqUserLoginField()
            request.BrokerID, request.UserID, request.Password = self.credentials
            result = self.api.ReqUserLogin(request, 1)
            if result != 0:
                raise BridgeError(f"CTP login request failed: result={result}")
        elif kind == "login":
            error, present, last = values
            if error or not present:
                raise BridgeError(f"CTP login failed: id={error}")
            if not last:
                return None
            if not self.connected or self.logged_in:
                raise BridgeError("unexpected CTP login response")
            self.logged_in = True
            print("CTP login succeeded", flush=True)
            result = self.api.SubscribeMarketData(
                [self.instrument.symbol.encode("ascii")], 1)
            if result != 0:
                raise BridgeError(f"CTP subscription request failed: result={result}")
        elif kind == "subscribed":
            error, symbol, last = values
            if error or symbol != self.instrument.symbol:
                raise BridgeError(f"CTP subscription failed: id={error}")
            if not self.logged_in or self.subscribed:
                raise BridgeError("unexpected CTP subscription response")
            if last:
                self.subscribed = True
        elif kind == "quote":
            if not self.logged_in:
                raise BridgeError("CTP quote preceded login")
            raw = values[0]
            try:
                trading_day = _text(raw.get("TradingDay", ""))
                if not re.fullmatch(r"[0-9]{8}", trading_day):
                    raise ValueError
                dt.datetime.strptime(trading_day, "%Y%m%d")
            except (ValueError, TypeError, UnicodeError, BridgeError):
                raise BridgeError("CTP quote has missing or invalid TradingDay") from None
            if self.trading_day is not None and trading_day != self.trading_day:
                raise BridgeError("CTP TradingDay changed; start a new single-day capture")
            # Some fronts may deliver the first snapshot before the subscription
            # acknowledgment. Preserve it; the handshake still has its timeout.
            if self.sequence == UINT64_MAX:
                raise BridgeError("CTP quote sequence exhausted")
            quote = normalize_quote(raw, self.instrument,
                                    self.source_id, self.sequence + 1)
            self.trading_day = trading_day
            self.sequence += 1
            return quote
        else:
            raise BridgeError("unknown CTP callback event")
        return None


def _consumer_stop_requested(stop_request_file):
    """Recognize the orchestrator's run-owned authorization to close TCP."""
    return stop_request_file is not None and Path(stop_request_file).is_file()


def _peer_state(connection, stop_request_file):
    """Return true only for explicitly requested consumer EOF/reset.

    The orchestrator creates its run-owned stop marker before stopping marketd.
    This handshake does not depend on how long recording flush/fsync takes.
    """
    try:
        data = connection.recv(1, socket.MSG_PEEK)
    except BlockingIOError:
        return False
    except ConnectionResetError:
        if _consumer_stop_requested(stop_request_file):
            return True
        raise
    if not data:
        if _consumer_stop_requested(stop_request_file):
            return True
        raise BridgeError("marketd consumer disconnected")
    raise BridgeError("marketd consumer sent data on a receive-only connection")


def pump(connection, session, callbacks, duration_seconds, max_quotes,
         connect_timeout_seconds, stopped,
         max_event_age_seconds=DEFAULT_MAX_EVENT_AGE_SECONDS,
         stop_request_file=None):
    """Drain copied callbacks and write complete frames with bounded backpressure.

    Duration starts at SDK initialization. A successful bounded capture must
    publish at least one snapshot. Partial writes retain the original bytes;
    a blocked consumer is terminal after the configured connection timeout.
    """
    started = time.monotonic()
    deadline = started + duration_seconds if duration_seconds else math.inf
    handshake_deadline = started + connect_timeout_seconds
    sent = 0
    pending = b""
    pending_started = 0
    pending_quote = None
    valid_sent = 0
    max_event_age_ns = int(max_event_age_seconds * 1000000000)
    while not stopped():
        now = time.monotonic()
        if callbacks.error:
            raise BridgeError(callbacks.error)
        if not session.subscribed and now >= handshake_deadline:
            raise BridgeError("CTP connection/login/subscription timed out")
        if not pending and (now >= deadline or (max_quotes and sent >= max_quotes)):
            break
        if not pending:
            try:
                event = callbacks.events.get_nowait()
            except queue.Empty:
                event = None
            if event is not None:
                quote = session.process(event)
                if quote is not None:
                    quote = freshness_gate(quote, time.time_ns(), max_event_age_ns)
                    pending = quote.encode()
                    pending_quote = quote
                    pending_started = now
                else:
                    continue
        readable, writable, errors = select.select(
            [connection], [connection] if pending else [], [connection], 0.05)
        if errors:
            raise BridgeError("marketd consumer socket failed")
        if readable:
            if _peer_state(connection, stop_request_file):
                break
        if pending and writable:
            checked = freshness_gate(pending_quote, time.time_ns(), max_event_age_ns)
            if checked != pending_quote:
                if len(pending) != FRAME.size:
                    raise BridgeError("CTP quote became stale during partial transmission")
                pending_quote = checked
                pending = checked.encode()
            try:
                count = connection.send(pending)
            except BlockingIOError:
                count = 0
            except (ConnectionResetError, BrokenPipeError):
                # marketd can close with unread quote bytes during shutdown,
                # producing RST or EPIPE instead of a zero-byte read.
                if _consumer_stop_requested(stop_request_file):
                    break
                raise
            if count:
                pending = pending[count:]
                if not pending:
                    sent += 1
                    if sent == 1:
                        print(f"CTP first snapshot: {session.instrument.venue}/"
                              f"{session.instrument.symbol} flags={pending_quote.flags}",
                              flush=True)
                    if pending_quote.flags & 7 == 7:
                        valid_sent += 1
                        if valid_sent == 1:
                            print(f"CTP first quote: {session.instrument.venue}/"
                                  f"{session.instrument.symbol} bid={pending_quote.bid_price}"
                                  f" qty={pending_quote.bid_qty} ask={pending_quote.ask_price}"
                                  f" qty={pending_quote.ask_qty} flags={pending_quote.flags}",
                                  flush=True)
        if pending and time.monotonic() - pending_started >= connect_timeout_seconds:
            raise BridgeError("marketd consumer backpressure timed out")
    if callbacks.error:
        raise BridgeError(callbacks.error)
    if not valid_sent and not stopped():
        raise BridgeError("capture ended without a fresh two-sided CTP quote")
    print(f"CTP bridge stopped: quotes={sent} fresh_quotes={valid_sent}", flush=True)
    return sent


def _finite_nonnegative(text):
    value = float(text)
    if not math.isfinite(value) or value < 0:
        raise argparse.ArgumentTypeError("must be a finite nonnegative number")
    return value


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--front", required=True,
                        help="broker-provided CTP market-data tcp://host:port")
    parser.add_argument("--catalog", required=True, type=Path)
    parser.add_argument("--venue", required=True, choices=VENUES)
    parser.add_argument("--symbol", required=True)
    parser.add_argument("--anonymous", action="store_true",
                        help="send an empty market-data login on a front that permits it")
    parser.add_argument("--listen-port", type=int, default=19110)
    parser.add_argument("--source-id", type=int, default=2)
    parser.add_argument("--duration-seconds", type=_finite_nonnegative, default=0,
                        help="0 runs until interrupted; duration starts at SDK init")
    parser.add_argument("--max-quotes", type=int, default=0,
                        help="0 imposes no quote count limit")
    parser.add_argument("--connect-timeout-seconds", type=_finite_nonnegative,
                        default=30, help="consumer, handshake and backpressure timeout")
    parser.add_argument("--max-event-age-seconds", type=_finite_nonnegative,
                        default=DEFAULT_MAX_EVENT_AGE_SECONDS,
                        help="disable quoting on unknown or stale/future event time (default: 5)")
    parser.add_argument("--stop-request-file", type=Path,
                        help="run-owned marker allowing expected marketd EOF during shutdown")
    args = parser.parse_args(argv)
    if not re.fullmatch(r"tcp://[^\s/:]+:[0-9]{1,5}", args.front):
        parser.error("--front must be tcp://host:port")
    if not 1 <= int(args.front.rsplit(":", 1)[1]) <= 65535:
        parser.error("--front port must be 1..65535")
    if not 1 <= args.listen_port <= 65535:
        parser.error("--listen-port must be 1..65535")
    if not 1 <= args.source_id <= (1 << 32) - 1:
        parser.error("--source-id must be 1..4294967295")
    if args.max_quotes < 0 or args.connect_timeout_seconds <= 0:
        parser.error("quote limit must be nonnegative and timeout must be positive")
    if args.max_event_age_seconds <= 0:
        parser.error("--max-event-age-seconds must be positive")
    return args


def run(args, stopped):
    instrument = load_instrument(args.catalog, args.venue, args.symbol)
    names = ("YQUANT_CTP_BROKER_ID", "YQUANT_CTP_USER_ID", "YQUANT_CTP_PASSWORD")
    credentials = ("", "", "") if args.anonymous else tuple(
        os.environ.get(name, "") for name in names)
    if not args.anonymous and not all(credentials):
        raise BridgeError("set YQUANT_CTP_BROKER_ID, YQUANT_CTP_USER_ID and YQUANT_CTP_PASSWORD")
    # Import is deliberately deferred: help, mapping and offline tests use only
    # Python's standard library. Only the market-data API is instantiated.
    try:
        from openctp_ctp import mdapi
    except ImportError:
        raise BridgeError("install an openctp-ctp build matching the broker API and this platform") from None
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("127.0.0.1", args.listen_port))
        listener.listen(1)
        listener.settimeout(0.1)
        print(f"CTP bridge listening: 127.0.0.1:{args.listen_port}", flush=True)
        deadline = time.monotonic() + args.connect_timeout_seconds
        while not stopped():
            if time.monotonic() >= deadline:
                raise BridgeError("marketd consumer connection timed out")
            try:
                connection, _ = listener.accept()
                break
            except socket.timeout:
                continue
        else:
            return 0
        # Close the listener after the sole consumer attaches. A replacement
        # consumer requires a new bridge session and a fresh sequence origin.
        listener.close()
        with connection, tempfile.TemporaryDirectory(prefix="yquant-ctp-flow-") as flow:
            connection.setblocking(False)
            connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            callbacks = CallbackQueue()
            api = mdapi.CThostFtdcMdApi.CreateFtdcMdApi(flow + os.sep)
            if api is None:
                raise BridgeError("CTP failed to create a market-data API")
            try:
                spi = create_spi(mdapi, callbacks)
                api.RegisterSpi(spi)
                api.RegisterFront(args.front)
                session = Session(mdapi, api, instrument, args.source_id, credentials)
                api.Init()
                return pump(connection, session, callbacks,
                            args.duration_seconds, args.max_quotes,
                            args.connect_timeout_seconds, stopped,
                            args.max_event_age_seconds, args.stop_request_file)
            finally:
                callbacks.closing = True
                api.Release()


def main(argv=None):
    args = parse_args(argv)
    stopping = False

    def stop(_signal, _frame):
        nonlocal stopping
        stopping = True

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    try:
        run(args, lambda: stopping)
        return 0
    except (BridgeError, OSError, ValueError, KeyError) as error:
        # No request object or environment value is included in diagnostics.
        print(f"CTP bridge failed: {error}", file=sys.stderr, flush=True)
        return 1
    except Exception as error:
        # Native-wrapper exceptions may include argument values. Report the
        # operation failure without echoing login fields or native traceback.
        print(f"CTP bridge failed: SDK operation raised {type(error).__name__}",
              file=sys.stderr, flush=True)
        return 1


if __name__ == "__main__":
    sys.exit(main())

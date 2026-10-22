"""Bounded codec for YQuant's owned trading messages.

The bridge writes wire version 3 and reads versions 1 through 3. Message
objects are dictionaries using the C++ field names; instrument dictionaries
use ``id`` and futures positions use ``instrument_id``. Opaque identifiers,
tokens and diagnostics accept UTF-8 strings or bytes and decode to bytes.
Version 3 carries broker identity and unallocated directional position freezes.
There is no socket or CTP dependency here. Live bridges reject simulation
quote messages at the framing boundary.
"""

import struct


HELLO, WELCOME, NEW_ORDER, CANCEL_ORDER, ORDER_UPDATE = range(1, 6)
ACCOUNT, STOP, DRAINED, HEARTBEAT, ERROR = range(6, 11)
HEADER_SIZE = 32
MAX_FRAME_SIZE = 16384
# Keep these values aligned with trading/protocol.cc and identifiers.h.
_MAGIC = 0x52545159
_VERSION = 3
_HEADER = struct.Struct("<IHHIIQQ")
_MAX_INSTRUMENTS = 64
_MAX_TOKEN_BYTES = 64
_MAX_OPAQUE_BYTES = 64
_MAX_REASON_BYTES = 128
_MAX_PRICE_DECIMALS = 6
_UINT64_MAX = (1 << 64) - 1
_INT64_MAX = (1 << 63) - 1
_INT64_MIN = -(1 << 63)
_MISSING = object()
_FUTURES_FIELDS = ("balance", "equity", "available", "margin", "reserved",
                   "realized_pnl", "unrealized_pnl", "commission")
_BUCKETS = ("long_today", "long_yesterday", "short_today", "short_yesterday")
_BUCKET_FIELDS = ("quantity", "frozen_qty", "open_notional")


class WireError(ValueError):
    """An invalid dictionary value or corrupt, unsupported wire frame."""


def _mapping(value, name):
    if not isinstance(value, dict):
        raise WireError(f"{name} must be a dictionary")
    return value


def _integer(value, name, minimum=0, maximum=_UINT64_MAX):
    if type(value) is not int or not minimum <= value <= maximum:
        raise WireError(f"{name} must be an integer in {minimum}..{maximum}")
    return value


def _int_field(obj, name, default=_MISSING, minimum=0, maximum=_UINT64_MAX):
    return _integer(obj.get(name, default), name, minimum, maximum)


def _signed(obj, name, default=_MISSING):
    return _int_field(obj, name, default, _INT64_MIN, _INT64_MAX)


def _boolean(value, name):
    if type(value) is not bool:
        raise WireError(f"{name} must be a boolean")
    return value


def _bytes(value, name, maximum, minimum=0, allow_nul=True):
    if isinstance(value, str):
        try:
            value = value.encode("utf-8")
        except UnicodeError as error:
            raise WireError(f"{name} must be valid UTF-8 or bytes") from error
    if not isinstance(value, bytes) or not minimum <= len(value) <= maximum:
        raise WireError(f"{name} must contain {minimum}..{maximum} bytes")
    if not allow_nul and b"\0" in value:
        raise WireError(f"{name} cannot contain NUL")
    return value


def _instrument(value, futures_only=False):
    """Validate the reversible numeric identity without catalog metadata."""
    value = _integer(value, "instrument_id")
    venue, code = value >> 32, value & 0xffffffff
    if 1 <= venue <= 3 and not futures_only and code <= 999999:
        return value
    if not 4 <= venue <= 9 or not code & 0x80000000:
        raise WireError("invalid instrument identity")
    product = (code & 0x7fffffff) >> 14
    letters = 0
    if not product:
        raise WireError("invalid futures product identity")
    while product:
        letters += 1
        if product % 27 == 0 or letters > 3:
            raise WireError("invalid futures product identity")
        product //= 27
    month = code & 0x3fff
    if venue == 6:
        if month > 999:
            raise WireError("invalid CZCE contract month")
    else:
        if not 1000 <= month <= 10999:
            raise WireError("invalid futures contract month")
        month -= 1000
    if not 1 <= month % 100 <= 12:
        raise WireError("invalid futures contract month")
    return value


def _effect(instrument, value):
    value = _integer(value, "position_effect", 0, 4)
    venue = instrument >> 32
    if (venue >= 4 and value == 0 or venue < 4 and value != 0
            or venue in (4, 8) and value == 4):
        raise WireError("position_effect does not match the instrument")
    return value


def _amount(price, quantity):
    if price <= 0 or quantity <= 0 or price > _INT64_MAX // quantity:
        raise WireError("price/quantity must be positive with int64 notional")


class _Writer:
    """Collect one bounded frame; no caller-owned output is mutated."""

    def __init__(self):
        self.parts = []
        self.size = HEADER_SIZE

    def raw(self, value):
        self.size += len(value)
        if self.size > MAX_FRAME_SIZE:
            raise WireError("message exceeds maximum frame size")
        self.parts.append(value)

    def put(self, form, *values):
        self.raw(struct.pack("<" + form, *values))

    def opaque(self, value):
        self.put("B", len(value))
        self.raw(value)


class _Reader:
    """Read whole fields only; truncated fields always raise WireError."""

    def __init__(self, payload, version):
        self.payload = payload
        self.offset = 0
        self.version = version

    def raw(self, size):
        if size > len(self.payload) - self.offset:
            raise WireError("truncated message payload")
        result = self.payload[self.offset:self.offset + size]
        self.offset += size
        return result

    def get(self, form):
        values = struct.unpack("<" + form, self.raw(struct.calcsize("<" + form)))
        return values[0] if len(values) == 1 else values

    def boolean(self):
        value = self.get("B")
        if value > 1:
            raise WireError("noncanonical boolean")
        return value == 1

    def opaque(self, maximum=_MAX_OPAQUE_BYTES):
        size = self.get("B")
        if size > maximum:
            raise WireError("oversized byte string")
        return self.raw(size)


def _write_instruments(writer, instruments):
    if (not isinstance(instruments, list)
            or not 1 <= len(instruments) <= _MAX_INSTRUMENTS):
        raise WireError("instruments must contain 1..64 entries")
    writer.put("H", len(instruments))
    seen = set()
    for item in instruments:
        item = _mapping(item, "instrument")
        identity = _instrument(item.get("id"))
        decimals = _int_field(item, "price_decimals", 0, 0, _MAX_PRICE_DECIMALS)
        if identity in seen:
            raise WireError("duplicate instrument identity")
        seen.add(identity)
        writer.put("QB", identity, decimals)


def _read_instruments(reader):
    count = reader.get("H")
    if not 1 <= count <= _MAX_INSTRUMENTS:
        raise WireError("invalid instrument count")
    return [dict(zip(("id", "price_decimals"), reader.get("QB")))
            for _ in range(count)]


def _write_account(writer, account):
    account = _mapping(account, "account")
    revision = _int_field(account, "revision", 0)
    kind = _int_field(account, "type", 0, 0, 1)
    broker = _boolean(account.get("broker_managed", False), "broker_managed")
    if broker and kind != 1:
        raise WireError("broker_managed requires a futures account")
    writer.put("QBB", revision, kind, broker)
    if kind == 1:
        if account.get("positions", []):
            raise WireError("futures account cannot contain cash positions")
        futures = _mapping(account.get("futures", {}), "futures")
        values = {name: _signed(futures, name, 0) for name in _FUTURES_FIELDS}
        if (values["margin"] < 0 or values["reserved"] < 0
                or values["commission"] < 0):
            raise WireError("negative futures margin, reservation or commission")
        if (not broker
                and (values["balance"] + values["unrealized_pnl"] != values["equity"]
                     or values["equity"] - values["margin"] - values["reserved"]
                     != values["available"])):
            raise WireError("inconsistent futures account identity")
        writer.put("8q", *(values[name] for name in _FUTURES_FIELDS))
        positions = account.get("futures_positions", [])
        if not isinstance(positions, list) or len(positions) > _MAX_INSTRUMENTS:
            raise WireError("invalid futures position count")
        writer.put("H", len(positions))
        seen = set()
        for position in positions:
            position = _mapping(position, "futures position")
            identity = _instrument(position.get("instrument_id"), True)
            if identity in seen:
                raise WireError("duplicate futures position identity")
            seen.add(identity)
            writer.put("Q", identity)
            gross = 0
            room = []
            for name in _BUCKETS:
                bucket = _mapping(position.get(name, {}), name)
                quantity, frozen, notional = (
                    _int_field(bucket, field, 0, 0, _INT64_MAX)
                    for field in _BUCKET_FIELDS)
                if frozen > quantity:
                    raise WireError("frozen quantity exceeds bucket quantity")
                gross += quantity
                room.append(quantity - frozen)
                writer.put("3q", quantity, frozen, notional)
            if gross > _INT64_MAX:
                raise WireError("futures gross quantity overflows int64")
            margin = _int_field(position, "margin", 0, 0, _INT64_MAX)
            writer.put("qq", margin, _signed(position, "unrealized_pnl", 0))
            long_frozen = _int_field(position, "unallocated_long_frozen", 0,
                                     0, _INT64_MAX)
            short_frozen = _int_field(position, "unallocated_short_frozen", 0,
                                      0, _INT64_MAX)
            if (long_frozen > room[0] + room[1]
                    or short_frozen > room[2] + room[3]
                    or not broker and (long_frozen or short_frozen)):
                raise WireError("invalid unallocated broker freeze")
            writer.put("qq", long_frozen, short_frozen)
        return
    if account.get("futures_positions", []):
        raise WireError("cash account cannot contain futures positions")
    cash = _mapping(account.get("cash", {}), "cash")
    balance, reserved, available = (
        _int_field(cash, name, 0, 0, _INT64_MAX)
        for name in ("balance", "reserved", "available"))
    if reserved > balance or available != balance - reserved:
        raise WireError("inconsistent cash account identity")
    writer.put("3q", balance, reserved, available)
    positions = account.get("positions", [])
    if not isinstance(positions, list) or len(positions) > _MAX_INSTRUMENTS:
        raise WireError("invalid cash position count")
    writer.put("H", len(positions))
    seen = set()
    for position in positions:
        position = _mapping(position, "cash position")
        identity = _instrument(position.get("instrument_id"))
        total, available, reserved = (
            _int_field(position, name, 0, 0, _INT64_MAX)
            for name in ("total_qty", "available_qty", "reserved_qty"))
        if (identity in seen or available > total
                or reserved > total - available):
            raise WireError("inconsistent or duplicate cash position")
        seen.add(identity)
        writer.put("Q3q", identity, total, available, reserved)


def _read_account(reader):
    result = {"revision": reader.get("Q"),
              "type": reader.get("B") if reader.version >= 2 else 0,
              "broker_managed": reader.boolean() if reader.version >= 3 else False}
    if result["type"] == 1:
        result["futures"] = dict(zip(_FUTURES_FIELDS, reader.get("8q")))
        count = reader.get("H")
        if count > _MAX_INSTRUMENTS:
            raise WireError("invalid futures position count")
        positions = []
        for _ in range(count):
            position = {"instrument_id": reader.get("Q")}
            for name in _BUCKETS:
                position[name] = dict(zip(_BUCKET_FIELDS, reader.get("3q")))
            position["margin"], position["unrealized_pnl"] = reader.get("2q")
            if reader.version >= 3:
                (position["unallocated_long_frozen"],
                 position["unallocated_short_frozen"]) = reader.get("2q")
            else:
                position["unallocated_long_frozen"] = 0
                position["unallocated_short_frozen"] = 0
            positions.append(position)
        result["futures_positions"] = positions
    elif result["type"] == 0:
        result["cash"] = dict(zip(("balance", "reserved", "available"),
                                   reader.get("3q")))
        count = reader.get("H")
        if count > _MAX_INSTRUMENTS:
            raise WireError("invalid cash position count")
        result["positions"] = [dict(zip(
            ("instrument_id", "total_qty", "available_qty", "reserved_qty"),
            reader.get("Q3q"))) for _ in range(count)]
    else:
        raise WireError("unknown account type")
    return result


def _write_reason(writer, reason):
    reason = _mapping(reason, "reason")
    code = _int_field(reason, "code", 0, -(1 << 31), (1 << 31) - 1)
    truncated = _boolean(reason.get("truncated", False), "truncated")
    text = _bytes(reason.get("text", b""), "reason text", _MAX_REASON_BYTES)
    writer.put("iBB", code, truncated, len(text))
    writer.raw(text)


def _read_reason(reader):
    return {"code": reader.get("i"), "truncated": reader.boolean(),
            "text": reader.opaque(_MAX_REASON_BYTES)}


def _write_order(writer, order):
    order = _mapping(order, "order")
    identity = _instrument(order.get("instrument_id"))
    client = _int_field(order, "client_order_id", minimum=1)
    broker = _bytes(order.get("broker_order_id", b""), "broker_order_id",
                    _MAX_OPAQUE_BYTES)
    side = _int_field(order, "side", minimum=1, maximum=2)
    price = _signed(order, "limit_price")
    quantity = _signed(order, "original_qty")
    _amount(price, quantity)
    cumulative = _int_field(order, "cumulative_filled_qty", 0, 0, quantity)
    leaves = _int_field(order, "leaves_qty", minimum=0, maximum=_INT64_MAX)
    state = _int_field(order, "state", 0, 0, 4)
    if ((state >= 2 and leaves != 0)
            or (state == 2 and cumulative != quantity)
            or (state < 2 and leaves != quantity - cumulative)):
        raise WireError("inconsistent order state and quantities")
    pending = _boolean(order.get("cancel_pending", False), "cancel_pending")
    cancel_id = _int_field(order, "cancel_request_id", 0)
    report = _int_field(order, "report_sequence", 0)
    received = _int_field(order, "receive_time_ns", 0)
    effect = _effect(identity, order.get("position_effect", 0))
    writer.put("Q", client)
    writer.opaque(broker)
    writer.put("QB4qBB3QB", identity, side, price, quantity, cumulative, leaves,
               state, pending, cancel_id, report, received, effect)


def _read_order(reader):
    result = {"client_order_id": reader.get("Q"),
              "broker_order_id": reader.opaque()}
    fields = ("instrument_id", "side", "limit_price", "original_qty",
              "cumulative_filled_qty", "leaves_qty", "state")
    result.update(zip(fields, reader.get("QB4qB")))
    result["cancel_pending"] = reader.boolean()
    result.update(zip(("cancel_request_id", "report_sequence", "receive_time_ns"),
                      reader.get("3Q")))
    result["position_effect"] = reader.get("B") if reader.version >= 2 else 0
    return result


def _write_fill(writer, fill, order):
    fill = _mapping(fill, "fill")
    # Executions must describe the same owned order as the adjacent snapshot.
    for name in ("client_order_id", "instrument_id", "side", "position_effect"):
        actual = _int_field(fill, name)
        if actual != order.get(name, 0):
            raise WireError(f"fill {name} differs from order")
    broker = _bytes(fill.get("broker_order_id"), "broker_order_id",
                    _MAX_OPAQUE_BYTES, 1)
    if broker != _bytes(order.get("broker_order_id", b""), "broker_order_id",
                        _MAX_OPAQUE_BYTES):
        raise WireError("fill broker_order_id differs from order")
    execution = _bytes(fill.get("execution_id"), "execution_id",
                       _MAX_OPAQUE_BYTES, 1)
    price, quantity = _signed(fill, "price"), _signed(fill, "quantity")
    _amount(price, quantity)
    if quantity > order.get("cumulative_filled_qty", 0):
        raise WireError("fill exceeds cumulative filled quantity")
    event = _int_field(fill, "event_time_ns", 0)
    received = _int_field(fill, "receive_time_ns", 0)
    has_event = _boolean(fill.get("has_event_time", False), "has_event_time")
    if not has_event and event:
        raise WireError("undated fill has a nonzero event time")
    writer.put("Q", fill["client_order_id"])
    writer.opaque(broker)
    writer.opaque(execution)
    writer.put("QB2q2QBB", fill["instrument_id"], fill["side"], price, quantity,
               event, received, has_event, fill["position_effect"])


def _read_fill(reader):
    result = {"client_order_id": reader.get("Q"),
              "broker_order_id": reader.opaque(), "execution_id": reader.opaque()}
    result.update(zip(("instrument_id", "side", "price", "quantity",
                       "event_time_ns", "receive_time_ns"), reader.get("QB2q2Q")))
    result["has_event_time"] = reader.boolean()
    result["position_effect"] = reader.get("B") if reader.version >= 2 else 0
    return result


def _write_payload(writer, message):
    kind = message["type"]
    if kind == HELLO:
        client = _int_field(message, "client_id", minimum=1)
        token = _bytes(message.get("token"), "token", _MAX_TOKEN_BYTES, 1, False)
        writer.put("Q", client)
        writer.opaque(token)
        _write_instruments(writer, message.get("instruments"))
    elif kind == WELCOME:
        _write_instruments(writer, message.get("instruments"))
        _write_account(writer, message.get("account"))
    elif kind == NEW_ORDER:
        request = _mapping(message.get("request"), "request")
        client = _int_field(message, "order_id", minimum=1)
        identity = _instrument(request.get("instrument_id"))
        side = _int_field(request, "side", minimum=1, maximum=2)
        price, quantity = _signed(request, "limit_price"), _signed(request, "quantity")
        _amount(price, quantity)
        effect = _effect(identity, request.get("position_effect", 0))
        writer.put("QQBqqB", client, identity, side, price, quantity, effect)
    elif kind == CANCEL_ORDER:
        writer.put("QQ", _int_field(message, "order_id", minimum=1),
                   _int_field(message, "cancel_id", minimum=1))
        writer.opaque(_bytes(message.get("broker_order_id", b""),
                             "broker_order_id", _MAX_OPAQUE_BYTES))
    elif kind == ORDER_UPDATE:
        notify = _boolean(message.get("notify_order", True), "notify_order")
        has_fill = message.get("fill") is not None
        if ("has_fill" in message
                and _boolean(message["has_fill"], "has_fill") != has_fill):
            raise WireError("has_fill disagrees with fill presence")
        writer.put("BB", notify, has_fill)
        order = _mapping(message.get("order"), "order")
        _write_order(writer, order)
        update_kind = _int_field(message, "kind", minimum=0, maximum=5)
        writer.put("BQ", update_kind, _int_field(message, "cancel_request_id", 0))
        _write_reason(writer, message.get("reason", {}))
        if has_fill:
            if update_kind != 2:
                raise WireError("only fill updates may carry a fill")
            _write_fill(writer, message["fill"], order)
        _write_account(writer, message.get("account"))
    elif kind == ACCOUNT:
        _write_account(writer, message.get("account"))
    elif kind == ERROR:
        writer.put("B", _int_field(message, "error_code", minimum=1, maximum=7))
        _write_reason(writer, message.get("reason", {}))


def encode_message(message):
    """Encode one version-3 dictionary; invalid values raise WireError.

    ``type`` and nonzero ``sequence`` are required; ``generation`` defaults to
    zero. Unused fields are ignored, as in the C++ codec. Optional order flags,
    identifiers, times, diagnostics and zero account/bucket values follow C++
    defaults. ``has_fill``, when supplied, must agree with ``fill`` presence.
    """
    message = _mapping(message, "message")
    kind = _int_field(message, "type", minimum=HELLO, maximum=ERROR)
    generation = _int_field(message, "generation", 0)
    sequence = _int_field(message, "sequence", minimum=1)
    writer = _Writer()
    _write_payload(writer, message)
    return (_HEADER.pack(_MAGIC, _VERSION, kind, writer.size, 0, generation,
                         sequence) + b"".join(writer.parts))


def frame_size(header):
    """Return full size for an exact valid header, otherwise zero.

    The header does not establish payload validity. Versions 1..3 are accepted
    for journal compatibility; simulation-quote message type 11 is rejected.
    """
    if not isinstance(header, (bytes, bytearray, memoryview)) or len(header) != HEADER_SIZE:
        return 0
    try:
        magic, version, kind, size, reserved, _, sequence = _HEADER.unpack(header)
    except (struct.error, BufferError, TypeError):
        return 0
    if (magic != _MAGIC or version not in (1, 2, 3)
            or not HELLO <= kind <= ERROR or reserved != 0 or sequence == 0
            or not HEADER_SIZE <= size <= MAX_FRAME_SIZE):
        return 0
    return size


def decode_message(frame):
    """Decode exactly one complete frame; malformed data raises WireError.

    The returned dictionary owns its byte strings. Account type and
    broker_managed are always explicit, including for legacy frames.
    """
    if not isinstance(frame, (bytes, bytearray, memoryview)):
        raise WireError("frame must be bytes")
    frame = bytes(frame)
    if len(frame) < HEADER_SIZE or frame_size(frame[:HEADER_SIZE]) != len(frame):
        raise WireError("invalid or incomplete frame")
    _, version, kind, _, _, generation, sequence = _HEADER.unpack_from(frame)
    result = {"type": kind, "generation": generation, "sequence": sequence}
    reader = _Reader(frame[HEADER_SIZE:], version)
    if kind == HELLO:
        result["client_id"] = reader.get("Q")
        result["token"] = reader.opaque(_MAX_TOKEN_BYTES)
        result["instruments"] = _read_instruments(reader)
    elif kind == WELCOME:
        result["instruments"] = _read_instruments(reader)
        result["account"] = _read_account(reader)
    elif kind == NEW_ORDER:
        result["order_id"] = reader.get("Q")
        result["request"] = dict(zip(("instrument_id", "side", "limit_price", "quantity"),
                                       reader.get("QBqq")))
        result["request"]["position_effect"] = reader.get("B") if version >= 2 else 0
    elif kind == CANCEL_ORDER:
        result["order_id"], result["cancel_id"] = reader.get("QQ")
        result["broker_order_id"] = reader.opaque()
    elif kind == ORDER_UPDATE:
        result["notify_order"], result["has_fill"] = reader.boolean(), reader.boolean()
        result["order"] = _read_order(reader)
        result["kind"], result["cancel_request_id"] = reader.get("BQ")
        result["reason"] = _read_reason(reader)
        if result["has_fill"]:
            result["fill"] = _read_fill(reader)
        result["account"] = _read_account(reader)
    elif kind == ACCOUNT:
        result["account"] = _read_account(reader)
    elif kind == ERROR:
        result["error_code"] = reader.get("B")
        result["reason"] = _read_reason(reader)
    if reader.offset != len(reader.payload):
        raise WireError("trailing message payload")
    # Reuse the same semantic checks in both directions. Legacy absent fields
    # have already been given their C++ defaults by the reader.
    _write_payload(_Writer(), result)
    return result

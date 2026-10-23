"""Wire compatibility and corruption checks without native CTP or networking."""

import importlib.util
from pathlib import Path
import struct
import unittest


PROJECT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "ctp_trading_wire", PROJECT / "scripts/ctp_trading_wire.py")
wire = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(wire)

# MakeInstrumentId("DCE", "a1805"); fixed independently of Python helpers.
FUTURE = 0x0000000580004AF5
EQUITY = 0x00000001000927C0  # SSE:600000.


def message(message_type, **fields):
    return dict(type=message_type, generation=27, sequence=1, **fields)


def account(broker_managed=True):
    return {
        "revision": 7, "type": 1, "broker_managed": broker_managed,
        "futures": dict(zip(
            ("balance", "equity", "available", "margin", "reserved",
             "realized_pnl", "unrealized_pnl", "commission"),
            (1000, 995, 975, 10, 10, -5, -5, 2))),
        "futures_positions": [],
    }


def position(identity=FUTURE):
    return {
        "instrument_id": identity,
        "long_today": {"quantity": 12, "frozen_qty": 2, "open_notional": 43200},
        "long_yesterday": {"quantity": 3, "frozen_qty": 1, "open_notional": 10797},
        "short_today": {"quantity": 0, "frozen_qty": 0, "open_notional": 0},
        "short_yesterday": {"quantity": 1, "frozen_qty": 0, "open_notional": 3605},
        "margin": 10, "unrealized_pnl": -5,
        "unallocated_long_frozen": 4, "unallocated_short_frozen": 1,
    }


def order(**changes):
    result = {
        "client_order_id": 17, "strategy_order_id": 0, "adapter_order_id": b"B\0O",
        "instrument_id": FUTURE, "side": 1, "limit_price": 3600,
        "original_qty": 10, "cumulative_filled_qty": 2, "leaves_qty": 8,
        "state": 1, "cancel_pending": True, "cancel_request_id": 9,
        "report_sequence": 4, "receive_time_ns": 1234, "position_effect": 4,
    }
    result.update(changes)
    return result


def fill(**changes):
    result = {
        "client_order_id": 17, "strategy_order_id": 0, "adapter_order_id": b"B\0O", "execution_id": b"E\0",
        "instrument_id": FUTURE, "side": 1, "price": 3599, "quantity": 2,
        "event_time_ns": 999, "receive_time_ns": 1234,
        "has_event_time": True, "position_effect": 4,
    }
    result.update(changes)
    return result


def update(**changes):
    result = message(wire.ORDER_UPDATE, notify_order=True, order=order(), kind=2,
                     cancel_request_id=9,
                     reason={"code": -4, "text": b"diagnostic", "truncated": False},
                     fill=fill(), account=account())
    result.update(changes)
    return result


def framed(kind, payload, version=5):
    # Independent framing helper for malformed and legacy payloads.
    return struct.pack("<IHHIIQQ", 0x52545159, version, kind,
                       32 + len(payload), 0, 27, 1) + payload


class GoldenLayoutTest(unittest.TestCase):
    def test_cpp_header_little_endian_and_fixed_size(self):
        expected = bytes.fromhex(
            "59515452 0500 0900 20000000 00000000 "
            "0102030405060708 1112131415161718")
        value = {"type": wire.HEARTBEAT, "generation": 0x0807060504030201,
                 "sequence": 0x1817161514131211}
        self.assertEqual(wire.encode_message(value), expected)
        self.assertEqual(wire.frame_size(expected), 32)
        self.assertEqual(wire.decode_message(expected), value)

    def test_cpp_new_order_layout_includes_generic_close(self):
        expected = bytes.fromhex(
            "59515452 0500 0300 4a000000 00000000 "
            "1b00000000000000 0100000000000000 "
            "1100000000000000 f54a008005000000 01 "
            "100e000000000000 0200000000000000 04 0000000000000000")
        value = message(wire.NEW_ORDER, client_order_id=17, strategy_order_id=0, request={
            "instrument_id": FUTURE, "side": 1, "limit_price": 3600,
            "quantity": 2, "position_effect": 4})
        self.assertEqual(wire.encode_message(value), expected)
        self.assertEqual(wire.decode_message(expected), value)

    def test_cpp_account_v3_places_broker_identity_after_type(self):
        expected = bytes.fromhex(
            "59515452 0500 0600 6c000000 00000000 "
            "1b00000000000000 0100000000000000 "
            "0700000000000000 01 01 "
            "e803000000000000 e303000000000000 cf03000000000000 "
            "0a00000000000000 0a00000000000000 fbffffffffffffff "
            "fbffffffffffffff 0200000000000000 0000")
        value = message(wire.ACCOUNT, account=account())
        self.assertEqual(wire.encode_message(value), expected)
        self.assertEqual(wire.decode_message(expected), value)

    def test_cpp_order_fill_and_position_field_order(self):
        value = update()
        value["account"]["futures_positions"] = [position()]
        expected_order = (
            struct.pack("<Q", 17) + b"\x03B\0O"
            + struct.pack("<QB4qBB3QB", FUTURE, 1, 3600, 10, 2, 8,
                          1, 1, 9, 4, 1234, 4) + struct.pack("<Q", 0))
        expected_fill = (
            struct.pack("<Q", 17) + b"\x03B\0O\x02E\0"
            + struct.pack("<QB2q2QBB", FUTURE, 1, 3599, 2, 999, 1234, 1, 4) + struct.pack("<Q", 0))
        expected_account = (struct.pack("<QBB8qH", 7, 1, 1,
                                        1000, 995, 975, 10, 10, -5, -5, 2, 1)
                            + struct.pack("<Q16q", FUTURE,
                                          12, 2, 43200, 3, 1, 10797,
                                          0, 0, 0, 1, 0, 3605, 10, -5, 4, 1))
        payload = (b"\x01\x01" + expected_order + struct.pack("<BQ", 2, 9)
                   + struct.pack("<iBB", -4, 0, 10) + b"diagnostic"
                   + expected_fill + expected_account)
        self.assertEqual(wire.encode_message(value), framed(5, payload))
        decoded = wire.decode_message(framed(5, payload))
        self.assertEqual(decoded["order"], value["order"])
        self.assertEqual(decoded["fill"], value["fill"])
        self.assertEqual(decoded["account"], value["account"])
        self.assertTrue(decoded["has_fill"])

    def test_legacy_v1_cash_account_and_v2_futures_identity(self):
        cash = framed(6, struct.pack("<Q3qH", 9, 100, 10, 90, 0), version=1)
        self.assertEqual(wire.decode_message(cash)["account"], {
            "revision": 9, "type": 0, "broker_managed": False,
            "cash": {"balance": 100, "reserved": 10, "available": 90},
            "positions": []})
        futures = framed(6, struct.pack("<QB8qH", 7, 1, 1000, 995, 975,
                                        10, 10, -5, -5, 2, 0), version=2)
        self.assertEqual(wire.decode_message(futures)["account"], account(False))
        # Legacy v2 position records omit the two broker freeze fields.
        legacy_position = (struct.pack("<QB8qH", 7, 1, 1000, 995, 975,
                                        10, 10, -5, -5, 2, 1)
                           + struct.pack("<Q14q", FUTURE,
                                         12, 2, 43200, 3, 1, 10797,
                                         0, 0, 0, 1, 0, 3605, 10, -5))
        decoded = wire.decode_message(framed(6, legacy_position, version=2))
        self.assertEqual(decoded["account"]["futures_positions"][0]
                         ["unallocated_long_frozen"], 0)
        self.assertEqual(decoded["account"]["futures_positions"][0]
                         ["unallocated_short_frozen"], 0)
        old_order = framed(3, struct.pack("<QQB2q", 17, EQUITY, 1, 120, 100),
                           version=1)
        self.assertEqual(wire.decode_message(old_order)["request"]["position_effect"], 0)


class MessageValuesTest(unittest.TestCase):
    def test_all_ten_message_types_roundtrip_and_keep_owned_bytes(self):
        dictionary = [{"id": FUTURE, "price_decimals": 0}]
        cases = [
            message(wire.HELLO, client_id=3, token=b"secret", instruments=dictionary),
            message(wire.WELCOME, instruments=dictionary, account=account()),
            message(wire.NEW_ORDER, client_order_id=17, request={
                "instrument_id": FUTURE, "side": 2, "limit_price": 3600,
                "quantity": 10, "position_effect": 1}),
            message(wire.CANCEL_ORDER, client_order_id=17, cancel_id=9,
                    adapter_order_id=b"B\0O"),
            update(), message(wire.ACCOUNT, account=account()),
            message(wire.STOP), message(wire.DRAINED), message(wire.HEARTBEAT),
            message(wire.ERROR, error_code=1,
                    reason={"code": -3, "text": b"failed", "truncated": False}),
        ]
        for value in cases:
            with self.subTest(kind=value["type"]):
                encoded = wire.encode_message(value)
                decoded = wire.decode_message(encoded)
                self.assertEqual(wire.encode_message(decoded), encoded)
        borrowed = bytearray(wire.encode_message(cases[3]))
        decoded = wire.decode_message(borrowed)
        borrowed[-1] = 0xff
        self.assertEqual(decoded["adapter_order_id"], b"B\0O")

    def test_optional_fill_and_defaults(self):
        value = update(kind=0, fill=None)
        decoded = wire.decode_message(wire.encode_message(value))
        self.assertFalse(decoded["has_fill"])
        self.assertNotIn("fill", decoded)
        value["has_fill"] = True
        with self.assertRaises(wire.WireError):
            wire.encode_message(value)
        decoded = wire.decode_message(wire.encode_message(message(
            wire.CANCEL_ORDER, client_order_id=1, cancel_id=2)))
        self.assertEqual(decoded["adapter_order_id"], b"")

    def test_local_cancel_not_sent_roundtrip_and_legacy_rejection(self):
        value = update(kind=6, fill=None, order=order(
            state=2, cumulative_filled_qty=10, leaves_qty=0,
            cancel_pending=False))
        encoded = wire.encode_message(value)
        decoded = wire.decode_message(encoded)
        self.assertEqual(decoded["kind"], 6)
        self.assertEqual(decoded["order"], value["order"])
        self.assertEqual(decoded["cancel_request_id"], 9)
        self.assertFalse(decoded["has_fill"])
        legacy = bytearray(encoded)
        struct.pack_into("<H", legacy, 4, 4)
        with self.assertRaises(wire.WireError):
            wire.decode_message(legacy)
        value["order"]["cancel_request_id"] = 0
        value["cancel_request_id"] = 0
        self.assertEqual(wire.decode_message(wire.encode_message(value))
                         ["cancel_request_id"], 0)

    def test_local_cancel_not_sent_requires_ended_matching_attempt(self):
        for changes in ({"state": 1, "cumulative_filled_qty": 2,
                         "leaves_qty": 8}, {"cancel_pending": True},
                        {"cancel_request_id": 10}):
            local = order(state=2, cumulative_filled_qty=10, leaves_qty=0,
                          cancel_pending=False)
            local.update(changes)
            with self.subTest(changes=changes), self.assertRaises(wire.WireError):
                wire.encode_message(update(kind=6, fill=None, order=local))

    def test_version_four_keeps_strategy_identity_layout(self):
        value = update()
        value["order"]["strategy_order_id"] = 3
        value["fill"]["strategy_order_id"] = 3
        encoded = bytearray(wire.encode_message(value))
        struct.pack_into("<H", encoded, 4, 4)
        decoded = wire.decode_message(encoded)
        self.assertEqual(decoded["order"], value["order"])
        self.assertEqual(decoded["fill"], value["fill"])

    def test_utf8_and_maximum_opaque_and_reason_bytes(self):
        value = update()
        value["order"]["adapter_order_id"] = b"\0" * 63 + b"x"
        value["fill"]["adapter_order_id"] = b"\0" * 63 + b"x"
        value["fill"]["execution_id"] = b"\xff" * 64
        value["reason"] = {"code": -(1 << 31), "text": b"r" * 128,
                           "truncated": True}
        decoded = wire.decode_message(wire.encode_message(value))
        self.assertEqual(decoded["fill"], value["fill"])
        self.assertEqual(decoded["reason"], value["reason"])
        value = message(wire.ERROR, error_code=7, reason={"text": "错误"})
        self.assertEqual(wire.decode_message(wire.encode_message(value))
                         ["reason"]["text"], "错误".encode())

    def test_full_independent_dictionary_and_position_bounds(self):
        value = account()
        # Valid DCE aYYMM identities with two-digit years and 12 months.
        identities = [((5 << 32) | 0x80004000 | (1000 + (i // 12) * 100
                                                + i % 12 + 1)) for i in range(64)]
        value["futures_positions"] = [position(identity) for identity in identities]
        encoded = wire.encode_message(message(
            wire.WELCOME, account=value,
            instruments=[{"id": identity, "price_decimals": i % 7}
                         for i, identity in enumerate(identities)]))
        self.assertGreater(len(encoded), 4096)
        self.assertLessEqual(len(encoded), 16384)
        self.assertEqual(len(wire.decode_message(encoded)["account"]
                             ["futures_positions"]), 64)

    def test_broker_account_preserves_reported_numbers_without_simulator_algebra(self):
        value = account(True)
        value["futures"]["equity"] = 1100
        value["futures"]["available"] = 945
        value["futures"]["reserved"] = 7
        encoded = wire.encode_message(message(wire.ACCOUNT, account=value))
        self.assertEqual(wire.decode_message(encoded)["account"], value)
        value["broker_managed"] = False
        with self.assertRaisesRegex(wire.WireError, "identity"):
            wire.encode_message(message(wire.ACCOUNT, account=value))


class InvalidInputTest(unittest.TestCase):
    def assert_rejected(self, value):
        with self.assertRaises(wire.WireError):
            wire.encode_message(value)

    def test_header_range_reserved_sequence_and_simulation_rejection(self):
        good = wire.encode_message(message(wire.HEARTBEAT))
        for offset, form, invalid in ((0, "I", 0), (4, "H", 0), (4, "H", 6),
                                      (6, "H", 0), (6, "H", 11),
                                      (8, "I", 31), (8, "I", 16385),
                                      (12, "I", 1), (24, "Q", 0)):
            with self.subTest(offset=offset, invalid=invalid):
                bad = bytearray(good)
                struct.pack_into("<" + form, bad, offset, invalid)
                self.assertEqual(wire.frame_size(bad), 0)
                with self.assertRaises(wire.WireError):
                    wire.decode_message(bad)
        max_header = bytearray(good)
        struct.pack_into("<I", max_header, 8, 16384)
        self.assertEqual(wire.frame_size(max_header), 16384)
        for header in (good[:31], good + b"\0", "x" * 32, None):
            self.assertEqual(wire.frame_size(header), 0)
        self.assert_rejected(message(11))

    def test_all_partial_frames_and_trailing_bytes_rejected(self):
        frame = wire.encode_message(update())
        for size in range(len(frame)):
            with self.subTest(size=size), self.assertRaises(wire.WireError):
                wire.decode_message(frame[:size])
        with self.assertRaises(wire.WireError):
            wire.decode_message(frame + b"\0")
        # A truthful header length must not make trailing payload acceptable.
        with self.assertRaises(wire.WireError):
            wire.decode_message(framed(9, b"\0"))

    def test_noncanonical_payload_booleans_and_oversized_ids(self):
        frame = wire.encode_message(update())
        # Order broker length3 means cancel_pending is at absolute offset88.
        # Locate subsequent flags independently by their field widths.
        pending_offset = 32 + 2 + 8 + 1 + 3 + 8 + 1 + 32 + 1
        for offset in (32, 33, pending_offset):
            with self.subTest(offset=offset):
                bad = bytearray(frame)
                bad[offset] = 2
                with self.assertRaisesRegex(wire.WireError, "boolean"):
                    wire.decode_message(bad)
        acct = bytearray(wire.encode_message(message(wire.ACCOUNT, account=account())))
        acct[41] = 2
        with self.assertRaisesRegex(wire.WireError, "boolean"):
            wire.decode_message(acct)
        with self.assertRaises(wire.WireError):
            wire.decode_message(framed(4, struct.pack("<QQB", 1, 2, 65) + b"x" * 65))
        with self.assertRaises(wire.WireError):
            wire.decode_message(framed(10, struct.pack("<BiBB", 1, 0, 0, 129)
                                       + b"r" * 129))

    def test_invalid_numeric_types_enums_ranges_and_product_overflow(self):
        for key, invalid in (("sequence", 0), ("sequence", True),
                             ("sequence", 1 << 64), ("generation", -1),
                             ("type", 3.0)):
            value = message(wire.HEARTBEAT)
            value[key] = invalid
            self.assert_rejected(value)
        request = {"instrument_id": FUTURE, "side": 1, "limit_price": 3600,
                   "quantity": 2, "position_effect": 1}
        for key, invalid in (("side", 0), ("side", 3), ("side", True),
                             ("quantity", 0), ("quantity", 1.0),
                             ("limit_price", -1), ("limit_price", 1 << 63),
                             ("position_effect", 0), ("position_effect", 5),
                             ("instrument_id", 0),
                             ("instrument_id", (5 << 32) | 0x80000001),
                             ("instrument_id", (5 << 32) | 0x80004000 | 3613)):
            bad = dict(request, **{key: invalid})
            self.assert_rejected(message(wire.NEW_ORDER, client_order_id=17, request=bad))
        request["limit_price"], request["quantity"] = (1 << 63) - 1, 2
        self.assert_rejected(message(wire.NEW_ORDER, client_order_id=17, request=request))
        self.assert_rejected(message(wire.CANCEL_ORDER, client_order_id=1, cancel_id=0))
        self.assert_rejected(message(wire.CANCEL_ORDER, client_order_id=1, cancel_id=2,
                                     adapter_order_id=b"x" * 65))
        for venue in (4, 8):
            request = {"instrument_id": (FUTURE & 0xffffffff) | venue << 32,
                       "side": 1, "limit_price": 3600, "quantity": 2,
                       "position_effect": 4}
            self.assert_rejected(message(wire.NEW_ORDER, client_order_id=17, request=request))

    def test_hello_dictionary_token_bounds_and_identity_validation(self):
        base = message(wire.HELLO, client_id=3, token=b"secret",
                       instruments=[{"id": FUTURE, "price_decimals": 0}])
        for token in (b"", b"a\0b", b"x" * 65, 1):
            self.assert_rejected(dict(base, token=token))
        self.assert_rejected(dict(base, client_id=0))
        for entries in ([], base["instruments"] * 2, base["instruments"] * 65,
                        [{"id": FUTURE, "price_decimals": 7}],
                        [{"id": FUTURE, "price_decimals": True}],
                        [{"id": 0xff00000001, "price_decimals": 0}],
                        [{"id": (6 << 32) | 0x80004000 | 1612, "price_decimals": 0}],
                        [{"id": (5 << 32) | 0x80000000 | (27 << 14) | 3612,
                          "price_decimals": 0}]):
            self.assert_rejected(dict(base, instruments=entries))
        payload = struct.pack("<QB", 3, 3) + b"a\0b" + struct.pack("<HQB", 1, FUTURE, 0)
        with self.assertRaises(wire.WireError):
            wire.decode_message(framed(1, payload))

    def test_account_identity_counts_duplicate_positions_and_bucket_constraints(self):
        for field, bad in (("margin", -1), ("reserved", -1), ("commission", -1),
                           ("balance", True), ("equity", 1 << 63)):
            value = account()
            value["futures"][field] = bad
            self.assert_rejected(message(wire.ACCOUNT, account=value))
        for field in ("equity", "available"):
            value = account(False)
            value["futures"][field] += 1
            self.assert_rejected(message(wire.ACCOUNT, account=value))
        for positions in ([position(), position()], [position()] * 65,
                          [position(EQUITY)]):
            value = account()
            value["futures_positions"] = positions
            self.assert_rejected(message(wire.ACCOUNT, account=value))
        for field, bad in (("quantity", -1), ("frozen_qty", 13),
                           ("open_notional", -1)):
            value = account()
            value["futures_positions"] = [position()]
            value["futures_positions"][0]["long_today"][field] = bad
            self.assert_rejected(message(wire.ACCOUNT, account=value))
        value = account()
        value["futures_positions"] = [position()]
        value["futures_positions"][0]["long_today"]["quantity"] = (1 << 63) - 1
        self.assert_rejected(message(wire.ACCOUNT, account=value))
        for field, invalid in (("unallocated_long_frozen", 13),
                               ("unallocated_short_frozen", 2),
                               ("unallocated_long_frozen", -1)):
            value = account()
            value["futures_positions"] = [position()]
            value["futures_positions"][0][field] = invalid
            self.assert_rejected(message(wire.ACCOUNT, account=value))
        value = account(False)
        value["futures_positions"] = [position()]
        self.assert_rejected(message(wire.ACCOUNT, account=value))
        value = {"type": 0, "broker_managed": True}
        self.assert_rejected(message(wire.ACCOUNT, account=value))
        payload = struct.pack("<QBB8qH", 0, 1, 1, *([0] * 8), 65)
        with self.assertRaises(wire.WireError):
            wire.decode_message(framed(6, payload))

    def test_order_and_fill_consistency_and_boolean_types(self):
        for changed in (order(leaves_qty=9), order(state=2, leaves_qty=0),
                        order(cumulative_filled_qty=11), order(cancel_pending=1),
                        order(position_effect=0), order(original_qty=0)):
            self.assert_rejected(update(order=changed))
        for changed in (fill(quantity=3), fill(execution_id=b""),
                        fill(client_order_id=18), fill(instrument_id=EQUITY),
                        fill(side=2), fill(position_effect=3),
                        fill(adapter_order_id=b"other"),
                        fill(has_event_time=False), fill(has_event_time=1)):
            self.assert_rejected(update(fill=changed))
        for changed in ({"notify_order": 1}, {"kind": 6}, {"kind": 0},
                        {"reason": {"text": b"r" * 129}},
                        {"reason": {"truncated": 1}}, {"has_fill": False}):
            self.assert_rejected(update(**changed))
        # A gateway cancel result may use zero without an invented client ID.
        value = update(cancel_request_id=0,
                       order=order(cancel_request_id=0), fill=None, kind=3)
        self.assertEqual(wire.decode_message(wire.encode_message(value))
                         ["cancel_request_id"], 0)

    def test_malformed_account_identity_is_rejected_after_decode(self):
        value = account(False)
        encoded = bytearray(wire.encode_message(message(wire.ACCOUNT, account=value)))
        struct.pack_into("<q", encoded, 32 + 10 + 8, 994)
        with self.assertRaisesRegex(wire.WireError, "identity"):
            wire.decode_message(encoded)
        # The same malformed algebra is valid for broker-owned valuation.
        encoded[41] = 1
        self.assertEqual(wire.decode_message(encoded)["account"]["futures"]["equity"], 994)


if __name__ == "__main__":
    unittest.main()

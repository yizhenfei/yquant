"""Deterministic test-only CTP MD/Trader API with broker account queries.

Workflow tests copy this module into a temporary PYTHONPATH. Production scripts
never import this fixture. Order reports, trades, position details, fees and
cash all enter through the same SDK callbacks used by the real bridge.
"""

from copy import deepcopy
from datetime import datetime, timedelta, timezone
import json
import os
from pathlib import Path
import threading
from types import SimpleNamespace as Row


THOST_TERT_QUICK = 2
INITIAL_BALANCE = 123456.78


def audit(event, **fields):
    path = os.environ.get("YQUANT_FAKE_CTP_AUDIT")
    if path:
        with Path(path).open("a", encoding="utf-8") as output:
            output.write(json.dumps({"event": event, **fields}) + "\n")


def trading_day():
    return datetime.now(timezone(timedelta(hours=8))).strftime("%Y%m%d")


def info(code=0):
    return Row(ErrorID=code, ErrorMsg="fixture broker response")


class CThostFtdcMdSpi:
    pass


class CThostFtdcTraderSpi:
    pass


# Native field constructors are mutable records; only bridge-assigned fields
# are consumed by this fixture.
for _name in ("ReqUserLogin", "ReqAuthenticate", "SettlementInfoConfirm",
              "QryTradingAccount", "QryInvestorPosition", "QryInvestorPositionDetail",
              "QryOrder", "QryTrade", "InputOrder", "InputOrderAction"):
    globals()["CThostFtdc" + _name + "Field"] = type(
        "CThostFtdc" + _name + "Field", (), {})


class CThostFtdcMdApi:
    @staticmethod
    def CreateFtdcMdApi(*_args):
        return CThostFtdcMdApi()

    def __init__(self):
        self.stopping = threading.Event()
        self.thread = None

    def RegisterSpi(self, spi):
        self.spi = spi

    def RegisterFront(self, front):
        assert front in {f"tcp://182.254.243.31:{port}" for port in (30011, 30012, 30013, 40011)}

    def Init(self):
        self.spi.OnFrontConnected()

    def ReqUserLogin(self, request, request_id):
        fields = (request.BrokerID, request.UserID, request.Password)
        if os.environ.get("YQUANT_FAKE_CTP_ANONYMOUS_MD") == "1":
            assert fields == ("", "", "")
        else:
            assert fields == ("9999", "123456", "test-simnow-password")
        audit("md_login", anonymous=not fields[0])
        self.spi.OnRspUserLogin(Row(TradingDay=trading_day()), info(), request_id, True)
        return 0

    def SubscribeMarketData(self, symbols, count):
        assert count == 1 and isinstance(symbols[0], bytes)
        self.symbol = symbols[0].decode("ascii")
        self.spi.OnRspSubMarketData(Row(InstrumentID=self.symbol), info(), 2, True)
        self.thread = threading.Thread(target=self.emit, daemon=True)
        self.thread.start()
        return 0

    def emit(self):
        while not self.stopping.wait(0.03):
            now = datetime.now(timezone(timedelta(hours=8)))
            self.spi.OnRtnDepthMarketData(Row(
                InstrumentID=self.symbol, ExchangeID="DCE",
                ActionDay=now.strftime("%Y%m%d"), TradingDay=now.strftime("%Y%m%d"),
                UpdateTime=now.strftime("%H:%M:%S"), UpdateMillisec=now.microsecond // 1000,
                BidPrice1=3662.0, BidVolume1=70, AskPrice1=3664.0, AskVolume1=53))

    def Release(self):
        self.stopping.set()
        if self.thread is not None:
            self.thread.join(timeout=1)
        audit("md_release")


class CThostFtdcTraderApi:
    @staticmethod
    def CreateFtdcTraderApi(*_args):
        return CThostFtdcTraderApi()

    def __init__(self):
        self.orders = {}
        self.trades = []
        self.lots = []
        self.commission = 0.0
        self.realized = 0.0
        self.disconnected = False
        self.fills = 0
        self.symbol = "a2701"

    def RegisterSpi(self, spi):
        self.spi = spi

    def RegisterFront(self, front):
        assert front in {f"tcp://182.254.243.31:{port}" for port in (30001, 30002, 30003, 40001)}

    def SubscribePrivateTopic(self, mode):
        assert mode == THOST_TERT_QUICK

    def SubscribePublicTopic(self, mode):
        assert mode == THOST_TERT_QUICK

    def Init(self):
        self.spi.OnFrontConnected()

    def ReqAuthenticate(self, request, request_id):
        assert (request.BrokerID, request.UserID) == ("9999", "123456")
        audit("td_authenticate")
        self.spi.OnRspAuthenticate(Row(), info(), request_id, True)
        return 0

    def ReqUserLogin(self, request, request_id, *_mac_arguments):
        assert (request.BrokerID, request.UserID, request.Password) == (
            "9999", "123456", "test-simnow-password")
        audit("td_login")
        code = 7 if os.environ.get("YQUANT_FAKE_CTP_FAILURE") == "login" else 0
        self.spi.OnRspUserLogin(Row(TradingDay=trading_day(), FrontID=7,
                                    SessionID=11, MaxOrderRef="100"), info(code), request_id, True)
        return 0

    def ReqSettlementInfoConfirm(self, request, request_id):
        audit("td_settlement")
        self.spi.OnRspSettlementInfoConfirm(Row(), info(), request_id, True)
        return 0

    def _query(self, kind, rows, request_id):
        audit("td_query", kind=kind)
        callback = getattr(self.spi, "OnRspQry" + kind)
        if not rows:
            callback(None, info(), request_id, True)
        for index, row in enumerate(rows):
            callback(Row(**deepcopy(row)), info(), request_id, index == len(rows) - 1)
        return 0

    def ReqQryTradingAccount(self, request, request_id):
        # Distinct broker fees and margins prove the runtime uses these queries.
        margin = sum(lot["Volume"] for lot in self.lots) * 123.45
        reserved = sum(10.0 for order in self.orders.values()
                       if order["OrderStatus"] == "3" and order["CombOffsetFlag"] == "0")
        balance = INITIAL_BALANCE + self.realized - self.commission
        audit("td_account", balance=round(balance, 2), reserved=reserved,
              long_today=sum(lot["Volume"] for lot in self.lots if lot["Direction"] == "0"),
              short_today=sum(lot["Volume"] for lot in self.lots if lot["Direction"] == "1"))
        return self._query("TradingAccount", [{"CurrencyID": "CNY", "Balance": balance,
            "PositionProfit": 0.0, "Available": balance - margin - reserved,
            "CurrMargin": margin, "FrozenMargin": reserved, "FrozenCash": 0.0,
            "FrozenCommission": 0.0, "CloseProfit": self.realized,
            "Commission": self.commission}], request_id)

    def ReqQryInvestorPosition(self, request, request_id):
        rows = []
        for direction in ("0", "1"):
            qty = sum(lot["Volume"] for lot in self.lots if lot["Direction"] == direction)
            if qty:
                frozen = sum(order["VolumeTotalOriginal"] for order in self.orders.values()
                             if order["OrderStatus"] == "3" and order["CombOffsetFlag"] != "0"
                             and order["Direction"] != direction)
                rows.append({"InstrumentID": self.symbol, "ExchangeID": "DCE",
                    "Position": qty, "TodayPosition": qty, "PosiDirection": "2" if direction == "0" else "3",
                    "PositionDate": "1", "LongFrozen": frozen if direction == "1" else 0,
                    "ShortFrozen": frozen if direction == "0" else 0,
                    "UseMargin": qty * 123.45, "PositionProfit": 0.0, "HedgeFlag": "1"})
        return self._query("InvestorPosition", rows, request_id)

    def ReqQryInvestorPositionDetail(self, request, request_id):
        return self._query("InvestorPositionDetail", self.lots, request_id)

    def ReqQryOrder(self, request, request_id):
        return self._query("Order", list(self.orders.values()), request_id)

    def ReqQryTrade(self, request, request_id):
        return self._query("Trade", self.trades, request_id)

    def ReqOrderInsert(self, request, request_id):
        failure = os.environ.get("YQUANT_FAKE_CTP_FAILURE")
        audit("td_order", side=request.Direction, effect=request.CombOffsetFlag,
              quantity=request.VolumeTotalOriginal, limit_price=request.LimitPrice)
        if failure == "unknown_send":
            return 0  # The request was sent; no acknowledgment establishes its state.
        if failure == "disconnect":
            if not self.disconnected:
                self.disconnected = True
                self.spi.OnFrontDisconnected(0x1001)
            return 0
        self.symbol = request.InstrumentID
        row = {name: deepcopy(value) for name, value in vars(request).items()}
        row.update(FrontID=7, SessionID=11, OrderSysID=f" {len(self.orders) + 1:09d}",
                   OrderStatus="3", OrderSubmitStatus="3", VolumeTraded=0,
                   TradingDay=trading_day())
        self.orders[request.OrderRef] = row
        self.spi.OnRtnOrder(Row(**deepcopy(row)))
        if self.fills < 4:
            self._fill(row)
        return 0

    def _fill(self, order):
        quantity, direction = order["VolumeTotalOriginal"], order["Direction"]
        effect = order["CombOffsetFlag"]
        if effect == "0":
            self.lots.append({"InstrumentID": self.symbol, "ExchangeID": "DCE",
                              "Volume": quantity, "Direction": direction,
                              "OpenDate": trading_day(), "OpenPrice": order["LimitPrice"],
                              "HedgeFlag": "1"})
        else:
            remaining = quantity
            for lot in self.lots:
                if lot["Direction"] != direction:
                    closed = min(remaining, lot["Volume"])
                    sign = 1 if lot["Direction"] == "0" else -1
                    self.realized += (order["LimitPrice"] - lot["OpenPrice"]) * sign * closed * 10
                    lot["Volume"] -= closed
                    remaining -= closed
            assert remaining == 0
            self.lots = [lot for lot in self.lots if lot["Volume"]]
        self.fills += 1
        self.commission += quantity * 1.25
        trade = {"InstrumentID": self.symbol, "ExchangeID": "DCE",
                 "TradingDay": trading_day(), "OrderSysID": order["OrderSysID"],
                 "OrderRef": order["OrderRef"], "TradeID": str(self.fills),
                 "Direction": direction, "OffsetFlag": effect,
                 "Volume": quantity, "Price": order["LimitPrice"]}
        self.trades.append(trade)
        self.spi.OnRtnTrade(Row(**deepcopy(trade)))
        order.update(OrderStatus="0", VolumeTraded=quantity)
        self.spi.OnRtnOrder(Row(**deepcopy(order)))
        audit("td_fill", side=direction, effect=effect, quantity=quantity)

    def ReqOrderAction(self, request, request_id):
        order = next((row for row in self.orders.values()
                      if (getattr(request, "OrderSysID", None) == row["OrderSysID"] or
                          getattr(request, "OrderRef", None) == row["OrderRef"])), None)
        assert order is not None and order["OrderStatus"] == "3"
        order["OrderStatus"] = "5"
        self.spi.OnRtnOrder(Row(**deepcopy(order)))
        audit("td_cancel", side=order["Direction"], effect=order["CombOffsetFlag"])
        return 0

    def Release(self):
        audit("td_release", active=sum(row["OrderStatus"] == "3" for row in self.orders.values()),
              fills=self.fills)

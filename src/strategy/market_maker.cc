#include "yquant/strategy/market_maker.h"

#include <cmath>
#include <limits>

namespace yquant::strategy {

using trading::OrderState;
using trading::RequestResult;
using trading::Side;

MarketMaker::MarketMaker(trading::TradingClient& client,
                         trading::OrderIdGenerator& ids,
                         std::uint64_t account_id, std::uint64_t strategy_id,
                         std::uint32_t instrument_id, std::int32_t quantity)
    : client_(client), ids_(ids), account_(account_id), strategy_(strategy_id),
      instrument_(instrument_id), quantity_(quantity) {}

void MarketMaker::OnQuote(const model::Quote& quote) {
  if (quote.instrument_id != instrument_ || quantity_ <= 0 ||
      !std::isfinite(quote.bid_price) || !std::isfinite(quote.ask_price) ||
      quote.bid_price <= 0 || quote.bid_price >= quote.ask_price ||
      quote.bid_quantity <= 0 || quote.ask_quantity <= 0) return;
  if (bid_.target != quote.bid_price) bid_.failed_target = 0;
  if (ask_.target != quote.ask_price) ask_.failed_target = 0;
  bid_.target = quote.bid_price;
  ask_.target = quote.ask_price;
  Requote();
}

void MarketMaker::Requote() {
  Advance(bid_, ask_, Side::kBuy);
  Advance(ask_, bid_, Side::kSell);
}

void MarketMaker::Advance(Leg& leg, const Leg& opposite, Side side) {
  if (leg.target == 0 || leg.target == leg.failed_target) return;
  if (leg.phase == Phase::kWorking && leg.price != leg.target) {
    if (leg.cancel_id == std::numeric_limits<std::uint64_t>::max()) return;
    leg.cancel_target = leg.target;
    if (client_.CancelOrder({leg.id, account_, strategy_, ++leg.cancel_id},
                            *this) == RequestResult::kDelivered) {
      leg.phase = Phase::kCanceling;
    } else {
      leg.failed_target = leg.target;
    }
    return;
  }
  if (leg.phase != Phase::kIdle) return;
  if (opposite.phase != Phase::kIdle &&
      (side == Side::kBuy ? leg.target >= opposite.price
                          : leg.target <= opposite.price)) return;
  trading::FundsSnapshot funds;
  if (!client_.QueryFunds({account_}, funds)) return;
  const auto id = ids_.Next();
  if (!id) {
    leg.failed_target = leg.target;
    return;
  }
  leg.id = *id;
  leg.price = leg.target;
  leg.phase = Phase::kSubmitting;
  if (client_.SubmitOrder({{*id, account_, strategy_, instrument_, side,
                            trading::Offset::kOpen, leg.price, quantity_}},
                          *this) != RequestResult::kDelivered) {
    Retire(leg, leg.price);
  }
}

MarketMaker::Leg* MarketMaker::Find(trading::OrderId id) {
  if (bid_.phase != Phase::kIdle && bid_.id == id) return &bid_;
  if (ask_.phase != Phase::kIdle && ask_.id == id) return &ask_;
  return nullptr;
}

void MarketMaker::Retire(Leg& leg, double failed_target) {
  const auto target = leg.target;
  leg = {};
  leg.target = target;
  leg.failed_target = failed_target;
}

void MarketMaker::Apply(const trading::OrderSnapshot& snapshot) {
  auto* leg = Find(snapshot.order.order_id);
  if (!leg) return;
  switch (snapshot.state) {
    case OrderState::kAccepted:
    case OrderState::kPartiallyFilled:
      if (leg->phase == Phase::kSubmitting) leg->phase = Phase::kWorking;
      break;
    case OrderState::kRejected:
      Retire(*leg, leg->price);
      break;
    case OrderState::kFilled:
    case OrderState::kCanceled:
      if (leg->phase == Phase::kCanceling) {
        leg->terminal = true;
      } else {
        Retire(*leg);
      }
      break;
    default:
      return;
  }
  Requote();
}

void MarketMaker::OnOrderReport(const trading::OrderReport& report) {
  Apply(report.snapshot);
}

void MarketMaker::OnTradeReport(const trading::TradeReport& report) {
  if (Find(report.snapshot.order.order_id)) {
    Apply(report.snapshot);
  } else {
    // A late detail can make account funds usable after its order was retired.
    Requote();
  }
}

void MarketMaker::OnCancelReport(const trading::CancelReport& report) {
  auto* leg = Find(report.order_id);
  if (!leg || leg->phase != Phase::kCanceling ||
      leg->cancel_id != report.cancel_request_id) return;
  if (report.result == trading::CancelResult::kCanceled || leg->terminal ||
      report.snapshot.state == OrderState::kFilled ||
      report.snapshot.state == OrderState::kCanceled) {
    Retire(*leg);
  } else {
    leg->phase = Phase::kWorking;
    leg->failed_target = leg->cancel_target;
  }
  Requote();
}

void MarketMaker::OnRequestFailure(const trading::RequestFailure& failure) {
  auto* leg = Find(failure.order_id);
  if (!leg) return;
  // A send failure can be followed by a valid counter report for the same ID.
  if (!failure.cancel_request_id &&
      failure.reason == trading::FailureReason::kSendFailed) return;
  if (!failure.cancel_request_id) {
    Retire(*leg, leg->price);
  } else if (leg->phase == Phase::kCanceling &&
             leg->cancel_id == *failure.cancel_request_id) {
    if (leg->terminal || (failure.snapshot &&
        (failure.snapshot->state == OrderState::kFilled ||
         failure.snapshot->state == OrderState::kCanceled))) {
      Retire(*leg);
    } else {
      leg->phase = Phase::kWorking;
      leg->failed_target = leg->cancel_target;
    }
  }
  Requote();
}

}  // namespace yquant::strategy

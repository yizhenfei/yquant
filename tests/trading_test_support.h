#ifndef YQUANT_TESTS_TRADING_TEST_SUPPORT_H_
#define YQUANT_TESTS_TRADING_TEST_SUPPORT_H_

#include <cstdint>
#include <initializer_list>

#include "gtest/gtest.h"
#include "yquant/model/currency.h"
#include "yquant/trading/funds.h"
#include "yquant/trading/funds_terms.h"
#include "yquant/trading/simulated_trading_adapter.h"
#include "yquant/trading/trading_adapter.h"
#include "yquant/trading/trading_service.h"

namespace yquant::trading::test {

// Gives tests of unrelated order behavior explicitly known zero trading costs.
inline void ConfigureZeroCostTerms(TradingService& service, std::uint64_t id) {
  FundsTerms terms;
  terms.currency = model::Currency("CNY");
  terms.contract_multiplier = 1;
  terms.margin_rate = 0;
  for (auto& fee : terms.fees) {
    fee = FeeRate{};
  }
  for (std::uint32_t instrument : {42U, 81U}) {
    for (Side side : {Side::kBuy, Side::kSell}) {
      EXPECT_TRUE(service.ConfigureFundsTerms(id, instrument, side, terms));
    }
  }
}

// Publishes real available funds before a test starts submitting orders.
inline void PrepareTradingAccount(TradingService& service, std::uint64_t id,
                                  SimulatedTradingAdapter& adapter) {
  ConfigureZeroCostTerms(service, id);
  adapter.QueueFunds({1, FundsRefreshStarted{}});
  adapter.QueueFunds(
      {1, FundsInfo{model::Currency("CNY"), 1000000, 1000000, 0, 0}});
  adapter.Run();
}

}  // namespace yquant::trading::test

#endif  // YQUANT_TESTS_TRADING_TEST_SUPPORT_H_

#include "strategyd/strategy_context.h"

#include <stddef.h>
#include <stdint.h>

#include "gtest/gtest.h"
#include "mock_trading_service.h"
#include "strategyd/market_data.h"
#include "strategyd/timer.h"
#include "yquant/model/identifiers.h"
#include "yquant/trading/trading_engine.h"

namespace yquant::strategyd {
namespace {

using base::ErrorCode;

class ContextTestClock final : public base::MonotonicClock {
 public:
  uint64_t Now() const noexcept override { return now; }
  uint64_t now = 1000;
};

// Two dictionary entries allow testing a known but unsubscribed instrument.
md::MarketDataConfig ContextConfig(model::SourceId source_id = {1}) {
  md::MarketDataConfig config;
  config.source_id = source_id;

  config.instrument_count = 2;
  const char* symbols[] = {"600000", "600001"};
  for (size_t index = 0; index < config.instrument_count; ++index) {
    auto& item = config.instruments[index];
    EXPECT_EQ(
        model::MakeInstrumentId("SSE", symbols[index], item.instrument.id),
        ErrorCode::kOk);
    item.instrument.venue = model::Venue::kSse;
    for (size_t digit = 0; digit < model::kSymbolLength; ++digit) {
      item.instrument.symbol[digit] = symbols[index][digit];
    }
    item.instrument.price_decimals = 2;
  }
  return config;
}

model::QuoteTick ContextQuote(model::InstrumentId id, uint64_t sequence,
                              model::SourceId source_id = {1}) {
  model::QuoteTick quote;
  quote.source_id = source_id;
  quote.instrument_id = id;
  quote.sequence = sequence;
  quote.receive_time_ns = 314;
  quote.bid_price = 1000;
  quote.bid_qty = 100;
  quote.ask_price = 1001;
  quote.ask_qty = 200;
  quote.flags = 1 | 2 | 4;
  return quote;
}

TEST(StrategyContextTest, EmptyDictionaryPreservesOutputsAndHasNoSource) {
  ContextTestClock clock;
  MarketDataImpl market_data(clock);
  testing::MockTradingService trading;
  TimerImpl timer;
  StrategyContextImpl context(clock, market_data, trading, timer);
  const auto definition = ContextConfig().instruments[0].instrument;
  auto unchanged_definition = definition;
  EXPECT_EQ(context.market_data().FindInstrument("SSE", "600000",
                                                 unchanged_definition),
            ErrorCode::kInvalidArgument);
  EXPECT_EQ(unchanged_definition, definition);
  auto untouched = ContextQuote(definition.id, 99);
  const auto original = untouched;
  EXPECT_FALSE(
      context.market_data().TryGetLatestQuote(definition.id, untouched));
  EXPECT_EQ(untouched, original);
  EXPECT_EQ(context.market_data().SubscribeQuotes(definition.id),
            ErrorCode::kInvalidState);
  market_data.BeginCallback(strategy::CallbackPhase::kStart);
  EXPECT_EQ(context.market_data().SubscribeQuotes(definition.id),
            ErrorCode::kUnsupported);
  market_data.EndCallback();
  EXPECT_TRUE(market_data.subscriptions().empty());
  EXPECT_EQ(context.Now(), 1000U);
  clock.now = 1234;
  EXPECT_EQ(context.Now(), 1234U);
}

TEST(StrategyContextTest, ResolvesDefinitionsAndRegistersOnlyDuringStart) {
  ContextTestClock clock;
  MarketDataImpl market_data(clock);
  testing::MockTradingService trading;
  TimerImpl timer;
  StrategyContextImpl context(clock, market_data, trading, timer);
  const auto config = ContextConfig();
  ASSERT_TRUE(market_data.Configure(config).ok());
  const auto definition = config.instruments[0].instrument;
  model::InstrumentDefinition found;
  ASSERT_EQ(context.market_data().FindInstrument("SSE", "600000", found),
            ErrorCode::kOk);
  EXPECT_EQ(found, definition);
  EXPECT_EQ(context.market_data().FindInstrument("SZSE", "600000", found),
            ErrorCode::kInvalidArgument);
  EXPECT_EQ(found, definition);
  market_data.BeginCallback(strategy::CallbackPhase::kStart);
  EXPECT_EQ(context.market_data().SubscribeQuotes({definition.id.value + 2}),
            ErrorCode::kInvalidArgument);
  EXPECT_EQ(context.market_data().SubscribeQuotes({}),
            ErrorCode::kInvalidArgument);
  EXPECT_TRUE(market_data.subscriptions().empty());
  ASSERT_EQ(context.market_data().SubscribeQuotes(definition.id),
            ErrorCode::kOk);
  ASSERT_EQ(context.market_data().SubscribeQuotes(definition.id),
            ErrorCode::kOk);
  ASSERT_EQ(market_data.subscriptions().size(), 1U);
  EXPECT_EQ(market_data.subscriptions()[0], definition.id);
  market_data.EndCallback();
  for (const auto phase :
       {strategy::CallbackPhase::kNone, strategy::CallbackPhase::kTick,
        strategy::CallbackPhase::kOrderUpdate, strategy::CallbackPhase::kFill,
        strategy::CallbackPhase::kStop}) {
    market_data.BeginCallback(phase);
    EXPECT_EQ(context.market_data().SubscribeQuotes(
                  config.instruments[1].instrument.id),
              ErrorCode::kInvalidState);
    EXPECT_EQ(market_data.subscriptions().size(), 1U);
    market_data.EndCallback();
  }
}

TEST(StrategyContextTest, KeepsAcceptedSnapshotAndRejectsMismatchedSource) {
  ContextTestClock clock;
  MarketDataImpl market_data(clock);
  testing::MockTradingService trading;
  TimerImpl timer;
  StrategyContextImpl context(clock, market_data, trading, timer);
  const strategy::StrategyContext& read_only_context = context;
  const auto config = ContextConfig({17});
  ASSERT_TRUE(market_data.Configure(config).ok());
  const auto id = config.instruments[0].instrument.id;
  market_data.BeginCallback(strategy::CallbackPhase::kStart);
  ASSERT_EQ(context.market_data().SubscribeQuotes(id), ErrorCode::kOk);
  market_data.EndCallback();
  auto snapshot = ContextQuote(id, 99);
  const auto sentinel = snapshot;
  EXPECT_FALSE(read_only_context.market_data().TryGetLatestQuote(id, snapshot));
  EXPECT_EQ(snapshot, sentinel);
  const auto first = ContextQuote(id, 2, config.source_id);
  bool accepted = false;
  ASSERT_EQ(market_data.ApplyQuote(first, accepted), ErrorCode::kOk);
  ASSERT_TRUE(accepted);
  ASSERT_TRUE(read_only_context.market_data().TryGetLatestQuote(id, snapshot));
  EXPECT_EQ(snapshot, first);
  EXPECT_EQ(snapshot.receive_time_ns, 314U);
  EXPECT_EQ(context.Now(), 1000U);
  auto old = ContextQuote(id, 1, config.source_id);
  old.bid_price = 500;
  accepted = true;
  EXPECT_EQ(market_data.ApplyQuote(old, accepted), ErrorCode::kOk);
  EXPECT_FALSE(accepted);
  EXPECT_TRUE(read_only_context.market_data().TryGetLatestQuote(id, snapshot));
  EXPECT_EQ(snapshot, first);
  const auto invalid = ContextQuote(id, 4, {2});
  for (const bool initial_accepted : {false, true}) {
    accepted = initial_accepted;
    EXPECT_EQ(market_data.ApplyQuote(invalid, accepted), ErrorCode::kCorrupt);
    EXPECT_EQ(accepted, initial_accepted);
    ASSERT_TRUE(
        read_only_context.market_data().TryGetLatestQuote(id, snapshot));
    EXPECT_EQ(snapshot, first);
  }
  EXPECT_FALSE(read_only_context.market_data().TryGetLatestQuote({}, snapshot));
  EXPECT_EQ(snapshot, first);
  EXPECT_FALSE(read_only_context.market_data().TryGetLatestQuote({id.value + 2},
                                                                 snapshot));
  EXPECT_EQ(snapshot, first);
  EXPECT_FALSE(read_only_context.market_data().TryGetLatestQuote(
      config.instruments[1].instrument.id, snapshot));
  EXPECT_EQ(snapshot, first);
  const auto next = ContextQuote(id, 3, config.source_id);
  accepted = false;
  ASSERT_EQ(market_data.ApplyQuote(next, accepted), ErrorCode::kOk);
  EXPECT_TRUE(accepted);
  ASSERT_TRUE(read_only_context.market_data().TryGetLatestQuote(id, snapshot));
  EXPECT_EQ(snapshot, next);
}

TEST(StrategyContextTest, BoundServicesKeepTheSameInstrumentIndependent) {
  ContextTestClock clock;
  MarketDataImpl first(clock);
  MarketDataImpl second(clock);
  const auto first_config = ContextConfig({1});
  const auto second_config = ContextConfig({17});
  ASSERT_TRUE(first.Configure(first_config).ok());
  ASSERT_TRUE(second.Configure(second_config).ok());
  const auto id = first_config.instruments[0].instrument.id;
  ASSERT_EQ(id, second_config.instruments[0].instrument.id);
  strategy::MarketData& first_service = first;
  strategy::MarketData& second_service = second;
  first.BeginCallback(strategy::CallbackPhase::kStart);
  ASSERT_EQ(first_service.SubscribeQuotes(id), ErrorCode::kOk);
  first.EndCallback();
  second.BeginCallback(strategy::CallbackPhase::kStart);
  ASSERT_EQ(second_service.SubscribeQuotes(id), ErrorCode::kOk);
  second.EndCallback();

  const auto first_quote = ContextQuote(id, 9, first_config.source_id);
  auto second_quote = ContextQuote(id, 1, second_config.source_id);
  second_quote.bid_price = 500;
  bool accepted = false;
  ASSERT_EQ(first.ApplyQuote(first_quote, accepted), ErrorCode::kOk);
  ASSERT_TRUE(accepted);
  auto snapshot = first_quote;
  EXPECT_FALSE(second_service.TryGetLatestQuote(id, snapshot));
  EXPECT_EQ(snapshot, first_quote);
  ASSERT_EQ(second.ApplyQuote(second_quote, accepted), ErrorCode::kOk);
  ASSERT_TRUE(accepted);
  ASSERT_TRUE(first_service.TryGetLatestQuote(id, snapshot));
  EXPECT_EQ(snapshot, first_quote);
  ASSERT_TRUE(second_service.TryGetLatestQuote(id, snapshot));
  EXPECT_EQ(snapshot, second_quote);
}

TEST(StrategyContextTest,
     QuoteTickAndQuantityStepFailuresPreserveCacheAndOutput) {
  ContextTestClock clock;
  MarketDataImpl market_data(clock);
  auto config = ContextConfig();
  auto& item = config.instruments[0];
  item.instrument.price_tick = 2;
  item.instrument.quantity_step = 5;

  ASSERT_TRUE(market_data.Configure(config).ok());
  const auto id = item.instrument.id;
  market_data.BeginCallback(strategy::CallbackPhase::kStart);
  ASSERT_EQ(market_data.SubscribeQuotes(id), ErrorCode::kOk);
  market_data.EndCallback();
  auto valid = ContextQuote(id, 2);
  valid.ask_price = 1002;
  bool accepted = false;
  ASSERT_EQ(market_data.ApplyQuote(valid, accepted), ErrorCode::kOk);
  ASSERT_TRUE(accepted);
  for (size_t field = 0; field != 4; ++field) {
    auto invalid = valid;
    invalid.sequence = 3;
    if (field == 0) {
      ++invalid.bid_price;
    }
    if (field == 1) {
      ++invalid.ask_price;
    }
    if (field == 2) {
      ++invalid.bid_qty;
    }
    if (field == 3) {
      ++invalid.ask_qty;
    }
    accepted = true;
    EXPECT_EQ(market_data.ApplyQuote(invalid, accepted), ErrorCode::kCorrupt);
    EXPECT_TRUE(accepted);
    model::QuoteTick cached;
    ASSERT_TRUE(market_data.TryGetLatestQuote(id, cached));
    EXPECT_EQ(cached, valid);
    // Old report sequences do not bypass structural/unit validation either.
    invalid.sequence = 1;
    EXPECT_EQ(market_data.ApplyQuote(invalid, accepted), ErrorCode::kCorrupt);
    EXPECT_TRUE(accepted);
  }
}

}  // namespace
}  // namespace yquant::strategyd

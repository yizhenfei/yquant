#include "strategyd/strategy_context.h"

#include "gtest/gtest.h"
#include "strategyd/market_data.h"

namespace yquant::strategyd {
namespace {

using base::ErrorCode;

class ContextTestClock final : public MonotonicClock {
 public:
  std::uint64_t Now() const noexcept override { return now; }
  std::uint64_t now = 1000;
};

// Two dictionary entries allow testing a known but unsubscribed instrument.
MarketDataConfig ContextConfig() {
  MarketDataConfig config;
  config.source_id = {1};
  config.interval_ns = 1000000;
  config.instrument_count = 2;
  const char* symbols[] = {"600000", "600001"};
  for (std::size_t index = 0; index < config.instrument_count; ++index) {
    auto& item = config.instruments[index];
    EXPECT_EQ(
        model::MakeInstrumentId("SSE", symbols[index], item.instrument.id),
        ErrorCode::kOk);
    item.instrument.venue = model::Venue::kSse;
    for (std::size_t digit = 0; digit < 6; ++digit) {
      item.instrument.symbol[digit] = symbols[index][digit];
    }
    item.instrument.price_decimals = 2;
    item.bid_price = 1000;
    item.bid_qty = 100;
    item.ask_price = 1001;
    item.ask_qty = 200;
    item.price_step = 1;
  }
  return config;
}

model::QuoteTick ContextQuote(model::InstrumentId id, std::uint64_t sequence) {
  model::QuoteTick quote;
  quote.source_id = {1};
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
  StrategyContextImpl context(clock, market_data);
  const auto definition = ContextConfig().instruments[0].instrument;
  auto unchanged_definition = definition;
  EXPECT_EQ(context.market_data().FindInstrument("SSE", "600000",
                                                 unchanged_definition),
            ErrorCode::kInvalidArgument);
  EXPECT_EQ(unchanged_definition, definition);
  auto untouched = ContextQuote(definition.id, 99);
  const auto original = untouched;
  EXPECT_FALSE(
      context.market_data().TryGetLatestQuote({1}, definition.id, untouched));
  EXPECT_EQ(untouched, original);
  EXPECT_EQ(context.market_data().SubscribeQuotes({1}, definition.id),
            ErrorCode::kInvalidState);
  market_data.BeginCallback(CallbackPhase::kStart);
  EXPECT_EQ(context.market_data().SubscribeQuotes({1}, definition.id),
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
  StrategyContextImpl context(clock, market_data);
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
  market_data.BeginCallback(CallbackPhase::kStart);
  EXPECT_EQ(context.market_data().SubscribeQuotes({2}, definition.id),
            ErrorCode::kInvalidArgument);
  EXPECT_EQ(context.market_data().SubscribeQuotes({1}, {}),
            ErrorCode::kInvalidArgument);
  EXPECT_TRUE(market_data.subscriptions().empty());
  ASSERT_EQ(context.market_data().SubscribeQuotes({1}, definition.id),
            ErrorCode::kOk);
  ASSERT_EQ(context.market_data().SubscribeQuotes({1}, definition.id),
            ErrorCode::kOk);
  ASSERT_EQ(market_data.subscriptions().size(), 1U);
  EXPECT_EQ(market_data.subscriptions()[0], definition.id);
  market_data.EndCallback();
  for (const auto phase :
       {CallbackPhase::kNone, CallbackPhase::kTick, CallbackPhase::kStop}) {
    market_data.BeginCallback(phase);
    EXPECT_EQ(context.market_data().SubscribeQuotes(
                  {1}, config.instruments[1].instrument.id),
              ErrorCode::kInvalidState);
    EXPECT_EQ(market_data.subscriptions().size(), 1U);
    market_data.EndCallback();
  }
}

TEST(StrategyContextTest, KeepsAcceptedSnapshotAndRequiresTheExactSourcePair) {
  ContextTestClock clock;
  MarketDataImpl market_data(clock);
  StrategyContextImpl context(clock, market_data);
  const strategy::StrategyContext& read_only_context = context;
  const auto config = ContextConfig();
  ASSERT_TRUE(market_data.Configure(config).ok());
  const auto id = config.instruments[0].instrument.id;
  market_data.BeginCallback(CallbackPhase::kStart);
  ASSERT_EQ(context.market_data().SubscribeQuotes({1}, id), ErrorCode::kOk);
  market_data.EndCallback();
  auto snapshot = ContextQuote(id, 99);
  const auto sentinel = snapshot;
  EXPECT_FALSE(
      read_only_context.market_data().TryGetLatestQuote({1}, id, snapshot));
  EXPECT_EQ(snapshot, sentinel);
  const auto first = ContextQuote(id, 2);
  bool accepted = false;
  ASSERT_EQ(market_data.ApplyQuote(first, accepted), ErrorCode::kOk);
  ASSERT_TRUE(accepted);
  ASSERT_TRUE(
      read_only_context.market_data().TryGetLatestQuote({1}, id, snapshot));
  EXPECT_EQ(snapshot, first);
  EXPECT_EQ(snapshot.receive_time_ns, 314U);
  EXPECT_EQ(context.Now(), 1000U);
  auto old = ContextQuote(id, 1);
  old.bid_price = 500;
  accepted = true;
  EXPECT_EQ(market_data.ApplyQuote(old, accepted), ErrorCode::kOk);
  EXPECT_FALSE(accepted);
  EXPECT_TRUE(
      read_only_context.market_data().TryGetLatestQuote({1}, id, snapshot));
  EXPECT_EQ(snapshot, first);
  auto invalid = ContextQuote(id, 3);
  invalid.source_id = {2};
  accepted = true;
  EXPECT_EQ(market_data.ApplyQuote(invalid, accepted), ErrorCode::kCorrupt);
  EXPECT_TRUE(accepted);
  EXPECT_FALSE(
      read_only_context.market_data().TryGetLatestQuote({2}, id, snapshot));
  EXPECT_EQ(snapshot, first);
  EXPECT_FALSE(read_only_context.market_data().TryGetLatestQuote(
      {1}, config.instruments[1].instrument.id, snapshot));
  EXPECT_EQ(snapshot, first);
  EXPECT_TRUE(
      read_only_context.market_data().TryGetLatestQuote({1}, id, snapshot));
  EXPECT_EQ(snapshot, first);
}

}  // namespace
}  // namespace yquant::strategyd

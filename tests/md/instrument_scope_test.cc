#include "yquant/md/instrument_scope.h"

#include <cstddef>
#include <cstdint>

#include "gtest/gtest.h"
#include "yquant/model/identifiers.h"

namespace yquant::md {
namespace {

// Builds valid identities without requiring a catalog or receive endpoint.
md::MarketDataConfig MakeConfig(std::size_t count = 3) {
  md::MarketDataConfig config;
  config.source_id = {17};
  config.instrument_count = count;
  for (std::size_t index = 0; index < count; ++index) {
    auto& definition = config.instruments[index].instrument;
    definition.venue = model::Venue::kSse;
    auto numeric_symbol = 600000 + index;
    for (std::size_t digit = model::kSymbolLength; digit != 0; --digit) {
      definition.symbol[digit - 1] = '0' + numeric_symbol % 10;
      numeric_symbol /= 10;
    }
    EXPECT_EQ(
        model::MakeInstrumentId("SSE", definition.symbol_view(), definition.id),
        base::ErrorCode::kOk);
    definition.price_decimals = index % 7;
  }
  return config;
}

TEST(InstrumentScopeTest, EmptyScopePreservesFailedLookupOutput) {
  InstrumentScope scope;
  EXPECT_EQ(scope.size(), 0U);
  EXPECT_EQ(scope.source_id().value, 0U);
  const auto config = MakeConfig();
  auto output = config.instruments[0].instrument;
  const auto unchanged = output;
  EXPECT_FALSE(scope.HasInstrument(output.id));
  EXPECT_FALSE(scope.FindIndex(output.id).has_value());
  EXPECT_FALSE(scope.TryGetInstrument(output.id, output));
  EXPECT_EQ(output, unchanged);
  EXPECT_FALSE(scope.HasInstrument({}));
}

TEST(InstrumentScopeTest, OwnsDefinitionsInConfigurationOrder) {
  auto config = MakeConfig();
  // Scope validation does not require receive endpoint configuration.
  config.adapter = static_cast<md::MarketDataAdapter>(99);
  const auto expected = config;
  InstrumentScope scope;
  ASSERT_TRUE(scope.Configure(config).ok());
  EXPECT_EQ(scope.size(), 3U);
  EXPECT_EQ(scope.source_id().value, 17U);
  config = {};

  for (std::size_t index = 0; index < 3; ++index) {
    const auto& definition = expected.instruments[index].instrument;
    EXPECT_EQ(scope[index], definition);
    EXPECT_TRUE(scope.HasInstrument(definition.id));
    ASSERT_EQ(scope.FindIndex(definition.id), index);
    model::InstrumentDefinition output;
    EXPECT_TRUE(scope.TryGetInstrument(definition.id, output));
    EXPECT_EQ(output, definition);
  }
  auto output = expected.instruments[0].instrument;
  const auto unchanged = output;
  EXPECT_FALSE(scope.TryGetInstrument({1}, output));
  EXPECT_EQ(output, unchanged);
}

TEST(InstrumentScopeTest,
     PreservesUnsortedOrderAndDistinguishesVenuesForSameSymbol) {
  struct Entry {
    model::Venue venue;
    const char* venue_name;
    const char* symbol;
    std::uint8_t price_decimals;
  };
  constexpr Entry kEntries[] = {
      {model::Venue::kSzse, "SZSE", "000001", 6},
      {model::Venue::kSse, "SSE", "600002", 2},
      {model::Venue::kBse, "BSE", "000001", 0},
      {model::Venue::kSse, "SSE", "000001", 4},
  };
  md::MarketDataConfig config;
  config.source_id = {17};
  config.instrument_count = 4;
  for (std::size_t index = 0; index < 4; ++index) {
    const auto& entry = kEntries[index];
    auto& definition = config.instruments[index].instrument;
    definition.venue = entry.venue;
    for (std::size_t digit = 0; digit < model::kSymbolLength; ++digit) {
      definition.symbol[digit] = entry.symbol[digit];
    }
    ASSERT_EQ(
        model::MakeInstrumentId(entry.venue_name, entry.symbol, definition.id),
        base::ErrorCode::kOk);
    definition.price_decimals = entry.price_decimals;
  }
  InstrumentScope scope;
  ASSERT_TRUE(scope.Configure(config).ok());
  ASSERT_EQ(scope.size(), 4U);
  for (std::size_t index = 0; index < 4; ++index) {
    SCOPED_TRACE(index);
    const auto& entry = kEntries[index];
    const auto id = config.instruments[index].instrument.id;
    EXPECT_TRUE(scope.HasInstrument(id));
    ASSERT_EQ(scope.FindIndex(id), index);
    EXPECT_EQ(scope[index].id, id);
    EXPECT_EQ(scope[index].venue, entry.venue);
    EXPECT_EQ(scope[index].symbol_view(), entry.symbol);
    EXPECT_EQ(scope[index].price_decimals, entry.price_decimals);
    model::InstrumentDefinition output;
    ASSERT_TRUE(scope.TryGetInstrument(id, output));
    EXPECT_EQ(output, scope[index]);
  }
}

TEST(InstrumentScopeTest, ResolvesEveryInstrumentAtCapacity) {
  const auto config = MakeConfig(64);
  InstrumentScope scope;
  ASSERT_TRUE(scope.Configure(config).ok());
  ASSERT_EQ(scope.size(), 64U);
  for (std::size_t index = 0; index < 64; ++index) {
    const auto& definition = config.instruments[index].instrument;
    ASSERT_EQ(scope.FindIndex(definition.id), index);
    model::InstrumentDefinition output;
    ASSERT_TRUE(scope.TryGetInstrument(definition.id, output));
    EXPECT_EQ(output, definition);
  }
  model::InstrumentId missing;
  ASSERT_EQ(model::MakeInstrumentId("SSE", "699999", missing),
            base::ErrorCode::kOk);
  EXPECT_FALSE(scope.HasInstrument(missing));
  EXPECT_FALSE(scope.FindIndex(missing).has_value());
}

TEST(InstrumentScopeTest, RejectsInvalidDefinitionsBeforeChangingState) {
  const auto valid = MakeConfig();
  for (std::size_t invalid_case = 0; invalid_case < 7; ++invalid_case) {
    SCOPED_TRACE(invalid_case);
    auto config = valid;
    switch (invalid_case) {
      case 0:
        config.source_id = {};
        break;
      case 1:
        config.instrument_count = 0;
        break;
      case 2:
        config.instrument_count = 65;
        break;
      case 3:
        config.instruments[0].instrument.id = {1};
        break;
      case 4:
        config.instruments[0].instrument.venue = model::Venue::kUnknown;
        break;
      case 5:
        config.instruments[0].instrument.price_decimals = 7;
        break;
      case 6:
        config.instruments[1] = config.instruments[0];
        break;
    }
    InstrumentScope scope;
    EXPECT_EQ(scope.Configure(config).code, base::ErrorCode::kInvalidArgument);
    EXPECT_EQ(scope.size(), 0U);
    EXPECT_EQ(scope.source_id().value, 0U);
    EXPECT_FALSE(scope.HasInstrument(valid.instruments[0].instrument.id));
    ASSERT_TRUE(scope.Configure(valid).ok());
    EXPECT_EQ(scope.size(), 3U);
  }
}

TEST(InstrumentScopeTest, RequiresResetBeforeReconfiguration) {
  const auto initial = MakeConfig();
  InstrumentScope scope;
  ASSERT_TRUE(scope.Configure(initial).ok());
  auto replacement = MakeConfig(1);
  replacement.source_id = {23};
  replacement.instruments[0] = initial.instruments[2];
  EXPECT_EQ(scope.Configure(replacement).code, base::ErrorCode::kInvalidState);
  EXPECT_EQ(scope.source_id().value, 17U);
  EXPECT_EQ(scope.size(), 3U);
  EXPECT_EQ(scope[0], initial.instruments[0].instrument);

  scope.Reset();
  EXPECT_EQ(scope.source_id().value, 0U);
  EXPECT_EQ(scope.size(), 0U);
  EXPECT_FALSE(scope.HasInstrument(initial.instruments[0].instrument.id));
  ASSERT_TRUE(scope.Configure(replacement).ok());
  EXPECT_EQ(scope.source_id().value, 23U);
  EXPECT_EQ(scope.size(), 1U);
  EXPECT_FALSE(scope.HasInstrument(initial.instruments[0].instrument.id));
  EXPECT_EQ(scope.FindIndex(initial.instruments[2].instrument.id), 0U);
}

}  // namespace
}  // namespace yquant::md

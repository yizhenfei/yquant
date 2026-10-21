#include "yquant/md/instrument_catalog.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <type_traits>

#include "gtest/gtest.h"
#include "strategyd/test_config_file.h"
#include "yquant/model/identifiers.h"
#include "yquant/model/instrument.h"

namespace yquant::md {
namespace {

using base::ErrorCode;
using ::yquant::strategyd::testing::ConfigFile;

static_assert(!std::is_move_constructible_v<InstrumentCatalog>);
static_assert(!std::is_move_assignable_v<InstrumentCatalog>);

constexpr std::string_view kSingleInstrument =
    R"({"instruments":[{"venue":"SSE","symbol":"600000","price_decimals":2}]})";

// Generates distinct identities for exercising catalog capacity independently
// of a strategy process's smaller active instrument limit.
std::string CatalogWithEntries(std::size_t count) {
  std::string json = R"({"instruments":[)";
  for (std::size_t index = 0; index < count; ++index) {
    if (index != 0) json += ',';
    char symbol[model::kSymbolLength + 1];
    std::snprintf(symbol, sizeof(symbol), "%0*zu",
                  static_cast<int>(model::kSymbolLength), index);
    json += R"({"venue":"SSE","symbol":")";
    json += symbol;
    json += R"(","price_decimals":2})";
  }
  json += "]}";
  return json;
}

TEST(InstrumentCatalogTest, LoadsUnsortedVenuesAndPreservesLeadingZeros) {
  InstrumentCatalog catalog;
  {
    ConfigFile file(
        R"({"instruments":[)"
        R"({"symbol":"000001","price_decimals":6,"venue":"BSE"},)"
        R"({"venue":"SSE","symbol":"000001","price_decimals":0},)"
        R"({"venue":"SZSE","symbol":"000001","price_decimals":2}]})");
    ASSERT_TRUE(file.valid());
    ASSERT_TRUE(catalog.Load(file.path()).ok()) << catalog.error().message;
  }
  ASSERT_EQ(catalog.size(), 3U);
  const struct {
    std::string_view venue;
    model::Venue expected_venue;
    std::uint8_t expected_decimals;
  } cases[] = {
      {"SSE", model::Venue::kSse, 0},
      {"SZSE", model::Venue::kSzse, 2},
      {"BSE", model::Venue::kBse, 6},
  };
  for (const auto& item : cases) {
    SCOPED_TRACE(item.venue);
    model::InstrumentId id;
    ASSERT_EQ(model::MakeInstrumentId(item.venue, "000001", id),
              ErrorCode::kOk);
    model::InstrumentDefinition definition;
    ASSERT_TRUE(catalog.Find(id, definition));
    EXPECT_EQ(definition.id, id);
    EXPECT_EQ(definition.venue, item.expected_venue);
    EXPECT_EQ(definition.symbol_view(), "000001");
    EXPECT_EQ(definition.price_decimals, item.expected_decimals);
  }

  model::InstrumentDefinition unchanged;
  unchanged.id = {123};
  unchanged.price_decimals = 5;
  auto out = unchanged;
  EXPECT_FALSE(catalog.Find({}, out));
  EXPECT_EQ(out, unchanged);
  model::InstrumentId unknown;
  ASSERT_EQ(model::MakeInstrumentId("SSE", "999999", unknown), ErrorCode::kOk);
  EXPECT_FALSE(catalog.Find(unknown, out));
  EXPECT_EQ(out, unchanged);
}

TEST(InstrumentCatalogTest, SupportsMoreThan64EntriesAndEnforces16384Limit) {
  for (const std::size_t count : {65U, 16384U, 16385U}) {
    SCOPED_TRACE(count);
    ConfigFile file(CatalogWithEntries(count));
    ASSERT_TRUE(file.valid());
    InstrumentCatalog catalog;
    const auto result = catalog.Load(file.path());
    if (count <= 16384) {
      ASSERT_TRUE(result.ok()) << catalog.error().message;
      EXPECT_EQ(catalog.size(), count);
      model::InstrumentDefinition last;
      EXPECT_TRUE(catalog.Find({(std::uint64_t{1} << 32) | (count - 1)}, last));
    } else {
      EXPECT_EQ(result.code, ErrorCode::kInvalidArgument);
      EXPECT_EQ(catalog.size(), 0U);
      EXPECT_STREQ(catalog.error().field_path, "instruments");
    }
  }
}

TEST(InstrumentCatalogTest, RejectsInvalidSchemaTypesAndPrecision) {
  const std::string_view cases[] = {
      "[]",
      "{}",
      R"({"instruments":{}})",
      R"({"instruments":[]})",
      R"({"instruments":[1]})",
      R"({"instruments":[],"extra":0})",
      R"({"instruments":[],"\u0069nstruments":[]})",
      R"({"instruments":[{"symbol":"600000","price_decimals":2}]})",
      R"({"instruments":[{"venue":"SSE","price_decimals":2}]})",
      R"({"instruments":[{"venue":"SSE","symbol":"600000"}]})",
      R"({"instruments":[{"venue":"XXX","symbol":"600000","price_decimals":2}]})",
      R"({"instruments":[{"venue":1,"symbol":"600000","price_decimals":2}]})",
      R"({"instruments":[{"venue":"SSE","symbol":600000,"price_decimals":2}]})",
      R"({"instruments":[{"venue":"SSE","symbol":"60000","price_decimals":2}]})",
      R"({"instruments":[{"venue":"SSE","symbol":"60000A","price_decimals":2}]})",
      R"({"instruments":[{"venue":"SSE","symbol":"600\u000000","price_decimals":2}]})",
      R"({"instruments":[{"venue":"SSE","symbol":"600000","price_decimals":-1}]})",
      R"({"instruments":[{"venue":"SSE","symbol":"600000","price_decimals":7}]})",
      R"({"instruments":[{"venue":"SSE","symbol":"600000","price_decimals":1.5}]})",
      R"({"instruments":[{"venue":"SSE","symbol":"600000","price_decimals":"2"}]})",
      R"({"instruments":[{"venue":"SSE","symbol":"600000","price_decimals":18446744073709551616}]})",
      R"({"instruments":[{"venue":"SSE","symbol":"600000","price_decimals":2,"extra":0}]})",
      R"({"instruments":[{"venue":"SSE","\u0076enue":"SSE","symbol":"600000","price_decimals":2}]})",
      R"({"instruments":[{"venue":"SSE","symbol":"600000","price_decimals":2},{"venue":"SSE","symbol":"600000","price_decimals":4}]})",
  };
  InstrumentCatalog catalog;
  for (const auto json : cases) {
    SCOPED_TRACE(json);
    ConfigFile file(json);
    ASSERT_TRUE(file.valid());
    EXPECT_EQ(catalog.Load(file.path()).code, ErrorCode::kInvalidArgument);
    EXPECT_EQ(catalog.size(), 0U);
    EXPECT_NE(catalog.error().message[0], '\0');
  }
}

TEST(InstrumentCatalogTest, FailedReloadHidesAllDefinitionsAndCanLoadAgain) {
  ConfigFile valid(kSingleInstrument);
  ConfigFile invalid(
      R"({"instruments":[{"venue":"SSE","symbol":"000001","price_decimals":2},1]})");
  ASSERT_TRUE(valid.valid());
  ASSERT_TRUE(invalid.valid());
  InstrumentCatalog catalog;
  ASSERT_TRUE(catalog.Load(valid.path()).ok());
  model::InstrumentId id;
  ASSERT_EQ(model::MakeInstrumentId("SSE", "600000", id), ErrorCode::kOk);
  model::InstrumentDefinition out;
  ASSERT_TRUE(catalog.Find(id, out));
  const auto unchanged = out;

  EXPECT_FALSE(catalog.Load(invalid.path()).ok());
  EXPECT_EQ(catalog.size(), 0U);
  EXPECT_FALSE(catalog.Find(id, out));
  EXPECT_EQ(out, unchanged);
  model::InstrumentId partially_parsed;
  ASSERT_EQ(model::MakeInstrumentId("SSE", "000001", partially_parsed),
            ErrorCode::kOk);
  EXPECT_FALSE(catalog.Find(partially_parsed, out));
  EXPECT_EQ(out, unchanged);

  ASSERT_TRUE(catalog.Load(valid.path()).ok());
  EXPECT_TRUE(catalog.Find(id, out));
  EXPECT_EQ(catalog.error().message[0], '\0');
  EXPECT_FALSE(catalog.Load(nullptr).ok());
  EXPECT_EQ(catalog.size(), 0U);
  EXPECT_FALSE(catalog.Find(id, out));
  EXPECT_EQ(out, unchanged);
}

}  // namespace
}  // namespace yquant::md

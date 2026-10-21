#include "strategyd/instrument_catalog.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

#include "gtest/gtest.h"
#include "test_config_file.h"
#include "yquant/model/identifiers.h"
#include "yquant/model/instrument.h"

namespace yquant::strategyd {
namespace {

using base::ErrorCode;
using testing::ConfigFile;

static_assert(!std::is_move_constructible_v<InstrumentCatalogLoader>);
static_assert(!std::is_move_assignable_v<InstrumentCatalogLoader>);

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

struct InvalidField {
  const char* name;
  std::string_view from;
  std::string_view to;
};
std::string Replace(std::string json, std::string_view from,
                    std::string_view to) {
  const auto position = json.find(from);
  if (position != std::string::npos) json.replace(position, from.size(), to);
  return json;
}
std::string FutureInstrument(bool mock = false) {
  std::string instrument =
      R"({"venue":"SHFE","symbol":"rb2501","price_decimals":0,)"
      R"("instrument_type":"futures","price_tick":2,"quantity_step":5,)"
      R"("contract_multiplier":10,"margin_rate_ppm":100000,)"
      R"("open_fee_per_lot":1000,"close_today_fee_per_lot":2000,)"
      R"("close_yesterday_fee_per_lot":3000)";
  if (mock) {
    instrument +=
        R"(,"bid_price":3500,"bid_qty":10,"ask_price":3502,"ask_qty":20,"price_step":2)";
  }
  return instrument + '}';
}

TEST(InstrumentCatalogTest, LoadsOwnedFuturesSpecificationsAndExactFees) {
  ConfigFile file(Replace(
      R"({"instruments":[)" + FutureInstrument(false) + "]}",
      R"("open_fee_per_lot":1000)", R"("open_fee_per_lot":9007199254740993)"));
  ASSERT_TRUE(file.valid());
  InstrumentCatalogLoader loader;
  ASSERT_TRUE(loader.Load(file.path()).ok())
      << loader.error().field_path << ": " << loader.error().message;
  model::InstrumentId id;
  ASSERT_EQ(model::MakeInstrumentId("SHFE", "rb2501", id), ErrorCode::kOk);
  model::InstrumentDefinition definition;
  ASSERT_TRUE(loader.Find(id, definition));
  EXPECT_EQ(definition.venue, model::Venue::kShfe);
  EXPECT_EQ(definition.symbol_view(), "rb2501");
  EXPECT_EQ(definition.type, model::InstrumentType::kFutures);
  EXPECT_EQ(definition.price_tick, 2);
  EXPECT_EQ(definition.quantity_step, 5);
  EXPECT_EQ(definition.contract_multiplier, 10);
  EXPECT_EQ(definition.margin_rate_ppm, 100000);
  EXPECT_EQ(definition.open_fee_per_lot, 9007199254740993LL);
  EXPECT_EQ(definition.close_today_fee_per_lot, 2000);
  EXPECT_EQ(definition.close_yesterday_fee_per_lot, 3000);
  EXPECT_TRUE(model::IsValidInstrumentId(definition.id));
  strategy::ConfigError error;
  EXPECT_TRUE(
      ValidateInstrumentDefinition(definition, "instrument", error).ok());
}

class InvalidFuturesCatalogTest
    : public ::testing::TestWithParam<InvalidField> {};

TEST_P(InvalidFuturesCatalogTest, RejectsInvalidContractSpecifications) {
  const auto& item = GetParam();
  ConfigFile file(
      Replace(R"({"instruments":[)" + FutureInstrument(false) + "]}", item.from,
              item.to));
  ASSERT_TRUE(file.valid());
  InstrumentCatalogLoader loader;
  EXPECT_FALSE(loader.Load(file.path()).ok());
  EXPECT_NE(loader.error().message[0], '\0');
  EXPECT_EQ(loader.size(), 0U);
}

INSTANTIATE_TEST_SUITE_P(
    FuturesSchema, InvalidFuturesCatalogTest,
    ::testing::Values(
        InvalidField{"MissingType", R"("instrument_type":"futures",)", ""},
        InvalidField{"UnknownType", R"("futures")", R"("option")"},
        InvalidField{"TypeString", R"("instrument_type":"futures")",
                     R"("instrument_type":1)"},
        InvalidField{"StockVenue", R"("SHFE")", R"("SSE")"},
        InvalidField{"SymbolCase", R"("rb2501")", R"("RB2501")"},
        InvalidField{"Month", R"("rb2501")", R"("rb2513")"},
        InvalidField{"MissingTick", R"("price_tick":2,)", ""},
        InvalidField{"MissingStep", R"("quantity_step":5,)", ""},
        InvalidField{"MissingMultiplier", R"("contract_multiplier":10,)", ""},
        InvalidField{"MissingMargin", R"("margin_rate_ppm":100000,)", ""},
        InvalidField{"MissingOpenFee", R"("open_fee_per_lot":1000,)", ""},
        InvalidField{"MissingTodayFee", R"("close_today_fee_per_lot":2000,)",
                     ""},
        InvalidField{"MissingYesterdayFee",
                     R"(,"close_yesterday_fee_per_lot":3000)", ""},
        InvalidField{"ZeroTick", R"("price_tick":2)", R"("price_tick":0)"},
        InvalidField{"NegativeTick", R"("price_tick":2)", R"("price_tick":-1)"},
        InvalidField{"TickString", R"("price_tick":2)", R"("price_tick":"2")"},
        InvalidField{"ZeroStep", R"("quantity_step":5)",
                     R"("quantity_step":0)"},
        InvalidField{"FractionalStep", R"("quantity_step":5)",
                     R"("quantity_step":5.0)"},
        InvalidField{"ZeroMultiplier", R"("contract_multiplier":10)",
                     R"("contract_multiplier":0)"},
        InvalidField{"ZeroMargin", R"("margin_rate_ppm":100000)",
                     R"("margin_rate_ppm":0)"},
        InvalidField{"LargeMargin", R"("margin_rate_ppm":100000)",
                     R"("margin_rate_ppm":1000001)"},
        InvalidField{"NegativeOpenFee", R"("open_fee_per_lot":1000)",
                     R"("open_fee_per_lot":-1)"},
        InvalidField{"NegativeTodayFee", R"("close_today_fee_per_lot":2000)",
                     R"("close_today_fee_per_lot":-1)"},
        InvalidField{"NegativeYesterdayFee",
                     R"("close_yesterday_fee_per_lot":3000)",
                     R"("close_yesterday_fee_per_lot":-1)"},
        InvalidField{"FeeOverflow", R"("open_fee_per_lot":1000)",
                     R"("open_fee_per_lot":9223372036854775808)"},
        InvalidField{"DuplicateDecodedField", R"("price_tick":2)",
                     R"("price_tick":2,"\u0070rice_tick":2)"}),
    [](const ::testing::TestParamInfo<InvalidField>& info) {
      return info.param.name;
    });

TEST(InstrumentCatalogTest, LoadsUnsortedVenuesAndPreservesLeadingZeros) {
  InstrumentCatalogLoader loader;
  {
    ConfigFile file(
        R"({"instruments":[)"
        R"({"symbol":"000001","price_decimals":6,"venue":"BSE"},)"
        R"({"venue":"SSE","symbol":"000001","price_decimals":0},)"
        R"({"venue":"SZSE","symbol":"000001","price_decimals":2}]})");
    ASSERT_TRUE(file.valid());
    ASSERT_TRUE(loader.Load(file.path()).ok()) << loader.error().message;
  }
  ASSERT_EQ(loader.size(), 3U);
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
    ASSERT_TRUE(loader.Find(id, definition));
    EXPECT_EQ(definition.id, id);
    EXPECT_EQ(definition.venue, item.expected_venue);
    EXPECT_EQ(definition.symbol_view(), "000001");
    EXPECT_EQ(definition.price_decimals, item.expected_decimals);
  }

  model::InstrumentDefinition unchanged;
  unchanged.id = {123};
  unchanged.price_decimals = 5;
  auto out = unchanged;
  EXPECT_FALSE(loader.Find({}, out));
  EXPECT_EQ(out, unchanged);
  model::InstrumentId unknown;
  ASSERT_EQ(model::MakeInstrumentId("SSE", "999999", unknown), ErrorCode::kOk);
  EXPECT_FALSE(loader.Find(unknown, out));
  EXPECT_EQ(out, unchanged);
}

TEST(InstrumentCatalogTest, SupportsMoreThan64EntriesAndEnforces16384Limit) {
  for (const std::size_t count : {65U, 16384U, 16385U}) {
    SCOPED_TRACE(count);
    ConfigFile file(CatalogWithEntries(count));
    ASSERT_TRUE(file.valid());
    InstrumentCatalogLoader loader;
    const auto result = loader.Load(file.path());
    if (count <= 16384) {
      ASSERT_TRUE(result.ok()) << loader.error().message;
      EXPECT_EQ(loader.size(), count);
      model::InstrumentDefinition last;
      EXPECT_TRUE(loader.Find({(std::uint64_t{1} << 32) | (count - 1)}, last));
    } else {
      EXPECT_EQ(result.code, ErrorCode::kInvalidArgument);
      EXPECT_EQ(loader.size(), 0U);
      EXPECT_STREQ(loader.error().field_path, "instruments");
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
  InstrumentCatalogLoader loader;
  for (const auto json : cases) {
    SCOPED_TRACE(json);
    ConfigFile file(json);
    ASSERT_TRUE(file.valid());
    EXPECT_EQ(loader.Load(file.path()).code, ErrorCode::kInvalidArgument);
    EXPECT_EQ(loader.size(), 0U);
    EXPECT_NE(loader.error().message[0], '\0');
  }
}

TEST(InstrumentCatalogTest, FailedReloadHidesAllDefinitionsAndCanLoadAgain) {
  ConfigFile valid(kSingleInstrument);
  ConfigFile invalid(
      R"({"instruments":[{"venue":"SSE","symbol":"000001","price_decimals":2},1]})");
  ASSERT_TRUE(valid.valid());
  ASSERT_TRUE(invalid.valid());
  InstrumentCatalogLoader loader;
  ASSERT_TRUE(loader.Load(valid.path()).ok());
  model::InstrumentId id;
  ASSERT_EQ(model::MakeInstrumentId("SSE", "600000", id), ErrorCode::kOk);
  model::InstrumentDefinition out;
  ASSERT_TRUE(loader.Find(id, out));
  const auto unchanged = out;

  EXPECT_FALSE(loader.Load(invalid.path()).ok());
  EXPECT_EQ(loader.size(), 0U);
  EXPECT_FALSE(loader.Find(id, out));
  EXPECT_EQ(out, unchanged);
  model::InstrumentId partially_parsed;
  ASSERT_EQ(model::MakeInstrumentId("SSE", "000001", partially_parsed),
            ErrorCode::kOk);
  EXPECT_FALSE(loader.Find(partially_parsed, out));
  EXPECT_EQ(out, unchanged);

  ASSERT_TRUE(loader.Load(valid.path()).ok());
  EXPECT_TRUE(loader.Find(id, out));
  EXPECT_EQ(loader.error().message[0], '\0');
  EXPECT_FALSE(loader.Load(nullptr).ok());
  EXPECT_EQ(loader.size(), 0U);
  EXPECT_FALSE(loader.Find(id, out));
  EXPECT_EQ(out, unchanged);
}

TEST(InstrumentCatalogPathTest, ResolvesReferencesBesideTheHostConfiguration) {
  std::array<char, kMaxInstrumentCatalogPathLength + 1> path = {};
  strategy::ConfigError error;
  error.Set("stale", "old error");
  ASSERT_TRUE(ResolveInstrumentCatalogPath("/tmp/yquant/host.json",
                                           "catalog.json", path, error)
                  .ok());
  EXPECT_STREQ(path.data(), "/tmp/yquant/catalog.json");
  EXPECT_EQ(error.message[0], '\0');

  ASSERT_TRUE(ResolveInstrumentCatalogPath("/tmp/yquant/host.json",
                                           "/tmp/common/catalog.json", path,
                                           error)
                  .ok());
  EXPECT_STREQ(path.data(), "/tmp/common/catalog.json");

  std::error_code filesystem_error;
  const auto current = std::filesystem::current_path(filesystem_error);
  ASSERT_FALSE(filesystem_error);
  ASSERT_TRUE(ResolveInstrumentCatalogPath("configs/host.json", "catalog.json",
                                           path, error)
                  .ok());
  EXPECT_EQ(std::string_view(path.data()),
            (current / "configs/catalog.json").native());
}

TEST(InstrumentCatalogPathTest, PreservesOutputForInvalidOrOverlongPaths) {
  std::array<char, kMaxInstrumentCatalogPathLength + 1> path;
  path.fill('x');
  const auto unchanged = path;
  strategy::ConfigError error;
  constexpr char kNulReference[] = "catalog\0.json";
  const std::string overlong(4096, 'x');
  const std::string long_directory = "/" + std::string(4090, 'x') + "/c";
  const struct {
    const char* config;
    std::string_view reference;
  } cases[] = {
      {nullptr, "catalog.json"},
      {"", "catalog.json"},
      {"/tmp/host.json", ""},
      {"/tmp/host.json", {kNulReference, sizeof(kNulReference) - 1}},
      {"/tmp/host.json", overlong},
      {overlong.c_str(), "catalog.json"},
      {long_directory.c_str(), "catalog.json"},
  };
  for (const auto& item : cases) {
    EXPECT_EQ(
        ResolveInstrumentCatalogPath(item.config, item.reference, path, error)
            .code,
        ErrorCode::kInvalidArgument);
    EXPECT_EQ(path, unchanged);
    EXPECT_STREQ(error.field_path, "instrument_catalog");
  }
  std::array<char, 4> short_path = {'k', 'e', 'e', 'p'};
  const auto short_unchanged = short_path;
  EXPECT_EQ(ResolveInstrumentCatalogPath("/tmp/host.json", "catalog.json",
                                         short_path, error)
                .code,
            ErrorCode::kInvalidArgument);
  EXPECT_EQ(short_path, short_unchanged);
}

TEST(InstrumentCatalogPathTest, AcceptsThe4095ByteBoundary) {
  std::string reference(4095, 'x');
  reference[0] = '/';
  std::array<char, kMaxInstrumentCatalogPathLength + 1> path = {};
  strategy::ConfigError error;
  ASSERT_TRUE(
      ResolveInstrumentCatalogPath("/tmp/host.json", reference, path, error)
          .ok());
  EXPECT_EQ(std::string_view(path.data()), reference);
  EXPECT_EQ(path.back(), '\0');
}

}  // namespace
}  // namespace yquant::strategyd

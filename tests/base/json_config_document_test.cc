#include "yquant/base/json_config_document.h"

#include <stddef.h>
#include <stdint.h>

#include <array>
#include <string>
#include <string_view>

#include "base/internal/json_config_parser.h"
#include "gtest/gtest.h"
#include "simdjson.h"
#include "test_config_file.h"

namespace yquant::base {
namespace {

using ::yquant::testing::ConfigFile;

TEST(JsonConfigDocumentTest, OwnsUnchangedPaddedSourceUntilReset) {
  JsonConfigDocument source;
  const std::string json =
      R"( { "number":18446744073709551616000000000000, "text":"\u0061" } )";
  {
    ConfigFile file(json);
    ASSERT_TRUE(file.valid());
    ASSERT_TRUE(source.Load(file.path()).ok()) << source.error().message;
  }
  EXPECT_EQ(source.source(), json);
  ASSERT_GE(source.capacity(), json.size() + simdjson::SIMDJSON_PADDING);
  for (size_t i = 0; i < simdjson::SIMDJSON_PADDING; ++i) {
    EXPECT_EQ(source.source().data()[json.size() + i], '\0');
  }
  source.Reset();
  EXPECT_TRUE(source.source().empty());
  EXPECT_EQ(source.capacity(), 0U);
}

TEST(JsonConfigDocumentTest, FailedReloadDiscardsSourceAndOwnsDiagnostic) {
  ConfigFile good(R"({"key":1})");
  ConfigFile bad(R"({"key":1,"\u006bey":2})");
  ASSERT_TRUE(good.valid());
  ASSERT_TRUE(bad.valid());
  JsonConfigDocument source;
  ASSERT_TRUE(source.Load(good.path()).ok());
  EXPECT_EQ(source.Load(bad.path()).code, ErrorCode::kInvalidArgument);
  EXPECT_TRUE(source.source().empty());
  EXPECT_EQ(source.capacity(), 0U);
  source.Reset();
  EXPECT_STREQ(source.error().field_path, "key");
  EXPECT_STREQ(source.error().message, "duplicate object key");
  ASSERT_TRUE(source.Load(good.path()).ok());
  EXPECT_STREQ(source.error().field_path, "");
  EXPECT_STREQ(source.error().message, "");
  EXPECT_FALSE(source.error().truncated);
}

TEST(JsonConfigDocumentTest, EmptyPathAndMissingFileReturnExplicitErrors) {
  JsonConfigDocument source;
  EXPECT_EQ(source.Load(nullptr).code, ErrorCode::kInvalidArgument);
  EXPECT_EQ(source.Load("").code, ErrorCode::kInvalidArgument);
  EXPECT_STREQ(source.error().field_path, "$");
  EXPECT_EQ(source.Load("/dev/null/yquant-config.json").code,
            ErrorCode::kIoError);
  EXPECT_STREQ(source.error().message, "cannot open configuration file");
  EXPECT_NE(source.error().message[0], '\0');
  EXPECT_TRUE(source.source().empty());
}

struct MalformedDocument {
  const char* name;
  const char* json;
};

class JsonConfigMalformedTest
    : public ::testing::TestWithParam<MalformedDocument> {};

TEST_P(JsonConfigMalformedTest, RejectsTheCompleteDocument) {
  ConfigFile file(GetParam().json);
  ASSERT_TRUE(file.valid());
  JsonConfigDocument source;
  EXPECT_EQ(source.Load(file.path()).code, ErrorCode::kInvalidArgument);
  EXPECT_NE(source.error().message[0], '\0');
  EXPECT_TRUE(source.source().empty());
}

INSTANTIATE_TEST_SUITE_P(
    FullValidation, JsonConfigMalformedTest,
    ::testing::Values(
        MalformedDocument{"Empty", ""}, MalformedDocument{"RootArray", "[]"},
        MalformedDocument{"RootNull", "null"},
        MalformedDocument{"RootString", R"("text")"},
        MalformedDocument{"Malformed", R"({"nested":{"value":)"},
        MalformedDocument{"TrailingDocument", "{} {}"},
        MalformedDocument{"TrailingGarbage", "{} garbage"},
        MalformedDocument{"Comment", R"({/*comment*/"a":1})"},
        MalformedDocument{"TrailingComma", R"({"a":1,})"},
        MalformedDocument{"NumberLeadingZero", R"({"nested":{"a":01}})"},
        MalformedDocument{"NumberMissingFraction", R"({"a":[1.]})"},
        MalformedDocument{"NumberMissingExponent", R"({"a":[1e+]})"},
        MalformedDocument{"NumberInvalidSuffix", R"({"a":[1x]})"},
        MalformedDocument{"InvalidBoolean", R"({"a":[tru]})"},
        MalformedDocument{"InvalidNull", R"({"a":[nul]})"},
        MalformedDocument{"UnpairedValueSurrogate", R"({"a":["\udfff"]})"},
        MalformedDocument{"UnpairedKeySurrogate", R"({"a":{"\ud800":1}})"}),
    [](const ::testing::TestParamInfo<MalformedDocument>& info) {
      return info.param.name;
    });

TEST(JsonConfigDocumentTest, AllowsUnrestrictedNumericRangesAndScalarKinds) {
  ConfigFile file(
      R"({"integer":18446744073709551616000000000000,)"
      R"("negative":-18446744073709551616000000000000,)"
      R"("exponent":1e9999,"array":[-0,0.1,1e-9999,true,false,null]})");
  ASSERT_TRUE(file.valid());
  JsonConfigDocument source;
  EXPECT_TRUE(source.Load(file.path()).ok()) << source.error().message;
}

TEST(JsonConfigDocumentTest, RejectsInvalidUtf8InKeysAndValues) {
  for (const char* text : {"{\"", "{\"a\":[\""}) {
    const std::string prefix(text);
    const std::string json =
        prefix + '\x80' + (prefix.size() == 2 ? "\":1}" : "\"]}");
    ConfigFile file(json);
    ASSERT_TRUE(file.valid());
    JsonConfigDocument source;
    EXPECT_EQ(source.Load(file.path()).code, ErrorCode::kInvalidArgument);
  }
}

TEST(JsonConfigDocumentTest, RejectsDecodedDuplicateKeysAndCopiesEscapedPath) {
  ConfigFile file(R"({"items":[{"a\u0000b":1,"a\u0000\u0062":2}]})");
  ASSERT_TRUE(file.valid());
  JsonConfigDocument source;
  EXPECT_EQ(source.Load(file.path()).code, ErrorCode::kInvalidArgument);
  EXPECT_STREQ(source.error().field_path, "items[0].a\\u0000b");
  EXPECT_STREQ(source.error().message, "duplicate object key");
}

TEST(JsonConfigDocumentTest, DistinguishesKeysInDifferentObjects) {
  ConfigFile file(R"({"one":{"a":1},"two":{"a":2},)"
                  R"("items":[{"":3},{"":4}]})");
  ASSERT_TRUE(file.valid());
  JsonConfigDocument source;
  EXPECT_TRUE(source.Load(file.path()).ok()) << source.error().message;
}

TEST(JsonConfigDocumentTest, PreservesDiagnosticPathTruncation) {
  const std::string key(300, 'k');
  ConfigFile file("{\"" + key + "\":{\"a\":1,\"a\":2}}");
  ASSERT_TRUE(file.valid());
  JsonConfigDocument source;
  EXPECT_EQ(source.Load(file.path()).code, ErrorCode::kInvalidArgument);
  EXPECT_TRUE(source.error().truncated);
  EXPECT_EQ(std::string_view(source.error().field_path).size(), 255U);
  EXPECT_STREQ(source.error().message, "duplicate object key");
}

TEST(JsonConfigDocumentTest, AcceptsOneMiBAndRejectsOneAdditionalByte) {
  std::string json = "{}";
  json.resize(1024 * 1024, ' ');
  ConfigFile exact(json);
  ASSERT_TRUE(exact.valid());
  JsonConfigDocument source;
  EXPECT_TRUE(source.Load(exact.path()).ok()) << source.error().message;
  json += ' ';
  ConfigFile excess(json);
  ASSERT_TRUE(excess.valid());
  EXPECT_EQ(source.Load(excess.path()).code, ErrorCode::kInvalidArgument);
  EXPECT_STREQ(source.error().message,
               "configuration file exceeds the 1 MiB limit");
  EXPECT_TRUE(source.source().empty());
}

TEST(JsonConfigDocumentTest, AcceptsDepth64AndRejectsDepth65) {
  for (const size_t depth : {64U, 65U}) {
    SCOPED_TRACE(depth);
    std::string json = R"({"nested":)";
    // The root object accounts for the first container level.
    json.append(depth - 1, '[');
    json += '0';
    json.append(depth - 1, ']');
    json += '}';
    ConfigFile file(json);
    ASSERT_TRUE(file.valid());
    JsonConfigDocument source;
    EXPECT_EQ(source.Load(file.path()).ok(), depth == 64U)
        << source.error().message;
  }
}

TEST(JsonConfigDocumentTest, ParserHelperStartsIndependentSchemaTraversal) {
  ConfigFile file(R"({"value":42})");
  ASSERT_TRUE(file.valid());
  JsonConfigDocument source;
  ASSERT_TRUE(source.Load(file.path()).ok());
  simdjson::ondemand::parser parser;
  simdjson::ondemand::document document;
  simdjson::ondemand::object object;
  ConfigError error;
  ASSERT_TRUE(
      internal::ParseConfigObject(source, parser, document, object, error).ok())
      << error.message;
  uint64_t value = 0;
  ASSERT_EQ(object["value"].get_uint64().get(value), simdjson::SUCCESS);
  EXPECT_EQ(value, 42U);
  EXPECT_EQ(source.source(), R"({"value":42})");
}

TEST(JsonConfigDocumentTest, TextHelperCopiesDecodedTextAndTerminator) {
  ConfigFile file(R"({"value":"\u0076alue"})");
  ASSERT_TRUE(file.valid());
  JsonConfigDocument source;
  ASSERT_TRUE(source.Load(file.path()).ok());
  simdjson::ondemand::parser parser;
  simdjson::ondemand::document document;
  simdjson::ondemand::object object;
  ConfigError error;
  ASSERT_TRUE(
      internal::ParseConfigObject(source, parser, document, object, error)
          .ok());
  simdjson::ondemand::value value;
  ASSERT_EQ(object["value"].get(value), simdjson::SUCCESS);
  std::array<char, 6> out;
  out.fill('?');
  ASSERT_TRUE(internal::ReadConfigText(value, out, "value", error).ok());
  EXPECT_STREQ(out.data(), "value");
  EXPECT_EQ(out.back(), '\0');
}

TEST(JsonConfigDocumentTest,
     TextHelperRejectsInvalidTextWithoutChangingOutput) {
  struct InvalidText {
    const char* token;
    const char* message;
  };
  constexpr std::string_view kInvalidTextMessage = "invalid configured name";
  const InvalidText cases[] = {
      {"42", simdjson::error_message(simdjson::INCORRECT_TYPE)},
      {"null", simdjson::error_message(simdjson::INCORRECT_TYPE)},
      {R"("")", kInvalidTextMessage.data()},
      {R"("a\u0000b")", kInvalidTextMessage.data()},
      {R"("abcd")", kInvalidTextMessage.data()},
      {R"("abcde")", kInvalidTextMessage.data()},
  };
  for (const auto& test : cases) {
    SCOPED_TRACE(test.token);
    ConfigFile file(std::string(R"({"value":)") + test.token + '}');
    ASSERT_TRUE(file.valid());
    JsonConfigDocument source;
    ASSERT_TRUE(source.Load(file.path()).ok());
    simdjson::ondemand::parser parser;
    simdjson::ondemand::document document;
    simdjson::ondemand::object object;
    ConfigError error;
    ASSERT_TRUE(
        internal::ParseConfigObject(source, parser, document, object, error)
            .ok());
    simdjson::ondemand::value value;
    ASSERT_EQ(object["value"].get(value), simdjson::SUCCESS);
    std::array<char, 4> out = {'o', 'l', 'd', '\0'};
    const auto before = out;
    EXPECT_EQ(
        internal::ReadConfigText(value, out, "name", error, kInvalidTextMessage)
            .code,
        ErrorCode::kInvalidArgument);
    EXPECT_EQ(out, before);
    EXPECT_STREQ(error.field_path, "name");
    EXPECT_STREQ(error.message, test.message);
  }
}

TEST(JsonConfigDocumentTest, TextHelperRejectsMissingOutputCapacity) {
  ConfigFile file(R"({"value":"text"})");
  ASSERT_TRUE(file.valid());
  JsonConfigDocument source;
  ASSERT_TRUE(source.Load(file.path()).ok());
  simdjson::ondemand::parser parser;
  simdjson::ondemand::document document;
  simdjson::ondemand::object object;
  ConfigError error;
  ASSERT_TRUE(
      internal::ParseConfigObject(source, parser, document, object, error)
          .ok());
  simdjson::ondemand::value value;
  ASSERT_EQ(object["value"].get(value), simdjson::SUCCESS);
  EXPECT_EQ(internal::ReadConfigText(value, {}, "value", error).code,
            ErrorCode::kInvalidArgument);
  EXPECT_STREQ(error.message, "empty, overlong or NUL-containing string");
}

TEST(JsonConfigDocumentTest, ParserErrorMapsAllocationFailureSeparately) {
  ConfigError error;
  EXPECT_EQ(internal::JsonError(simdjson::MEMALLOC, "field", error).code,
            ErrorCode::kResourceExhausted);
  EXPECT_STREQ(error.field_path, "field");
  EXPECT_NE(error.message[0], '\0');
  EXPECT_EQ(internal::JsonError(simdjson::INCORRECT_TYPE, "$", error).code,
            ErrorCode::kInvalidArgument);
  EXPECT_STREQ(error.field_path, "$");
}

}  // namespace
}  // namespace yquant::base

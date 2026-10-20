#include "marketd/config.h"

#include <cstdio>
#include <string>

#include "gtest/gtest.h"
#include "test_support.h"

namespace yquant::marketd {
namespace {

using testing::RuntimeDirectory;

// Every fixture creates only input JSON; validation must not publish discovery.
class ConfigTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(directory_.valid());
    ASSERT_TRUE(directory_.Write("mock.json", testing::kMockJson));
  }
  base::Status Load(std::string_view source, std::string_view publishers,
                    std::string_view extra = "") {
    EXPECT_TRUE(directory_.Write(
        "marketd.json", "{\"source\":" + std::string(source) +
                            ",\"publishers\":" + std::string(publishers) +
                            std::string(extra) + "}"));
    return loader_.Load(directory_.File("marketd.json").c_str());
  }
  std::string Shm(std::string_view fields = "") const {
    return "{\"adapter\":\"shm\",\"runtime_directory\":\"" +
           std::string(directory_.path()) + "\",\"stream\":\"quotes\"" +
           std::string(fields) + "}";
  }
  RuntimeDirectory directory_;
  ConfigLoader loader_;
};

TEST_F(ConfigTest, LoadsRelativeMockAndPublisherDefaultsWithoutOutputs) {
  ASSERT_TRUE(
      Load(R"({"adapter":"mock","config":"mock.json"})", "[" + Shm() + "]")
          .ok());
  EXPECT_EQ(loader_.config().source.adapter, Adapter::kMock);
  EXPECT_EQ(loader_.config().source.mock.source_id.value, 42U);
  ASSERT_EQ(loader_.config().publisher_count, 1U);
  EXPECT_EQ(loader_.config().publishers[0].capacity, 1024U);
  EXPECT_EQ(loader_.config().publishers[0].max_payload_size, 256U);
  EXPECT_FALSE(std::filesystem::exists(directory_.File("quotes.lock")));
}

TEST_F(ConfigTest, LoadsTcpInputAndMixedOutputsIndependentOfKeyOrder) {
  ASSERT_TRUE(
      Load(
          R"({"port":19100,"address":"127.0.0.1","adapter":"tcp"})",
          "[" + Shm() +
              R"(,{"port":19101,"max_clients":8,"adapter":"tcp","address":"0.0.0.0"}])")
          .ok());
  EXPECT_EQ(loader_.config().source.adapter, Adapter::kTcp);
  EXPECT_EQ(loader_.config().source.tcp.port, 19100);
  EXPECT_EQ(loader_.config().publisher_count, 2U);
  EXPECT_EQ(loader_.config().publishers[1].max_clients, 8U);
}

TEST_F(ConfigTest, SharedMemoryValidationMatchesTransportLimits) {
  const std::string mock = R"({"adapter":"mock","config":"mock.json"})";
  EXPECT_FALSE(
      Load(mock,
           "[" + Shm(",\"capacity\":16777216,\"max_payload_size\":4096") + "]")
          .ok());
  EXPECT_FALSE(std::filesystem::exists(directory_.File("quotes.lock")));
  const auto invalid_stream = "{\"adapter\":\"shm\",\"runtime_directory\":\"" +
                              std::string(directory_.path()) +
                              "\",\"stream\":\"_quotes\"}";
  EXPECT_FALSE(Load(invalid_stream, "[" + Shm() + "]").ok());
  const auto parent_path = "{\"adapter\":\"shm\",\"runtime_directory\":\"" +
                           std::string(directory_.path()) +
                           "/../other\",\"stream\":\"quotes\"}";
  EXPECT_FALSE(Load(parent_path, "[" + Shm() + "]").ok());
}

TEST_F(ConfigTest, RejectsUnknownAndAdapterSpecificFields) {
  const std::string valid = R"({"adapter":"mock","config":"mock.json"})";
  EXPECT_FALSE(Load(valid, "[" + Shm() + "]", ",\"unknown\":1").ok());
  EXPECT_FALSE(Load(R"({"adapter":"mock","config":"mock.json","port":1})",
                    "[" + Shm() + "]")
                   .ok());
  EXPECT_STREQ(loader_.error().field_path, "source.port");
  EXPECT_FALSE(Load(valid, "[" + Shm(",\"config\":\"mock.json\"") + "]").ok());
  EXPECT_FALSE(Load(valid, "[" + Shm(",\"bad\":true") + "]").ok());
}

TEST_F(ConfigTest, RejectsDecodedDuplicateKeysAndWrongTypes) {
  EXPECT_FALSE(
      Load(R"({"adapter":"mock","\u0061dapter":"mock","config":"mock.json"})",
           "[" + Shm() + "]")
          .ok());
  EXPECT_FALSE(
      Load(R"({"adapter":1,"config":"mock.json"})", "[" + Shm() + "]").ok());
  EXPECT_FALSE(Load(R"({"adapter":"mock","config":"mock.json"})",
                    "[" + Shm(",\"capacity\":4.5") + "]")
                   .ok());
  EXPECT_FALSE(Load(R"({"adapter":"mock","config":"mock.json"})",
                    "[" + Shm(",\"capacity\":-1") + "]")
                   .ok());
}

TEST_F(ConfigTest, RejectsMissingFieldsAndPublisherBounds) {
  EXPECT_FALSE(Load(R"({"adapter":"mock"})", "[" + Shm() + "]").ok());
  EXPECT_FALSE(Load(R"({"adapter":"mock","config":"mock.json"})", "[]").ok());
  EXPECT_FALSE(Load(R"({"adapter":"mock","config":"mock.json"})", "{}").ok());
  std::string outputs = "[";
  for (int i = 0; i < 9; ++i) {
    if (i != 0) outputs += ',';
    outputs += Shm();
  }
  outputs += ']';
  EXPECT_FALSE(
      Load(R"({"adapter":"mock","config":"mock.json"})", outputs).ok());
}

TEST_F(ConfigTest, RejectsInvalidCapacityAndPayloadSize) {
  const std::string source = R"({"adapter":"mock","config":"mock.json"})";
  for (const auto field :
       {",\"capacity\":0", ",\"capacity\":3", ",\"capacity\":33554432",
        ",\"max_payload_size\":79", ",\"max_payload_size\":4097"}) {
    EXPECT_FALSE(Load(source, "[" + Shm(field) + "]").ok()) << field;
  }
  EXPECT_TRUE(
      Load(source, "[" + Shm(",\"capacity\":2,\"max_payload_size\":80") + "]")
          .ok());
}

TEST_F(ConfigTest, RejectsNetworkAndStringBoundViolations) {
  const std::string output = "[" + Shm() + "]";
  for (const auto source :
       {R"({"adapter":"tcp","address":"localhost","port":1})",
        R"({"adapter":"tcp","address":"127.0.0.1","port":0})",
        R"({"adapter":"tcp","address":"127.0.0.1","port":65536})",
        R"({"adapter":"tcp","address":"0.0.0.0","port":1})",
        R"({"adapter":"mock","config":"mock\u0000.json"})"}) {
    EXPECT_FALSE(Load(source, output).ok()) << source;
  }
  EXPECT_FALSE(
      Load(
          R"({"adapter":"mock","config":"mock.json"})",
          R"([{"adapter":"tcp","address":"127.0.0.1","port":1,"max_clients":0}])")
          .ok());
  EXPECT_FALSE(
      Load(
          R"({"adapter":"mock","config":"mock.json"})",
          R"([{"adapter":"tcp","address":"127.0.0.1","port":1,"max_clients":17}])")
          .ok());
  auto config = testing::LocalConfig(directory_.path());
  std::memset(config.publishers[0].stream, 'x',
              sizeof(config.publishers[0].stream));
  strategy::ConfigError error;
  EXPECT_FALSE(ValidateConfig(config, error).ok());
}

TEST_F(ConfigTest, RejectsDirectFeedbackAndNormalizedDuplicateEndpoints) {
  const auto shm = Shm();
  EXPECT_FALSE(Load(shm, "[" + shm + "]").ok());
  EXPECT_FALSE(Load(R"({"adapter":"mock","config":"mock.json"})",
                    "[" + shm + "," + shm + "]")
                   .ok());
  auto config = testing::LocalConfig(directory_.path());
  config.publishers[1] = config.publishers[0];
  std::snprintf(config.publishers[1].runtime_directory,
                sizeof(config.publishers[1].runtime_directory), "%s/./",
                directory_.path());
  config.publisher_count = 2;
  strategy::ConfigError error;
  EXPECT_FALSE(ValidateConfig(config, error).ok());
  EXPECT_FALSE(Load(R"({"adapter":"tcp","address":"127.0.0.1","port":19100})",
                    R"([{"adapter":"tcp","address":"0.0.0.0","port":19100}])")
                   .ok());
}

TEST_F(ConfigTest, AllowsRemoteUpstreamAndLocalListenerOnSamePort) {
  EXPECT_TRUE(Load(R"({"adapter":"tcp","address":"198.51.100.1","port":19100})",
                   R"([{"adapter":"tcp","address":"0.0.0.0","port":19100}])")
                  .ok());
  EXPECT_FALSE(Load(R"({"adapter":"tcp","address":"127.0.0.1","port":19100})",
                    R"([{"adapter":"tcp","address":"0.0.0.0","port":19100}])")
                   .ok());
}

TEST_F(ConfigTest, FailedReloadClearsPreviouslyValidConfiguration) {
  ASSERT_TRUE(
      Load(R"({"adapter":"mock","config":"mock.json"})", "[" + Shm() + "]")
          .ok());
  EXPECT_FALSE(
      Load(R"({"adapter":"mock","config":"missing.json"})", "[" + Shm() + "]")
          .ok());
  EXPECT_EQ(loader_.config().publisher_count, 0U);
  EXPECT_NE(std::strstr(loader_.error().field_path, "source.config"), nullptr);
}

TEST_F(ConfigTest, ResolvesReplayAndRecordingPathsWithoutOpeningFiles) {
  ASSERT_TRUE(
      Load(
          R"({"speed":2.5,"adapter":"replay","path":"history.csv",)"
          R"("format":"dolphindb_csv","source_id":7,)"
          R"("instrument_catalog":"catalog.json"})",
          R"([{"adapter":"record","path":"quotes.yqr","queue_capacity":4096}])")
          .ok())
      << loader_.error().message;
  const auto& config = loader_.config();
  EXPECT_EQ(config.source.adapter, Adapter::kReplay);
  EXPECT_EQ(config.source.replay.format, ReplayFormat::kDolphinDbCsv);
  EXPECT_STREQ(config.source.replay.path,
               directory_.File("history.csv").c_str());
  EXPECT_EQ(config.source.replay.speed, 2.5);
  EXPECT_EQ(config.source.replay.source_id.value, 7U);
  EXPECT_STREQ(config.source.replay.instrument_catalog,
               directory_.File("catalog.json").c_str());
  EXPECT_EQ(config.publishers[0].adapter, Adapter::kRecord);
  EXPECT_STREQ(config.publishers[0].record.path,
               directory_.File("quotes.yqr").c_str());
  EXPECT_EQ(config.publishers[0].record.queue_capacity, 4096U);
  EXPECT_FALSE(std::filesystem::exists(directory_.File("quotes.yqr")));
}

TEST_F(ConfigTest, LoadsRecordingReplayAndRejectsFileSchemaViolations) {
  const std::string output = R"([{"adapter":"record","path":"new.yqr"}])";
  const std::string recording =
      R"({"adapter":"replay","path":"old.yqr","format":"recording"})";
  ASSERT_TRUE(Load(recording, output).ok());
  EXPECT_EQ(loader_.config().source.replay.speed, 1.0);
  EXPECT_EQ(loader_.config().publishers[0].record.queue_capacity, 1048576U);
  for (
      const auto source :
      {R"({"adapter":"replay","path":"x"})",
       R"({"adapter":"replay","path":"x","format":"unknown"})",
       R"({"adapter":"replay","path":"x","format":"dolphindb_csv"})",
       R"({"adapter":"replay","path":"x","format":"dolphindb_csv","source_id":1})",
       R"({"adapter":"replay","path":"x","format":"dolphindb_csv","instrument_catalog":"instruments.json"})",
       R"({"adapter":"replay","path":"x","format":"dolphindb_csv","source_id":1,"price_decimals":4,"instrument_catalog":"instruments.json"})",
       R"({"adapter":"replay","path":"x","format":"dolphindb_csv","source_id":1,"instrument_catalog":""})",
       R"({"adapter":"replay","path":"x","format":"dolphindb_csv","source_id":1,"instrument_catalog":"bad\u0000.json"})",
       R"({"adapter":"replay","path":"x","format":"recording","speed":-1})",
       R"({"adapter":"replay","path":"x","format":"recording","max_records":0})",
       R"({"adapter":"replay","path":"x","format":"recording","max_records":2000001})",
       R"({"adapter":"replay","path":"x","format":"recording","source_id":1})",
       R"({"adapter":"replay","path":"x","format":"recording","instrument_catalog":"instruments.json"})",
       R"({"adapter":"replay","path":"x","format":"recording","price_decimals":4})",
       R"({"adapter":"record","path":"x"})"}) {
    EXPECT_FALSE(Load(source, output).ok()) << source;
  }
  for (const auto publisher :
       {R"([{"adapter":"record","path":"x","queue_capacity":128}])",
        R"([{"adapter":"record","path":"x","queue_capacity":1023}])",
        R"([{"adapter":"record","path":"x","queue_capacity":134217728}])",
        R"([{"adapter":"record","path":"x","speed":1}])",
        R"([{"adapter":"replay","path":"x","format":"recording"}])"}) {
    EXPECT_FALSE(Load(recording, publisher).ok()) << publisher;
  }
}

TEST_F(ConfigTest, RejectsRecordingFeedbackAndNormalizedDuplicatePaths) {
  const std::string recording =
      R"({"adapter":"replay","path":"old.yqr","format":"recording"})";
  EXPECT_FALSE(
      Load(recording, R"([{"adapter":"record","path":"./old.yqr"}])").ok());
  EXPECT_FALSE(Load(recording, R"([{"adapter":"record","path":"new.yqr"},)"
                               R"({"adapter":"record","path":"./new.yqr"}])")
                   .ok());
  EXPECT_FALSE(std::filesystem::exists(directory_.File("new.yqr")));
}

}  // namespace
}  // namespace yquant::marketd

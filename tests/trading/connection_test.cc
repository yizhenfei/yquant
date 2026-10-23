#include "yquant/trading/connection.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstring>
#include <span>
#include <string>
#include <thread>

#include "gtest/gtest.h"

namespace yquant::trading {
namespace {

constexpr std::uint64_t kConnectTimeoutNs = 1000000000;
constexpr auto kWaitTimeout = std::chrono::seconds(2);

trading::Message Heartbeat(std::uint64_t sequence = 1) {
  trading::Message message;
  message.type = trading::MessageType::kHeartbeat;
  message.generation = 31;
  message.sequence = sequence;
  return message;
}

trading::ReceiveResult WaitActive(trading::Connection& connection,
                                  trading::Message& output) {
  const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
  trading::ReceiveResult result;
  do {
    result = connection.Poll(output);
    if (result.state != trading::ReceiveState::kIdle) return result;
    std::this_thread::yield();
  } while (std::chrono::steady_clock::now() < deadline);
  return result;
}

// Nonblocking accept can remain idle after connect returns. Retries only idle
// results until the shared deadline; errors and timeouts report diagnostics.
::testing::AssertionResult WaitAccept(trading::Listener& listener,
                                      trading::Connection& connection,
                                      bool& accepted) {
  const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
  do {
    const auto status = listener.Accept(connection, accepted);
    if (!status.ok()) {
      return ::testing::AssertionFailure()
             << "Accept failed: code=" << static_cast<int>(status.code)
             << ", system_error=" << status.system_error.message()
             << ", detail=" << connection.error_detail();
    }
    if (accepted) return ::testing::AssertionSuccess();
    std::this_thread::yield();
  } while (std::chrono::steady_clock::now() < deadline);
  return ::testing::AssertionFailure()
         << "Accept queue remained empty for "
         << std::chrono::duration_cast<std::chrono::milliseconds>(kWaitTimeout)
                .count()
         << " ms";
}

class ConnectedEndpoints : public testing::Test {
 protected:
  void SetUp() override {
    trading::Endpoint endpoint;
    endpoint.port = 0;
    ASSERT_TRUE(listener_.Open(endpoint).ok());
    endpoint.port = listener_.port();
    ASSERT_TRUE(sender_.Connect(endpoint, kConnectTimeoutNs).ok());
    bool accepted = false;
    ASSERT_TRUE(WaitAccept(listener_, receiver_, accepted));
    ASSERT_TRUE(accepted);
  }
  trading::Listener listener_;
  trading::Connection sender_;
  trading::Connection receiver_;
};

class RawPeerEndpoints : public testing::Test {
 protected:
  void SetUp() override {
    trading::Endpoint endpoint;
    endpoint.port = 0;
    ASSERT_TRUE(listener_.Open(endpoint).ok());
    raw_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(raw_fd_, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(listener_.port());
    ASSERT_EQ(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr), 1);
    ASSERT_EQ(connect(raw_fd_, reinterpret_cast<sockaddr*>(&address),
                      sizeof(address)),
              0);
    const int enabled = 1;
    ASSERT_EQ(setsockopt(raw_fd_, IPPROTO_TCP, TCP_NODELAY, &enabled,
                         sizeof(enabled)),
              0);
#ifdef SO_NOSIGPIPE
    ASSERT_EQ(setsockopt(raw_fd_, SOL_SOCKET, SO_NOSIGPIPE, &enabled,
                         sizeof(enabled)),
              0);
#endif
    bool accepted = false;
    ASSERT_TRUE(WaitAccept(listener_, receiver_, accepted));
    ASSERT_TRUE(accepted);
  }
  void TearDown() override {
    if (raw_fd_ >= 0) close(raw_fd_);
  }
  void Send(std::span<const std::byte> bytes) {
    std::size_t offset = 0;
    while (offset != bytes.size()) {
#ifdef MSG_NOSIGNAL
      constexpr int kFlags = MSG_NOSIGNAL;
#else
      constexpr int kFlags = 0;
#endif
      const auto sent =
          send(raw_fd_, bytes.data() + offset, bytes.size() - offset, kFlags);
      ASSERT_GT(sent, 0);
      offset += static_cast<std::size_t>(sent);
    }
  }
  trading::Listener listener_;
  int raw_fd_ = -1;
  trading::Connection receiver_;
};

TEST_F(ConnectedEndpoints, IndependentSequencesAreAssignedInBothDirections) {
  auto message = Heartbeat(9999);  // Caller sequence does not enter the stream.
  ASSERT_TRUE(sender_.Queue(message).ok());
  ASSERT_TRUE(sender_.Queue(message).ok());
  ASSERT_TRUE(receiver_.Queue(message).ok());
  ASSERT_TRUE(sender_.Service().ok());
  trading::Message output;
  ASSERT_EQ(WaitActive(receiver_, output).state,
            trading::ReceiveState::kMessage);
  EXPECT_EQ(output.sequence, 1);
  EXPECT_EQ(output.generation, 31);
  ASSERT_EQ(WaitActive(receiver_, output).state,
            trading::ReceiveState::kMessage);
  EXPECT_EQ(output.sequence, 2);
  ASSERT_EQ(WaitActive(sender_, output).state, trading::ReceiveState::kMessage);
  EXPECT_EQ(output.sequence, 1);
}

TEST_F(ConnectedEndpoints,
       FullQueueNeverOverwritesAndRejectedQueueConsumesNoSequence) {
  for (std::size_t index = 0; index < 64; ++index)
    ASSERT_TRUE(sender_.Queue(Heartbeat()).ok());
  EXPECT_FALSE(sender_.can_queue());
  EXPECT_FALSE(sender_.empty());
  EXPECT_EQ(sender_.Queue(Heartbeat()).code,
            base::ErrorCode::kResourceExhausted);
  trading::Message output;
  for (std::uint64_t sequence = 1; sequence <= 64; ++sequence) {
    ASSERT_TRUE(sender_.Service().ok());
    ASSERT_EQ(WaitActive(receiver_, output).state,
              trading::ReceiveState::kMessage);
    EXPECT_EQ(output.sequence, sequence);
  }
  EXPECT_TRUE(sender_.empty());
  EXPECT_TRUE(sender_.can_queue());
  ASSERT_TRUE(sender_.Queue(Heartbeat()).ok());
  ASSERT_TRUE(sender_.Service().ok());
  ASSERT_EQ(WaitActive(receiver_, output).state,
            trading::ReceiveState::kMessage);
  EXPECT_EQ(output.sequence, 65);
}

TEST_F(ConnectedEndpoints,
       InvalidMessageDoesNotConsumeSequenceOrCloseConnection) {
  auto message = Heartbeat();
  message.type =
      trading::MessageType::kHello;  // Missing credentials and instruments.
  EXPECT_EQ(sender_.Queue(message).code, base::ErrorCode::kInvalidArgument);
  EXPECT_TRUE(sender_.open());
  EXPECT_TRUE(sender_.empty());
  ASSERT_TRUE(sender_.Queue(Heartbeat()).ok());
  ASSERT_TRUE(sender_.Service().ok());
  trading::Message output;
  ASSERT_EQ(WaitActive(receiver_, output).state,
            trading::ReceiveState::kMessage);
  EXPECT_EQ(output.sequence, 1);
}

TEST_F(ConnectedEndpoints, IdleAndClosedLeaveOutputUnchanged) {
  trading::Message output;
  output.client_id = 37;
  output.sequence = 91;
  EXPECT_EQ(receiver_.Poll(output).state, trading::ReceiveState::kIdle);
  EXPECT_EQ(output.client_id, 37);
  EXPECT_EQ(output.sequence, 91);
  sender_.Close();
  EXPECT_EQ(WaitActive(receiver_, output).state,
            trading::ReceiveState::kClosed);
  EXPECT_FALSE(receiver_.open());
  EXPECT_EQ(receiver_.Poll(output).state, trading::ReceiveState::kClosed);
  EXPECT_EQ(output.client_id, 37);
  EXPECT_EQ(output.sequence, 91);
  EXPECT_EQ(receiver_.Queue(Heartbeat()).code, base::ErrorCode::kInvalidState);
}

TEST_F(RawPeerEndpoints, FragmentedHeaderAndBodyAreRetainedAcrossPolls) {
  trading::Message message;
  message.type = trading::MessageType::kNewOrder;
  message.sequence = 1;
  message.generation = 51;
  message.client_order_id = {5};
  message.request = {{0x1000927c0ULL}, model::OrderSide::kBuy, 1000, 300};
  std::array<std::byte, trading::kMaxFrameSize> bytes{};
  std::size_t size = 0;
  ASSERT_TRUE(trading::EncodeMessage(message, bytes, size).ok());
  trading::Message output;
  output.client_id = 39;
  Send(std::span(bytes).first(7));
  EXPECT_EQ(receiver_.Poll(output).state, trading::ReceiveState::kIdle);
  EXPECT_EQ(output.client_id, 39);
  Send(std::span(bytes).subspan(7, 25));
  EXPECT_EQ(receiver_.Poll(output).state, trading::ReceiveState::kIdle);
  Send(std::span(bytes).subspan(32, 3));
  EXPECT_EQ(receiver_.Poll(output).state, trading::ReceiveState::kIdle);
  Send(std::span(bytes).subspan(35, size - 35));
  ASSERT_EQ(WaitActive(receiver_, output).state,
            trading::ReceiveState::kMessage);
  EXPECT_EQ(output.type, trading::MessageType::kNewOrder);
  EXPECT_EQ(output.request.quantity, 300);
  EXPECT_EQ(output.sequence, 1);
}

TEST_F(RawPeerEndpoints, CoalescedFramesAreDeliveredBeforePeerEof) {
  std::array<std::byte, trading::kMaxFrameSize> bytes{};
  std::size_t first_size = 0;
  ASSERT_TRUE(trading::EncodeMessage(Heartbeat(1), bytes, first_size).ok());
  std::size_t second_size = 0;
  ASSERT_TRUE(trading::EncodeMessage(Heartbeat(2),
                                     std::span(bytes).subspan(first_size),
                                     second_size)
                  .ok());
  Send(std::span(bytes).first(first_size + second_size));
  ASSERT_EQ(shutdown(raw_fd_, SHUT_WR), 0);
  trading::Message output;
  ASSERT_EQ(WaitActive(receiver_, output).state,
            trading::ReceiveState::kMessage);
  EXPECT_EQ(output.sequence, 1);
  ASSERT_EQ(WaitActive(receiver_, output).state,
            trading::ReceiveState::kMessage);
  EXPECT_EQ(output.sequence, 2);
  ASSERT_EQ(WaitActive(receiver_, output).state,
            trading::ReceiveState::kClosed);
  EXPECT_EQ(output.sequence, 2);
}

TEST_F(RawPeerEndpoints, EofInHeaderIsCorruptAndDiagnosticSurvivesClose) {
  std::array<std::byte, trading::kMaxFrameSize> bytes{};
  std::size_t size = 0;
  ASSERT_TRUE(trading::EncodeMessage(Heartbeat(), bytes, size).ok());
  Send(std::span(bytes).first(5));
  ASSERT_EQ(shutdown(raw_fd_, SHUT_WR), 0);
  trading::Message output;
  output.client_id = 89;
  const auto result = WaitActive(receiver_, output);
  EXPECT_EQ(result.state, trading::ReceiveState::kError);
  EXPECT_EQ(result.status.code, base::ErrorCode::kCorrupt);
  EXPECT_EQ(output.client_id, 89);
  EXPECT_FALSE(receiver_.open());
  const std::string detail(receiver_.error_detail());
  EXPECT_FALSE(detail.empty());
  receiver_.Close();
  EXPECT_EQ(receiver_.error_detail(), detail);
}

TEST_F(RawPeerEndpoints, EofInBodyIsCorrupt) {
  trading::Message message;
  message.type = trading::MessageType::kError;
  message.sequence = 1;
  message.error_code = base::ErrorCode::kCorrupt;
  message.reason.Set(4, "broken");
  std::array<std::byte, trading::kMaxFrameSize> bytes{};
  std::size_t size = 0;
  ASSERT_TRUE(trading::EncodeMessage(message, bytes, size).ok());
  Send(std::span(bytes).first(size - 1));
  ASSERT_EQ(shutdown(raw_fd_, SHUT_WR), 0);
  trading::Message output;
  EXPECT_EQ(WaitActive(receiver_, output).status.code,
            base::ErrorCode::kCorrupt);
}

TEST_F(RawPeerEndpoints, RejectsMissingFirstSequence) {
  std::array<std::byte, trading::kMaxFrameSize> bytes{};
  std::size_t size = 0;
  ASSERT_TRUE(trading::EncodeMessage(Heartbeat(2), bytes, size).ok());
  Send(std::span(bytes).first(size));
  trading::Message output;
  output.client_id = 29;
  const auto result = WaitActive(receiver_, output);
  EXPECT_EQ(result.state, trading::ReceiveState::kError);
  EXPECT_EQ(result.status.code, base::ErrorCode::kCorrupt);
  EXPECT_EQ(output.client_id, 29);
  EXPECT_EQ(receiver_.Poll(output).state, trading::ReceiveState::kError);
}

TEST_F(RawPeerEndpoints, RejectsDuplicateSequenceAfterOneAcceptedFrame) {
  std::array<std::byte, trading::kMaxFrameSize> bytes{};
  std::size_t size = 0;
  ASSERT_TRUE(trading::EncodeMessage(Heartbeat(), bytes, size).ok());
  Send(std::span(bytes).first(size));
  Send(std::span(bytes).first(size));
  trading::Message output;
  ASSERT_EQ(WaitActive(receiver_, output).state,
            trading::ReceiveState::kMessage);
  EXPECT_EQ(WaitActive(receiver_, output).state, trading::ReceiveState::kError);
  EXPECT_EQ(output.sequence, 1);
}

TEST_F(RawPeerEndpoints, RejectsInvalidHeaderWithoutReadingDeclaredBody) {
  std::array<std::byte, trading::kMaxFrameSize> bytes{};
  std::size_t size = 0;
  ASSERT_TRUE(trading::EncodeMessage(Heartbeat(), bytes, size).ok());
  bytes[12] = std::byte{1};
  Send(std::span(bytes).first(size));
  trading::Message output;
  EXPECT_EQ(WaitActive(receiver_, output).status.code,
            base::ErrorCode::kCorrupt);
}

TEST(TradingConnection, AdoptionFailureRetainsCallerDescriptorOwnership) {
  int descriptors[2];
  ASSERT_EQ(pipe(descriptors), 0);
  trading::Connection connection;
  EXPECT_FALSE(connection.Adopt(descriptors[0]).ok());
  EXPECT_FALSE(connection.open());
  EXPECT_GE(fcntl(descriptors[0], F_GETFD), 0);
  close(descriptors[0]);
  close(descriptors[1]);
}

TEST(TradingConnection, RejectsInvalidAddressesAndUseWhileClosed) {
  trading::Connection connection;
  EXPECT_TRUE(connection.empty());
  EXPECT_FALSE(connection.open());
  EXPECT_FALSE(connection.can_queue());
  EXPECT_EQ(connection.Adopt(-1).code, base::ErrorCode::kInvalidArgument);
  EXPECT_EQ(connection.Service().code, base::ErrorCode::kInvalidState);
  EXPECT_EQ(connection.Queue(Heartbeat()).code, base::ErrorCode::kInvalidState);
  trading::Endpoint endpoint;
  std::memcpy(endpoint.address, "localhost", 10);
  EXPECT_EQ(connection.Connect(endpoint, kConnectTimeoutNs).code,
            base::ErrorCode::kInvalidArgument);
  trading::Listener listener;
  EXPECT_EQ(listener.Open(endpoint).code, base::ErrorCode::kInvalidArgument);
  endpoint = {};
  EXPECT_EQ(connection.Connect(endpoint, 0).code,
            base::ErrorCode::kInvalidArgument);
  endpoint.port = 0;
  EXPECT_EQ(connection.Connect(endpoint, kConnectTimeoutNs).code,
            base::ErrorCode::kInvalidArgument);
  bool accepted = true;
  EXPECT_EQ(listener.Accept(connection, accepted).code,
            base::ErrorCode::kInvalidState);
  EXPECT_FALSE(accepted);
}

TEST(TradingConnection, EmptyAcceptQueueDoesNotModifyConnection) {
  trading::Endpoint endpoint;
  endpoint.port = 0;
  trading::Listener listener;
  ASSERT_TRUE(listener.Open(endpoint).ok());
  EXPECT_NE(listener.port(), 0);
  EXPECT_EQ(listener.Open(endpoint).code, base::ErrorCode::kInvalidState);
  trading::Connection connection;
  bool accepted = true;
  EXPECT_TRUE(listener.Accept(connection, accepted).ok());
  EXPECT_FALSE(accepted);
  EXPECT_FALSE(connection.open());
  listener.Close();
  EXPECT_EQ(listener.port(), 0);
}

TEST(TradingConnection, RefusedConnectLeavesObjectClosedAndReusable) {
  trading::Endpoint endpoint;
  endpoint.port = 0;
  trading::Listener listener;
  ASSERT_TRUE(listener.Open(endpoint).ok());
  endpoint.port = listener.port();
  listener.Close();
  trading::Connection connection;
  EXPECT_EQ(connection.Connect(endpoint, kConnectTimeoutNs).code,
            base::ErrorCode::kIoError);
  EXPECT_FALSE(connection.open());
  ASSERT_TRUE(listener.Open(endpoint).ok());
  EXPECT_TRUE(connection.Connect(endpoint, kConnectTimeoutNs).ok());
}

}  // namespace
}  // namespace yquant::trading

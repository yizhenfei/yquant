#include "yquant/market/tcp_transport.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <span>
#include <thread>

#include "gtest/gtest.h"
#include "yquant/market/quote_codec.h"

namespace yquant::market {
namespace {

using Clock = std::chrono::steady_clock;
constexpr uint64_t kReceiveTime = 123456;

model::QuoteTick Quote(uint64_t sequence = 1) {
  model::QuoteTick quote;
  quote.source_id.value = 13;
  quote.instrument_id.value = (uint64_t{1} << 32) | 600000;
  quote.sequence = sequence;
  quote.event_time_ns = 1000 + sequence;
  quote.receive_time_ns = 999;
  quote.bid_price = 10000;
  quote.bid_qty = 100;
  quote.ask_price = 10001;
  quote.ask_qty = 200;
  quote.flags = model::kKnownQuoteFlags;
  return quote;
}

bool Nonblocking(int descriptor) {
  const int flags = ::fcntl(descriptor, F_GETFL, 0);
  return flags >= 0 && ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0;
}

// A raw peer lets tests choose exact frame boundaries independently of send().
class RawServer {
 public:
  ~RawServer() {
    ClosePeer();
    if (listener_ >= 0) {
      ::close(listener_);
    }
  }

  bool Init() {
    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener_ < 0 || !Nonblocking(listener_)) {
      return false;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(listener_, reinterpret_cast<sockaddr*>(&address),
               sizeof(address)) != 0 ||
        ::listen(listener_, 4) != 0) {
      return false;
    }
    socklen_t size = sizeof(address);
    if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&address),
                      &size) != 0) {
      return false;
    }
    port_ = ntohs(address.sin_port);
    return true;
  }

  bool Accept() {
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    while (Clock::now() < deadline) {
      peer_ = ::accept(listener_, nullptr, nullptr);
      if (peer_ >= 0) {
        return Nonblocking(peer_);
      }
      if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        return false;
      }
      std::this_thread::yield();
    }
    return false;
  }

  bool Send(std::span<const std::byte> bytes) {
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    while (!bytes.empty() && Clock::now() < deadline) {
      const auto count = ::send(peer_, bytes.data(), bytes.size(), 0);
      if (count > 0) {
        bytes = bytes.subspan(static_cast<size_t>(count));
      } else if (count < 0 &&
                 (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        std::this_thread::yield();
      } else {
        return false;
      }
    }
    return bytes.empty();
  }

  void ClosePeer() {
    if (peer_ >= 0) {
      ::close(peer_);
    }
    peer_ = -1;
  }
  uint16_t port() const { return port_; }

 private:
  int listener_ = -1;
  int peer_ = -1;
  uint16_t port_ = 0;
};

// A connected receiver with a deliberately small advertised window.
class IdleClient {
 public:
  ~IdleClient() {
    if (descriptor_ >= 0) {
      ::close(descriptor_);
    }
  }
  bool Connect(uint16_t port) {
    descriptor_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (descriptor_ < 0 || !Nonblocking(descriptor_)) {
      return false;
    }
    const int receive_bytes = 1024;
    if (::setsockopt(descriptor_, SOL_SOCKET, SO_RCVBUF, &receive_bytes,
                     sizeof(receive_bytes)) != 0) {
      return false;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (::connect(descriptor_, reinterpret_cast<sockaddr*>(&address),
                  sizeof(address)) == 0) {
      return true;
    }
    if (errno != EINPROGRESS) {
      return false;
    }
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    while (Clock::now() < deadline) {
      pollfd descriptor{descriptor_, POLLOUT, 0};
      const int ready = ::poll(&descriptor, 1, 0);
      if (ready > 0) {
        int error = 0;
        socklen_t size = sizeof(error);
        return ::getsockopt(descriptor_, SOL_SOCKET, SO_ERROR, &error, &size) ==
                   0 &&
               error == 0;
      }
      if (ready < 0 && errno != EINTR) {
        return false;
      }
      std::this_thread::yield();
    }
    return false;
  }

 private:
  int descriptor_ = -1;
};

// Nonblocking connect can finish before accept runs, so publication starts
// only after every intended downstream has its own queue on the publisher.
bool WaitForClients(TcpPublisher& publisher, size_t expected) {
  const auto deadline = Clock::now() + std::chrono::seconds(3);
  while (Clock::now() < deadline) {
    const auto status = publisher.Service(0);
    if (!status.ok()) {
      ADD_FAILURE() << publisher.error_detail();
      return false;
    }
    if (publisher.connected_clients() == expected) {
      return true;
    }
    std::this_thread::yield();
  }
  ADD_FAILURE() << "TCP accepted clients: " << publisher.connected_clients()
                << ", expected: " << expected;
  return false;
}

PollResult Await(TcpSource& source, model::QuoteTick& out) {
  const auto deadline = Clock::now() + std::chrono::seconds(3);
  PollResult result;
  while (Clock::now() < deadline) {
    result = source.Poll(kReceiveTime, out);
    if (result.state != PollState::kIdle) {
      return result;
    }
    std::this_thread::yield();
  }
  ADD_FAILURE() << "TCP source remained idle until deadline";
  return result;
}

void ExpectQuote(const model::QuoteTick& actual, const model::QuoteTick& sent) {
  auto expected = sent;
  expected.receive_time_ns = kReceiveTime;
  EXPECT_EQ(actual, expected);
}

TEST(TcpTransportTest, EphemeralPortFansOutCompleteFramesToTwoClients) {
  TcpPublisher publisher;
  ASSERT_TRUE(publisher.Init({}).ok()) << publisher.error_detail();
  ASSERT_NE(publisher.port(), 0);
  const TcpEndpoint endpoint{"127.0.0.1", publisher.port()};
  TcpSource first;
  TcpSource second;
  ASSERT_TRUE(first.Init(endpoint).ok()) << first.error_detail();
  ASSERT_TRUE(second.Init(endpoint).ok()) << second.error_detail();
  ASSERT_TRUE(WaitForClients(publisher, 2));
  auto output = Quote(999);
  EXPECT_EQ(first.Poll(kReceiveTime, output).state, PollState::kIdle);
  EXPECT_EQ(output, Quote(999));
  for (uint64_t sequence = 1; sequence <= 128; ++sequence) {
    const auto quote = Quote(sequence);
    ASSERT_TRUE(publisher.Publish(quote).ok());
    ASSERT_EQ(Await(first, output).state, PollState::kEvent);
    ExpectQuote(output, quote);
    ASSERT_EQ(Await(second, output).state, PollState::kEvent);
    ExpectQuote(output, quote);
  }
  EXPECT_EQ(publisher.disconnected_clients(), 0);
}

TEST(TcpTransportTest, PartialFramesReassembleWithoutChangingOutputEarly) {
  RawServer server;
  ASSERT_TRUE(server.Init());
  TcpSource source;
  ASSERT_TRUE(source.Init({"127.0.0.1", server.port()}).ok());
  ASSERT_TRUE(server.Accept());
  market::QuoteFrame frame;
  ASSERT_TRUE(market::EncodeQuote(Quote(42), frame).ok());
  auto output = Quote(999);
  ASSERT_TRUE(server.Send(std::span(frame).first(5)));
  EXPECT_EQ(source.Poll(kReceiveTime, output).state, PollState::kIdle);
  EXPECT_EQ(output, Quote(999));
  ASSERT_TRUE(server.Send(std::span(frame).subspan(5, 38)));
  EXPECT_EQ(source.Poll(kReceiveTime, output).state, PollState::kIdle);
  EXPECT_EQ(output, Quote(999));
  ASSERT_TRUE(server.Send(std::span(frame).subspan(43)));
  ASSERT_EQ(Await(source, output).state, PollState::kEvent);
  ExpectQuote(output, Quote(42));
  EXPECT_EQ(source.Poll(kReceiveTime, output).state, PollState::kIdle);
  ExpectQuote(output, Quote(42));
}

TEST(TcpTransportTest, CoalescedFramesPreserveTheirBoundaries) {
  RawServer server;
  ASSERT_TRUE(server.Init());
  TcpSource source;
  ASSERT_TRUE(source.Init({"127.0.0.1", server.port()}).ok());
  ASSERT_TRUE(server.Accept());
  std::array<std::byte, 2 * market::kQuoteFrameSize> bytes;
  market::QuoteFrame first;
  market::QuoteFrame second;
  ASSERT_TRUE(market::EncodeQuote(Quote(1), first).ok());
  ASSERT_TRUE(market::EncodeQuote(Quote(2), second).ok());
  std::copy(first.begin(), first.end(), bytes.begin());
  std::copy(second.begin(), second.end(), bytes.begin() + first.size());
  ASSERT_TRUE(server.Send(bytes));
  model::QuoteTick output;
  ASSERT_EQ(Await(source, output).state, PollState::kEvent);
  ExpectQuote(output, Quote(1));
  ASSERT_EQ(Await(source, output).state, PollState::kEvent);
  ExpectQuote(output, Quote(2));
}

TEST(TcpTransportTest, InvalidFrameFailureIsTerminalAndPreservesOutput) {
  RawServer server;
  ASSERT_TRUE(server.Init());
  TcpSource source;
  ASSERT_TRUE(source.Init({"127.0.0.1", server.port()}).ok());
  ASSERT_TRUE(server.Accept());
  market::QuoteFrame invalid;
  ASSERT_TRUE(market::EncodeQuote(Quote(), invalid).ok());
  invalid[4] ^= std::byte{0xff};
  ASSERT_TRUE(server.Send(invalid));
  auto output = Quote(999);
  const auto error = Await(source, output);
  ASSERT_EQ(error.state, PollState::kError);
  EXPECT_EQ(error.status.code, base::ErrorCode::kCorrupt);
  EXPECT_EQ(output, Quote(999));
  market::QuoteFrame valid;
  ASSERT_TRUE(market::EncodeQuote(Quote(2), valid).ok());
  ASSERT_TRUE(server.Send(valid));
  const auto again = source.Poll(kReceiveTime, output);
  EXPECT_EQ(again.state, PollState::kError);
  EXPECT_EQ(again.status.code, error.status.code);
  EXPECT_EQ(output, Quote(999));
}

TEST(TcpTransportTest, PartialFrameDisconnectIsTerminalCorruption) {
  RawServer server;
  ASSERT_TRUE(server.Init());
  TcpSource source;
  ASSERT_TRUE(source.Init({"127.0.0.1", server.port()}).ok());
  ASSERT_TRUE(server.Accept());
  market::QuoteFrame frame;
  ASSERT_TRUE(market::EncodeQuote(Quote(), frame).ok());
  ASSERT_TRUE(server.Send(std::span(frame).first(17)));
  server.ClosePeer();
  auto output = Quote(999);
  const auto result = Await(source, output);
  EXPECT_EQ(result.state, PollState::kError);
  EXPECT_EQ(result.status.code, base::ErrorCode::kCorrupt);
  EXPECT_EQ(output, Quote(999));
  EXPECT_EQ(source.Poll(kReceiveTime, output).state, PollState::kError);
}

TEST(TcpTransportTest, CompleteFrameCanDrainBeforeTerminalDisconnect) {
  RawServer server;
  ASSERT_TRUE(server.Init());
  TcpSource source;
  ASSERT_TRUE(source.Init({"127.0.0.1", server.port()}).ok());
  ASSERT_TRUE(server.Accept());
  market::QuoteFrame frame;
  ASSERT_TRUE(market::EncodeQuote(Quote(), frame).ok());
  ASSERT_TRUE(server.Send(frame));
  server.ClosePeer();
  model::QuoteTick output;
  ASSERT_EQ(Await(source, output).state, PollState::kEvent);
  ExpectQuote(output, Quote());
  const auto before = output;
  const auto result = Await(source, output);
  EXPECT_EQ(result.state, PollState::kError);
  EXPECT_EQ(result.status.code, base::ErrorCode::kIoError);
  EXPECT_EQ(output, before);
}

TEST(TcpTransportTest, ClosingOneClientPreservesOtherClientProgress) {
  TcpPublisher publisher;
  ASSERT_TRUE(publisher.Init({}, 2).ok());
  const TcpEndpoint endpoint{"127.0.0.1", publisher.port()};
  TcpSource closed;
  TcpSource healthy;
  ASSERT_TRUE(closed.Init(endpoint).ok());
  ASSERT_TRUE(healthy.Init(endpoint).ok());
  ASSERT_TRUE(WaitForClients(publisher, 2));
  ASSERT_TRUE(publisher.Publish(Quote()).ok());
  model::QuoteTick output;
  ASSERT_EQ(Await(closed, output).state, PollState::kEvent);
  ASSERT_EQ(Await(healthy, output).state, PollState::kEvent);
  closed.Close();
  ASSERT_TRUE(WaitForClients(publisher, 1));
  for (uint64_t sequence = 2; sequence <= 16; ++sequence) {
    ASSERT_TRUE(publisher.Publish(Quote(sequence)).ok());
    ASSERT_EQ(Await(healthy, output).state, PollState::kEvent);
    ExpectQuote(output, Quote(sequence));
  }
  EXPECT_EQ(publisher.disconnected_clients(), 1);
}

TEST(TcpTransportTest, IdleDisconnectReleasesFullListenerWithoutPublication) {
  TcpPublisher publisher;
  ASSERT_TRUE(publisher.Init({}, 1).ok());
  const TcpEndpoint endpoint{"127.0.0.1", publisher.port()};
  TcpSource previous;
  ASSERT_TRUE(previous.Init(endpoint).ok());
  ASSERT_TRUE(WaitForClients(publisher, 1));
  auto output = Quote(999);
  EXPECT_EQ(previous.Poll(kReceiveTime, output).state, PollState::kIdle);
  ASSERT_TRUE(publisher.Service(0).ok());
  previous.Close();

  // Service must free the only client slot while the market is idle.
  ASSERT_TRUE(WaitForClients(publisher, 0));
  ASSERT_EQ(publisher.disconnected_clients(), 1);
  EXPECT_EQ(output, Quote(999));

  TcpSource replacement;
  ASSERT_TRUE(replacement.Init(endpoint).ok());
  ASSERT_TRUE(WaitForClients(publisher, 1));
  EXPECT_EQ(replacement.Poll(kReceiveTime, output).state, PollState::kIdle);
  ASSERT_TRUE(publisher.Service(0).ok());
  ASSERT_TRUE(publisher.Publish(Quote(7)).ok());
  ASSERT_EQ(Await(replacement, output).state, PollState::kEvent);
  ExpectQuote(output, Quote(7));
  EXPECT_EQ(publisher.disconnected_clients(), 1);
}

TEST(TcpTransportTest, SlowClientQueueOverflowIsIsolated) {
  TcpPublisher publisher;
  ASSERT_TRUE(publisher.Init({}, 2).ok());
  IdleClient slow;
  ASSERT_TRUE(slow.Connect(publisher.port()));
  TcpSource healthy;
  ASSERT_TRUE(healthy.Init({"127.0.0.1", publisher.port()}).ok());
  ASSERT_TRUE(WaitForClients(publisher, 2));
  model::QuoteTick output;
  uint64_t sequence = 0;
  const auto deadline = Clock::now() + std::chrono::seconds(3);
  // The idle socket eventually fills both its TCP receive window and the
  // publisher's bounded queue, while each healthy frame is drained immediately.
  while (publisher.disconnected_clients() == 0 && Clock::now() < deadline &&
         sequence < 500'000) {
    ++sequence;
    ASSERT_TRUE(publisher.Publish(Quote(sequence)).ok());
    PollResult result;
    do {
      result = healthy.Poll(kReceiveTime, output);
      if (result.state != PollState::kIdle) {
        break;
      }
      std::this_thread::yield();
    } while (Clock::now() < deadline);
    ASSERT_EQ(result.state, PollState::kEvent);
    ASSERT_EQ(output.sequence, sequence);
  }
  ASSERT_EQ(publisher.disconnected_clients(), 1)
      << "idle client did not exhaust its bounded queue";
  ASSERT_TRUE(publisher.Publish(Quote(++sequence)).ok());
  ASSERT_EQ(Await(healthy, output).state, PollState::kEvent);
  ExpectQuote(output, Quote(sequence));
}

TEST(TcpTransportTest, InvalidParametersAndClosedObjectsPreserveOutput) {
  TcpPublisher publisher;
  TcpSource source;
  EXPECT_EQ(publisher.Init({}, 0).code, base::ErrorCode::kInvalidArgument);
  EXPECT_EQ(publisher.Init({}, 17).code, base::ErrorCode::kInvalidArgument);
  TcpEndpoint invalid;
  std::strcpy(invalid.address, "invalid");
  invalid.port = 1234;
  EXPECT_EQ(publisher.Init(invalid).code, base::ErrorCode::kInvalidArgument);
  EXPECT_EQ(source.Init(invalid).code, base::ErrorCode::kInvalidArgument);
  std::memset(invalid.address, '1', sizeof(invalid.address));
  EXPECT_EQ(publisher.Init(invalid).code, base::ErrorCode::kInvalidArgument);
  EXPECT_EQ(source.Init(invalid).code, base::ErrorCode::kInvalidArgument);
  EXPECT_EQ(source.Init({}).code, base::ErrorCode::kInvalidArgument);
  EXPECT_EQ(source.Init({"0.0.0.0", 1234}).code,
            base::ErrorCode::kInvalidArgument);
  EXPECT_EQ(publisher.Publish(Quote()).code, base::ErrorCode::kInvalidState);
  EXPECT_EQ(publisher.Service(0).code, base::ErrorCode::kInvalidState);
  auto output = Quote(999);
  EXPECT_EQ(source.Poll(kReceiveTime, output).status.code,
            base::ErrorCode::kInvalidState);
  EXPECT_EQ(output, Quote(999));
  ASSERT_TRUE(publisher.Init({}).ok());
  EXPECT_EQ(publisher.Init({}).code, base::ErrorCode::kInvalidState);
  ASSERT_TRUE(source.Init({"127.0.0.1", publisher.port()}).ok());
  EXPECT_EQ(source.Init({"127.0.0.1", publisher.port()}).code,
            base::ErrorCode::kInvalidState);
  source.Close();
  source.Close();
  publisher.Close();
  publisher.Close();
  EXPECT_EQ(publisher.port(), 0);
}

TEST(TcpTransportTest, ConnectionRefusalReturnsTerminalFailure) {
  TcpPublisher reservation;
  ASSERT_TRUE(reservation.Init({}).ok());
  const TcpEndpoint endpoint{"127.0.0.1", reservation.port()};
  reservation.Close();
  TcpSource source;
  auto output = Quote(999);
  const auto initialized = source.Init(endpoint);
  if (initialized.ok()) {
    const auto result = Await(source, output);
    ASSERT_EQ(result.state, PollState::kError);
    EXPECT_EQ(result.status.code, base::ErrorCode::kIoError);
    EXPECT_EQ(source.Poll(kReceiveTime, output).state, PollState::kError);
  } else {
    EXPECT_EQ(initialized.code, base::ErrorCode::kIoError);
  }
  EXPECT_EQ(output, Quote(999));
}

}  // namespace
}  // namespace yquant::market

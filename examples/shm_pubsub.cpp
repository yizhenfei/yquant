#include <yquant/md/shm_pubsub.hpp>

#include <charconv>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string_view>
#include <thread>

using namespace std::chrono_literals;
using namespace yquant::md;

namespace {

bool positive_number(std::string_view text, std::uint64_t& value) {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size() && value > 0;
}

int publish(const Endpoint& endpoint, std::uint64_t count, std::uint64_t timeout_ms) {
    auto result = ShmPublisher::create({endpoint, 1024, sizeof(std::uint64_t)});
    if (!result) {
        std::cerr << result.error().message << '\n';
        return 1;
    }
    auto publisher = std::move(*result);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (std::uint64_t value = 1; value <= count; ++value) {
        if (std::chrono::steady_clock::now() >= deadline) {
            std::cerr << "publisher timed out\n";
            return 1;
        }
        EventHeader header;
        header.source_id = 1;
        header.instrument_id = 100;
        header.type = 1;
        header.has_source_sequence = true;
        header.source_sequence = value;
        const auto payload = std::as_bytes(std::span(&value, 1));
        auto sequence = publisher.publish({header, payload});
        if (!sequence) {
            std::cerr << sequence.error().message << '\n';
            return 1;
        }
        if (auto health = publisher.tick(); !health) {
            std::cerr << health.error().message << '\n';
            return 1;
        }
        std::cout << "published " << *sequence << '\n';
        std::this_thread::sleep_for(10ms);
    }
    publisher.close();
    return 0;
}

int subscribe(const Endpoint& endpoint, std::uint64_t count, std::uint64_t timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    auto result = ShmSubscriber::attach(endpoint, {}, StartPosition::at(1));
    while (!result && std::chrono::steady_clock::now() < deadline) {
        if (result.error().code != ErrorCode::NotFound && result.error().code != ErrorCode::NotReady) {
            std::cerr << result.error().message << '\n';
            return 1;
        }
        std::this_thread::sleep_for(1ms);
        result = ShmSubscriber::attach(endpoint, {}, StartPosition::at(1));
    }
    if (!result) {
        std::cerr << "timed out waiting for publisher\n";
        return 1;
    }
    auto subscriber = std::move(*result);
    EventBuffer buffer(subscriber.max_payload_size());
    std::uint64_t received = 0;
    while (received < count && std::chrono::steady_clock::now() < deadline) {
        const auto read = subscriber.try_next(buffer);
        if (read.status == ReadStatus::Event) {
            if (buffer.payload.size() != sizeof(std::uint64_t)) {
                std::cerr << "unexpected payload size\n";
                return 1;
            }
            std::uint64_t value;
            std::memcpy(&value, buffer.payload.data(), sizeof(value));
            std::cout << "received " << read.sequence << ": " << value << '\n';
            ++received;
        } else if (read.status == ReadStatus::Gap) {
            std::cerr << "gap at " << read.gap->expected_sequence
                      << ", recovery candidate " << read.gap->resume_candidate << '\n';
            return 1;
        } else if (read.status == ReadStatus::EndOfGeneration || read.status == ReadStatus::StreamChanged ||
                   read.status == ReadStatus::Closed || read.status == ReadStatus::Error) {
            std::cerr << (read.error ? read.error->message : "stream ended before requested count") << '\n';
            return 1;
        } else {
            if (auto refreshed = subscriber.refresh(); !refreshed) {
                std::cerr << refreshed.error().message << '\n';
                return 1;
            }
            std::this_thread::sleep_for(1ms);
        }
    }
    if (received != count) {
        std::cerr << "timed out after " << received << " events\n";
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::uint64_t count = 100;
    std::uint64_t timeout_ms = 10000;
    if (argc < 4 || argc > 6 || (argc >= 5 && !positive_number(argv[4], count)) ||
        (argc >= 6 && !positive_number(argv[5], timeout_ms)) || timeout_ms > 3'600'000 || count > 1'000'000) {
        std::cerr << "usage: yquant_shm_example pub|sub RUNTIME_DIRECTORY STREAM [COUNT=100] [TIMEOUT_MS=10000]\n"
                     "Start sub first, then pub in another terminal using the same directory and stream.\n";
        return 2;
    }
    const Endpoint endpoint{argv[2], argv[3]};
    const std::string_view mode(argv[1]);
    if (mode == "pub") return publish(endpoint, count, timeout_ms);
    if (mode == "sub") return subscribe(endpoint, count, timeout_ms);
    std::cerr << "mode must be pub or sub\n";
    return 2;
}

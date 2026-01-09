#include <array>
#include <atomic>
#include <chrono>
#include <datapod/pods/lockfree/ring_buffer.hpp>
#include <echo/echo.hpp>
#include <iomanip>
#include <thread>
#include <vector>

int main() {
    using namespace std::chrono_literals;

    echo::info("=== Testing MPMC Ring Buffer with Multiple Producers ===");

    dp::usize buffer_size = 512ULL * 1024 * 1024; // 512MB buffer
    echo::info("Creating ", buffer_size / 1024 / 1024, "MB shared memory buffer...");

    auto create_res = dp::RingBuffer<dp::MPMC, dp::u8>::create_shm("/test_mpmc_large", buffer_size);
    if (create_res.is_err()) {
        echo::error("Failed to create buffer: ", create_res.error().message.c_str());
        return 1;
    }
    auto buffer = std::move(create_res.value());

    echo::info("Buffer created successfully");
    echo::info("Capacity: ", buffer.capacity() / 1024 / 1024, "MB");

    constexpr int PRODUCER_COUNT = 4;
    const dp::usize bytes_per_producer = 64ULL * 1024 * 1024; // 64MB per producer
    const dp::usize total_bytes = bytes_per_producer * PRODUCER_COUNT;

    echo::info("Launching ", PRODUCER_COUNT, " producers with ", bytes_per_producer / 1024 / 1024, "MB each");

    std::atomic<bool> error_flag{false};
    std::atomic<dp::usize> produced{0};
    std::atomic<dp::usize> consumed{0};

    std::array<dp::usize, 256> counts{};
    counts.fill(0);

    std::vector<std::thread> producers;
    producers.reserve(PRODUCER_COUNT);

    for (int p = 0; p < PRODUCER_COUNT; ++p) {
        producers.emplace_back([&, p]() {
            dp::u8 value = static_cast<dp::u8>(0x10 * (p + 1));
            for (dp::usize i = 0; i < bytes_per_producer; ++i) {
                while (true) {
                    if (error_flag.load(std::memory_order_relaxed)) {
                        return;
                    }

                    auto res = buffer.push(value);
                    if (res.is_ok()) {
                        produced.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }

                    if (res.error().code != dp::Error::TIMEOUT) {
                        echo::error("Producer ", p, " push failed: ", res.error().message.c_str());
                        error_flag.store(true, std::memory_order_relaxed);
                        return;
                    }

                    std::this_thread::sleep_for(5us);
                }
            }
        });
    }

    std::thread consumer([&]() {
        while (!error_flag.load(std::memory_order_relaxed) && consumed.load(std::memory_order_relaxed) < total_bytes) {
            auto res = buffer.pop();
            if (res.is_ok()) {
                auto value = res.value();
                counts[static_cast<std::size_t>(value)]++;
                consumed.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            if (res.error().code != dp::Error::TIMEOUT) {
                echo::error("Consumer pop failed: ", res.error().message.c_str());
                error_flag.store(true, std::memory_order_relaxed);
                break;
            }

            std::this_thread::sleep_for(5us);
        }
    });

    for (auto &producer : producers) {
        producer.join();
    }
    consumer.join();

    echo::info("Produced bytes: ", produced.load(), " Consumed bytes: ", consumed.load());

    bool success = !error_flag.load(std::memory_order_relaxed);
    for (int p = 0; p < PRODUCER_COUNT; ++p) {
        dp::u8 value = static_cast<dp::u8>(0x10 * (p + 1));
        auto expected = bytes_per_producer;
        auto actual = counts[static_cast<std::size_t>(value)];
        if (actual != expected) {
            echo::error("Data mismatch for producer ", p, " value=0x", std::hex, static_cast<int>(value), std::dec,
                        " expected ", expected, " got ", actual);
            success = false;
        }
    }

    if (consumed.load() != total_bytes) {
        echo::error("Total consumed bytes mismatch: expected ", total_bytes, " got ", consumed.load());
        success = false;
    }

    if (!success) {
        echo::error("=== MPMC ring buffer test FAILED ===");
        return 1;
    }

    echo::info("=== MPMC ring buffer test PASSED ===");
    return 0;
}

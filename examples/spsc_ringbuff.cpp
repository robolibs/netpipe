/// Test SPSC ring buffer with large messages
/// Verify that SPSC can handle multi-GB data transfers

#include <chrono>
#include <datapod/pods/lockfree/ring_buffer.hpp>
#include <echo/echo.hpp>
#include <thread>

int main() {
    echo::info("=== Testing SPSC Ring Buffer with Large Messages ===");

    // Create 2GB buffer
    dp::usize buffer_size = 2ULL * 1024 * 1024 * 1024;
    echo::info("Creating ", buffer_size / 1024 / 1024, "MB shared memory buffer...");

    auto create_res = dp::RingBuffer<dp::SPSC, dp::u8>::create_shm("/test_spsc_large", buffer_size);
    if (create_res.is_err()) {
        echo::error("Failed to create buffer");
        return 1;
    }
    auto buffer = std::move(create_res.value());

    echo::info("Buffer created successfully");
    echo::info("Capacity: ", buffer.capacity() / 1024 / 1024, "MB");

    // Test 1: Write and read 500MB sequentially
    echo::info("");
    echo::info("Test 1: Sequential 500MB write/read...");
    dp::usize test_size = 500 * 1024 * 1024;
    auto start = std::chrono::high_resolution_clock::now();

    for (dp::usize i = 0; i < test_size; i++) {
        auto res = buffer.push(static_cast<dp::u8>(i % 256));
        if (res.is_err()) {
            echo::error("Push failed at byte ", i);
            return 1;
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    echo::info("✓ Wrote 500MB in ", duration, "ms (", (500.0 / duration * 1000), " MB/s)");

    start = std::chrono::high_resolution_clock::now();

    for (dp::usize i = 0; i < test_size; i++) {
        auto res = buffer.pop();
        if (res.is_err()) {
            echo::error("Pop failed at byte ", i);
            return 1;
        }
        if (res.value() != static_cast<dp::u8>(i % 256)) {
            echo::error("Data mismatch at byte ", i, " expected ", (i % 256), " got ", res.value());
            return 1;
        }
    }

    end = std::chrono::high_resolution_clock::now();
    duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    echo::info("✓ Read 500MB in ", duration, "ms (", (500.0 / duration * 1000), " MB/s) - VERIFIED!");

    // Test 2: Concurrent producer/consumer with 1GB
    echo::info("");
    echo::info("Test 2: Concurrent 1GB producer/consumer...");
    dp::usize concurrent_size = 1024 * 1024 * 1024;
    std::atomic<bool> error_flag{false};

    std::thread producer([&]() {
        auto start = std::chrono::high_resolution_clock::now();
        for (dp::usize i = 0; i < concurrent_size; i++) {
            while (buffer.push(static_cast<dp::u8>(i % 256)).is_err()) {
                if (error_flag)
                    return;
                std::this_thread::sleep_for(std::chrono::microseconds(1));
            }
        }
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        echo::info("✓ Producer wrote 1GB in ", duration, "ms (", (1024.0 / duration * 1000), " MB/s)");
    });

    std::thread consumer([&]() {
        auto start = std::chrono::high_resolution_clock::now();
        for (dp::usize i = 0; i < concurrent_size; i++) {
            dp::u8 val;
            while (true) {
                auto res = buffer.pop();
                if (res.is_ok()) {
                    val = res.value();
                    break;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(1));
            }
            if (val != static_cast<dp::u8>(i % 256)) {
                echo::error("Concurrent data mismatch at byte ", i, " expected ", (i % 256), " got ", val);
                error_flag = true;
                return;
            }
        }
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        echo::info("✓ Consumer read 1GB in ", duration, "ms (", (1024.0 / duration * 1000), " MB/s) - VERIFIED!");
    });

    producer.join();
    consumer.join();

    if (error_flag) {
        echo::error("Concurrent test FAILED");
        return 1;
    }

    // Test 3: Multiple large messages (simulating RPC pattern)
    echo::info("");
    echo::info("Test 3: Multiple 100MB messages (RPC pattern)...");

    for (int msg = 0; msg < 5; msg++) {
        dp::usize msg_size = 100 * 1024 * 1024;

        // Write message
        auto start = std::chrono::high_resolution_clock::now();
        for (dp::usize i = 0; i < msg_size; i++) {
            auto res = buffer.push(static_cast<dp::u8>((i + msg) % 256));
            if (res.is_err()) {
                echo::error("Message ", msg, " push failed at byte ", i);
                return 1;
            }
        }
        auto end = std::chrono::high_resolution_clock::now();
        auto write_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        // Read message
        start = std::chrono::high_resolution_clock::now();
        for (dp::usize i = 0; i < msg_size; i++) {
            auto res = buffer.pop();
            if (res.is_err()) {
                echo::error("Message ", msg, " pop failed at byte ", i);
                return 1;
            }
            if (res.value() != static_cast<dp::u8>((i + msg) % 256)) {
                echo::error("Message ", msg, " data mismatch at byte ", i);
                return 1;
            }
        }
        end = std::chrono::high_resolution_clock::now();
        auto read_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        echo::info("✓ Message ", msg + 1, "/5: write=", write_duration, "ms, read=", read_duration, "ms");
    }

    echo::info("");
    echo::info("=== ALL TESTS PASSED! ===");
    echo::info("SPSC ring buffer handles multi-GB data perfectly.");
    echo::info("The issue must be elsewhere in the SHM stream implementation.");

    return 0;
}

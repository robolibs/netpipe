#include <atomic>
#include <chrono>
#include <doctest/doctest.h>
#include <memory>
#include <netpipe/netpipe.hpp>
#include <thread>

// Helper to create test payload
static netpipe::Message create_payload(size_t size) {
    netpipe::Message msg(size);
    for (size_t i = 0; i < size; i++) {
        msg[i] = static_cast<dp::u8>(i % 256);
    }
    return msg;
}

TEST_CASE("RPC Timeouts - Handler exceeds call timeout (TCP)") {
    const size_t PAYLOAD_SIZE = 1024; // 1KB
    const dp::u32 METHOD_SLOW = 1;

    netpipe::TcpStream server_stream;
    netpipe::TcpEndpoint endpoint{"127.0.0.1", 23001};
    REQUIRE(server_stream.listen(endpoint).is_ok());

    std::atomic<bool> handler_called{false};
    std::atomic<bool> handler_completed{false};

    // Server thread with slow handler
    std::thread server_thread([&]() {
        auto accept_res = server_stream.accept();
        if (accept_res.is_ok()) {
            auto client_stream = std::move(accept_res.value());
            netpipe::Remote<netpipe::Bidirect> remote(*client_stream);

            remote.register_method(METHOD_SLOW, [&](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
                handler_called = true;
                // Sleep longer than client timeout
                std::this_thread::sleep_for(std::chrono::milliseconds(2000));
                handler_completed = true;
                return dp::result::ok(req);
            });

            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    netpipe::TcpStream client_stream;
    REQUIRE(client_stream.connect(endpoint).is_ok());
    netpipe::Remote<netpipe::Bidirect> client_remote(client_stream);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Call with short timeout
    auto payload = create_payload(PAYLOAD_SIZE);
    auto start = std::chrono::steady_clock::now();
    auto resp = client_remote.call(METHOD_SLOW, payload, 500); // 500ms timeout
    auto duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

    // Should timeout
    CHECK(resp.is_err());

    // Should timeout around 500ms (allow some margin)
    CHECK(duration >= 400);
    CHECK(duration <= 800);

    // Handler should have been called
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(handler_called);

    // Connection should still be alive - test with another request
    std::this_thread::sleep_for(std::chrono::milliseconds(2000)); // Wait for slow handler to finish

    client_stream.close();
    server_thread.join();
    server_stream.close();
}

TEST_CASE("RPC Timeouts - Multiple consecutive timeouts (TCP)") {
    const size_t PAYLOAD_SIZE = 512; // 512 bytes
    const dp::u32 METHOD_SLOW = 1;

    netpipe::TcpStream server_stream;
    netpipe::TcpEndpoint endpoint{"127.0.0.1", 23002};
    REQUIRE(server_stream.listen(endpoint).is_ok());

    std::atomic<int> handler_count{0};

    // Server thread
    std::thread server_thread([&]() {
        auto accept_res = server_stream.accept();
        if (accept_res.is_ok()) {
            auto client_stream = std::move(accept_res.value());
            netpipe::Remote<netpipe::Bidirect> remote(*client_stream);

            remote.register_method(METHOD_SLOW, [&](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
                handler_count++;
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                return dp::result::ok(req);
            });

            std::this_thread::sleep_for(std::chrono::seconds(10));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    netpipe::TcpStream client_stream;
    REQUIRE(client_stream.connect(endpoint).is_ok());
    netpipe::Remote<netpipe::Bidirect> client_remote(client_stream);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto payload = create_payload(PAYLOAD_SIZE);

    // Make 3 consecutive calls that will timeout
    int timeout_count = 0;
    for (int i = 0; i < 3; i++) {
        auto resp = client_remote.call(METHOD_SLOW, payload, 300); // 300ms timeout
        if (resp.is_err()) {
            timeout_count++;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // All 3 should timeout
    CHECK(timeout_count == 3);

    // All 3 handlers should have been called
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(handler_count >= 3);

    client_stream.close();
    server_thread.join();
    server_stream.close();
}

TEST_CASE("RPC Timeouts - Mixed timeouts and successes (TCP)") {
    const size_t PAYLOAD_SIZE = 512; // 512 bytes
    const dp::u32 METHOD_FAST = 1;
    const dp::u32 METHOD_SLOW = 2;

    netpipe::TcpStream server_stream;
    netpipe::TcpEndpoint endpoint{"127.0.0.1", 23003};
    REQUIRE(server_stream.listen(endpoint).is_ok());

    std::atomic<int> fast_count{0}, slow_count{0};

    // Server thread
    std::thread server_thread([&]() {
        auto accept_res = server_stream.accept();
        if (accept_res.is_ok()) {
            auto client_stream = std::move(accept_res.value());
            netpipe::Remote<netpipe::Bidirect> remote(*client_stream);

            remote.register_method(METHOD_FAST, [&](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
                fast_count++;
                return dp::result::ok(req);
            });

            remote.register_method(METHOD_SLOW, [&](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
                slow_count++;
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                return dp::result::ok(req);
            });

            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    netpipe::TcpStream client_stream;
    REQUIRE(client_stream.connect(endpoint).is_ok());
    netpipe::Remote<netpipe::Bidirect> client_remote(client_stream);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto payload = create_payload(PAYLOAD_SIZE);

    int success_count = 0;
    int timeout_count = 0;

    // Alternate between fast and slow methods
    for (int i = 0; i < 4; i++) {
        dp::u32 method = (i % 2 == 0) ? METHOD_FAST : METHOD_SLOW;
        auto resp = client_remote.call(method, payload, 500); // 500ms timeout

        if (resp.is_ok()) {
            success_count++;
        } else {
            timeout_count++;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Fast methods should succeed, slow should timeout
    CHECK(success_count == 2); // 2 fast calls
    CHECK(timeout_count == 2); // 2 slow calls
    CHECK(fast_count == 2);
    CHECK(slow_count >= 2);

    client_stream.close();
    server_thread.join();
    server_stream.close();
}

TEST_CASE("RPC Timeouts - Connection stays alive after timeout (TCP)") {
    const size_t PAYLOAD_SIZE = 512; // 512 bytes
    const dp::u32 METHOD_SLOW = 1;
    const dp::u32 METHOD_FAST = 2;

    netpipe::TcpStream server_stream;
    netpipe::TcpEndpoint endpoint{"127.0.0.1", 23004};
    REQUIRE(server_stream.listen(endpoint).is_ok());

    std::atomic<int> slow_count{0}, fast_count{0};

    // Server thread
    std::thread server_thread([&]() {
        auto accept_res = server_stream.accept();
        if (accept_res.is_ok()) {
            auto client_stream = std::move(accept_res.value());
            netpipe::Remote<netpipe::Bidirect> remote(*client_stream);

            remote.register_method(METHOD_SLOW, [&](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
                slow_count++;
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                return dp::result::ok(req);
            });

            remote.register_method(METHOD_FAST, [&](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
                fast_count++;
                return dp::result::ok(req);
            });

            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    netpipe::TcpStream client_stream;
    REQUIRE(client_stream.connect(endpoint).is_ok());
    netpipe::Remote<netpipe::Bidirect> client_remote(client_stream);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto payload = create_payload(PAYLOAD_SIZE);

    // First call times out
    auto resp1 = client_remote.call(METHOD_SLOW, payload, 300);
    CHECK(resp1.is_err()); // Should timeout

    // Wait for slow handler to finish
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // Second call should succeed (connection still alive)
    auto resp2 = client_remote.call(METHOD_FAST, payload, 1000);
    CHECK(resp2.is_ok()); // Should succeed

    CHECK(slow_count == 1);
    CHECK(fast_count == 1);

    client_stream.close();
    server_thread.join();
    server_stream.close();
}

TEST_CASE("RPC Timeouts - Timeout with concurrent requests (TCP)" * doctest::skip()) {
    const size_t PAYLOAD_SIZE = 512; // 512 bytes
    const dp::u32 METHOD_FAST = 1;
    const dp::u32 METHOD_SLOW = 2;

    netpipe::TcpStream server_stream;
    netpipe::TcpEndpoint endpoint{"127.0.0.1", 23005};
    REQUIRE(server_stream.listen(endpoint).is_ok());

    std::atomic<int> fast_count{0}, slow_count{0};

    // Server thread
    std::thread server_thread([&]() {
        auto accept_res = server_stream.accept();
        if (accept_res.is_ok()) {
            auto client_stream = std::move(accept_res.value());
            netpipe::Remote<netpipe::Bidirect> remote(*client_stream);

            remote.register_method(METHOD_FAST, [&](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
                fast_count++;
                return dp::result::ok(req);
            });

            remote.register_method(METHOD_SLOW, [&](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
                slow_count++;
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                return dp::result::ok(req);
            });

            std::this_thread::sleep_for(std::chrono::seconds(10));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    netpipe::TcpStream client_stream;
    REQUIRE(client_stream.connect(endpoint).is_ok());
    netpipe::Remote<netpipe::Bidirect> client_remote(client_stream);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::atomic<int> success_count{0}, timeout_count{0};

    // Launch concurrent requests - mix of fast and slow
    std::vector<std::thread> threads;
    for (int i = 0; i < 10; i++) {
        threads.emplace_back([&, i]() {
            auto payload = create_payload(PAYLOAD_SIZE);
            dp::u32 method = (i % 2 == 0) ? METHOD_FAST : METHOD_SLOW;
            auto resp = client_remote.call(method, payload, 500);

            if (resp.is_ok()) {
                success_count++;
            } else {
                timeout_count++;
            }
        });
    }

    for (auto &t : threads) {
        t.join();
    }

    // Fast methods should mostly succeed, slow should mostly timeout
    CHECK(success_count >= 4); // At least 4 out of 5 fast calls
    CHECK(timeout_count >= 4); // At least 4 out of 5 slow calls
    CHECK(fast_count >= 4);
    CHECK(slow_count >= 4);

    client_stream.close();
    server_thread.join();
    server_stream.close();
}

TEST_CASE("RPC Timeouts - Verify timeout duration accuracy (TCP)" * doctest::skip()) {
    const size_t PAYLOAD_SIZE = 256; // 256 bytes
    const dp::u32 METHOD_SLOW = 1;

    netpipe::TcpStream server_stream;
    netpipe::TcpEndpoint endpoint{"127.0.0.1", 23006};
    REQUIRE(server_stream.listen(endpoint).is_ok());

    // Server thread
    std::thread server_thread([&]() {
        auto accept_res = server_stream.accept();
        if (accept_res.is_ok()) {
            auto client_stream = std::move(accept_res.value());
            netpipe::Remote<netpipe::Bidirect> remote(*client_stream);

            remote.register_method(METHOD_SLOW, [&](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
                std::this_thread::sleep_for(std::chrono::milliseconds(5000));
                return dp::result::ok(req);
            });

            std::this_thread::sleep_for(std::chrono::seconds(10));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    netpipe::TcpStream client_stream;
    REQUIRE(client_stream.connect(endpoint).is_ok());
    netpipe::Remote<netpipe::Bidirect> client_remote(client_stream);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto payload = create_payload(PAYLOAD_SIZE);

    // Test different timeout values
    std::vector<dp::u32> timeouts = {500, 1000, 1500};

    for (auto timeout : timeouts) {
        auto start = std::chrono::steady_clock::now();
        auto resp = client_remote.call(METHOD_SLOW, payload, timeout);
        auto duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

        // Should timeout
        CHECK(resp.is_err());

        // Duration should be close to timeout value (within 30% margin)
        CHECK(duration >= timeout * 0.7);
        CHECK(duration <= timeout * 1.3);

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    client_stream.close();
    server_thread.join();
    server_stream.close();
}

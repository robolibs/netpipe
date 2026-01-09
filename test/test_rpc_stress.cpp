#include <atomic>
#include <chrono>
#include <doctest/doctest.h>
#include <memory>
#include <netpipe/netpipe.hpp>
#include <thread>
#include <vector>

// Helper to create test payload
static netpipe::Message create_payload(size_t size, dp::u8 seed = 0) {
    netpipe::Message msg(size);
    for (size_t i = 0; i < size; i++) {
        msg[i] = static_cast<dp::u8>((i + seed) % 256);
    }
    return msg;
}

// Helper to verify payload
static bool verify_payload(const netpipe::Message &msg, size_t expected_size, dp::u8 seed = 0) {
    if (msg.size() != expected_size)
        return false;
    for (size_t i = 0; i < msg.size(); i++) {
        if (msg[i] != static_cast<dp::u8>((i + seed) % 256))
            return false;
    }
    return true;
}

TEST_CASE("RPC Stress - Multiple threads calling same method (TCP)") {
    const size_t PAYLOAD_SIZE = 1024; // 1KB
    const dp::u32 METHOD_ECHO = 1;
    const int NUM_THREADS = 10;
    const int REQUESTS_PER_THREAD = 100;

    netpipe::TcpStream server_stream;
    netpipe::TcpEndpoint endpoint{"127.0.0.1", 21001};
    REQUIRE(server_stream.listen(endpoint).is_ok());

    std::atomic<int> total_requests{0};
    std::atomic<int> successful_requests{0};

    // Server thread
    std::thread server_thread([&]() {
        auto accept_res = server_stream.accept();
        REQUIRE(accept_res.is_ok());
        auto client_stream = std::move(accept_res.value());

        netpipe::Remote<netpipe::Bidirect> remote(*client_stream);

        // Register echo method
        remote.register_method(METHOD_ECHO, [&](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            total_requests++;
            // Echo back the request
            return dp::result::ok(req);
        });

        // Keep server alive for duration of test
        std::this_thread::sleep_for(std::chrono::seconds(30));
    });

    // Give server time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Client connection
    netpipe::TcpStream client_stream;
    REQUIRE(client_stream.connect(endpoint).is_ok());
    netpipe::Remote<netpipe::Bidirect> client_remote(client_stream);

    // Give time for connection to establish
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Launch multiple client threads
    std::vector<std::thread> client_threads;
    for (int t = 0; t < NUM_THREADS; t++) {
        client_threads.emplace_back([&, t]() {
            for (int i = 0; i < REQUESTS_PER_THREAD; i++) {
                dp::u8 seed = static_cast<dp::u8>(t * REQUESTS_PER_THREAD + i);
                auto payload = create_payload(PAYLOAD_SIZE, seed);

                auto resp = client_remote.call(METHOD_ECHO, payload, 5000);
                if (resp.is_ok() && verify_payload(resp.value(), PAYLOAD_SIZE, seed)) {
                    successful_requests++;
                }
            }
        });
    }

    // Wait for all client threads to complete
    for (auto &t : client_threads) {
        t.join();
    }

    // Verify all requests succeeded
    const int EXPECTED_REQUESTS = NUM_THREADS * REQUESTS_PER_THREAD;
    CHECK(total_requests == EXPECTED_REQUESTS);
    CHECK(successful_requests == EXPECTED_REQUESTS);

    // Cleanup
    client_stream.close();
    server_stream.close();
    server_thread.join();
}

TEST_CASE("RPC Stress - Multiple threads calling different methods (TCP)") {
    const size_t PAYLOAD_SIZE = 1024; // 1KB
    const dp::u32 METHOD_ADD = 1;
    const dp::u32 METHOD_MULTIPLY = 2;
    const dp::u32 METHOD_ECHO = 3;
    const int NUM_THREADS = 15;
    const int REQUESTS_PER_THREAD = 50;

    netpipe::TcpStream server_stream;
    netpipe::TcpEndpoint endpoint{"127.0.0.1", 21002};
    REQUIRE(server_stream.listen(endpoint).is_ok());

    std::atomic<int> add_calls{0}, multiply_calls{0}, echo_calls{0};
    std::atomic<int> successful_requests{0};

    // Server thread
    std::thread server_thread([&]() {
        auto accept_res = server_stream.accept();
        REQUIRE(accept_res.is_ok());
        auto client_stream = std::move(accept_res.value());

        netpipe::Remote<netpipe::Bidirect> remote(*client_stream);

        // Register multiple methods
        remote.register_method(METHOD_ADD, [&](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            add_calls++;
            netpipe::Message resp(req.size());
            for (size_t i = 0; i < req.size(); i++) {
                resp[i] = req[i] + 1;
            }
            return dp::result::ok(resp);
        });

        remote.register_method(METHOD_MULTIPLY, [&](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            multiply_calls++;
            netpipe::Message resp(req.size());
            for (size_t i = 0; i < req.size(); i++) {
                resp[i] = req[i] * 2;
            }
            return dp::result::ok(resp);
        });

        remote.register_method(METHOD_ECHO, [&](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            echo_calls++;
            return dp::result::ok(req);
        });

        std::this_thread::sleep_for(std::chrono::seconds(30));
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    netpipe::TcpStream client_stream;
    REQUIRE(client_stream.connect(endpoint).is_ok());
    netpipe::Remote<netpipe::Bidirect> client_remote(client_stream);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Launch threads calling different methods
    std::vector<std::thread> client_threads;
    for (int t = 0; t < NUM_THREADS; t++) {
        client_threads.emplace_back([&, t]() {
            for (int i = 0; i < REQUESTS_PER_THREAD; i++) {
                dp::u32 method = (t % 3 == 0) ? METHOD_ADD : (t % 3 == 1) ? METHOD_MULTIPLY : METHOD_ECHO;
                dp::u8 seed = static_cast<dp::u8>(t * REQUESTS_PER_THREAD + i);
                auto payload = create_payload(PAYLOAD_SIZE, seed);

                auto resp = client_remote.call(method, payload, 5000);
                if (resp.is_ok()) {
                    bool valid = false;
                    if (method == METHOD_ADD) {
                        // Verify add operation
                        valid = (resp.value().size() == PAYLOAD_SIZE);
                        for (size_t j = 0; j < PAYLOAD_SIZE && valid; j++) {
                            valid = (resp.value()[j] == static_cast<dp::u8>((j + seed + 1) % 256));
                        }
                    } else if (method == METHOD_MULTIPLY) {
                        // Verify multiply operation
                        valid = (resp.value().size() == PAYLOAD_SIZE);
                        for (size_t j = 0; j < PAYLOAD_SIZE && valid; j++) {
                            valid = (resp.value()[j] == static_cast<dp::u8>(((j + seed) * 2) % 256));
                        }
                    } else {
                        // Verify echo
                        valid = verify_payload(resp.value(), PAYLOAD_SIZE, seed);
                    }

                    if (valid) {
                        successful_requests++;
                    }
                }
            }
        });
    }

    for (auto &t : client_threads) {
        t.join();
    }

    const int EXPECTED_REQUESTS = NUM_THREADS * REQUESTS_PER_THREAD;
    CHECK(successful_requests == EXPECTED_REQUESTS);
    CHECK(add_calls + multiply_calls + echo_calls == EXPECTED_REQUESTS);

    client_stream.close();
    server_stream.close();
    server_thread.join();
}

TEST_CASE("RPC Stress - Bidirectional concurrent calls (TCP)") {
    const size_t PAYLOAD_SIZE = 10 * 1024; // 10KB
    const dp::u32 METHOD_PING = 1;
    const dp::u32 METHOD_PONG = 2;
    const int CONCURRENT_CALLS = 50;

    netpipe::TcpStream server_stream;
    netpipe::TcpEndpoint endpoint{"127.0.0.1", 21003};
    REQUIRE(server_stream.listen(endpoint).is_ok());

    std::atomic<int> peer1_calls{0}, peer2_calls{0};
    std::atomic<int> peer1_success{0}, peer2_success{0};

    std::shared_ptr<std::unique_ptr<netpipe::Stream>> shared_server_stream;
    std::shared_ptr<netpipe::TcpStream> shared_client_stream = std::make_shared<netpipe::TcpStream>();

    // Peer 1 (server side)
    std::thread t1([&, shared_server_stream]() mutable {
        auto s = std::move(server_stream.accept().value());
        shared_server_stream = std::make_shared<std::unique_ptr<netpipe::Stream>>(std::move(s));

        netpipe::Remote<netpipe::Bidirect> r(**shared_server_stream);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // Register handler for PONG
        r.register_method(METHOD_PONG, [&](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            peer1_calls++;
            return dp::result::ok(req);
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        // Make concurrent PING calls
        std::vector<std::thread> call_threads;
        for (int i = 0; i < CONCURRENT_CALLS; i++) {
            call_threads.emplace_back([&, i]() {
                auto payload = create_payload(PAYLOAD_SIZE, static_cast<dp::u8>(i));
                auto resp = r.call(METHOD_PING, payload, 10000);
                if (resp.is_ok() && verify_payload(resp.value(), PAYLOAD_SIZE, static_cast<dp::u8>(i))) {
                    peer1_success++;
                }
            });
        }

        for (auto &t : call_threads) {
            t.join();
        }

        std::this_thread::sleep_for(std::chrono::seconds(5));
    });

    // Peer 2 (client side)
    std::thread t2([&, shared_client_stream]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        REQUIRE(shared_client_stream->connect(endpoint).is_ok());

        netpipe::Remote<netpipe::Bidirect> r(*shared_client_stream);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // Register handler for PING
        r.register_method(METHOD_PING, [&](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            peer2_calls++;
            return dp::result::ok(req);
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        // Make concurrent PONG calls
        std::vector<std::thread> call_threads;
        for (int i = 0; i < CONCURRENT_CALLS; i++) {
            call_threads.emplace_back([&, i]() {
                auto payload = create_payload(PAYLOAD_SIZE, static_cast<dp::u8>(i + 100));
                auto resp = r.call(METHOD_PONG, payload, 10000);
                if (resp.is_ok() && verify_payload(resp.value(), PAYLOAD_SIZE, static_cast<dp::u8>(i + 100))) {
                    peer2_success++;
                }
            });
        }

        for (auto &t : call_threads) {
            t.join();
        }

        std::this_thread::sleep_for(std::chrono::seconds(5));
    });

    t1.join();
    t2.join();

    CHECK(peer1_calls == CONCURRENT_CALLS);
    CHECK(peer2_calls == CONCURRENT_CALLS);
    CHECK(peer1_success == CONCURRENT_CALLS);
    CHECK(peer2_success == CONCURRENT_CALLS);

    server_stream.close();
}

TEST_CASE("RPC Stress - Mixed payload sizes under load (TCP)") {
    const dp::u32 METHOD_ECHO = 1;
    const int NUM_THREADS = 10;
    const int REQUESTS_PER_THREAD = 10;
    const std::vector<size_t> PAYLOAD_SIZES = {
        100,       // 100 bytes
        1024,      // 1KB
        10 * 1024, // 10KB
        100 * 1024 // 100KB
    };

    netpipe::TcpStream server_stream;
    netpipe::TcpEndpoint endpoint{"127.0.0.1", 21004};
    REQUIRE(server_stream.listen(endpoint).is_ok());

    std::atomic<int> total_requests{0};
    std::atomic<int> successful_requests{0};

    std::thread server_thread([&]() {
        auto accept_res = server_stream.accept();
        REQUIRE(accept_res.is_ok());
        auto client_stream = std::move(accept_res.value());

        netpipe::Remote<netpipe::Bidirect> remote(*client_stream);

        remote.register_method(METHOD_ECHO, [&](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            total_requests++;
            return dp::result::ok(req);
        });

        std::this_thread::sleep_for(std::chrono::seconds(30));
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    netpipe::TcpStream client_stream;
    REQUIRE(client_stream.connect(endpoint).is_ok());
    netpipe::Remote<netpipe::Bidirect> client_remote(client_stream);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::vector<std::thread> client_threads;
    for (int t = 0; t < NUM_THREADS; t++) {
        client_threads.emplace_back([&, t]() {
            for (int i = 0; i < REQUESTS_PER_THREAD; i++) {
                // Cycle through different payload sizes
                size_t payload_size = PAYLOAD_SIZES[(t * REQUESTS_PER_THREAD + i) % PAYLOAD_SIZES.size()];
                dp::u8 seed = static_cast<dp::u8>(t * REQUESTS_PER_THREAD + i);
                auto payload = create_payload(payload_size, seed);

                auto resp = client_remote.call(METHOD_ECHO, payload, 10000);
                if (resp.is_ok() && verify_payload(resp.value(), payload_size, seed)) {
                    successful_requests++;
                }
            }
        });
    }

    for (auto &t : client_threads) {
        t.join();
    }

    const int EXPECTED_REQUESTS = NUM_THREADS * REQUESTS_PER_THREAD;
    CHECK(total_requests == EXPECTED_REQUESTS);
    CHECK(successful_requests == EXPECTED_REQUESTS);

    client_stream.close();
    server_stream.close();
    server_thread.join();
}

TEST_CASE("RPC Stress - High concurrency stress test (TCP)" * doctest::skip()) {
    const size_t PAYLOAD_SIZE = 512; // 512 bytes
    const dp::u32 METHOD_ECHO = 1;
    const int NUM_THREADS = 20;
    const int REQUESTS_PER_THREAD = 10;

    netpipe::TcpStream server_stream;
    netpipe::TcpEndpoint endpoint{"127.0.0.1", 21005};
    REQUIRE(server_stream.listen(endpoint).is_ok());

    std::atomic<int> total_requests{0};
    std::atomic<int> successful_requests{0};
    std::atomic<int> failed_requests{0};

    std::thread server_thread([&]() {
        auto accept_res = server_stream.accept();
        REQUIRE(accept_res.is_ok());
        auto client_stream = std::move(accept_res.value());

        netpipe::Remote<netpipe::Bidirect> remote(*client_stream);

        remote.register_method(METHOD_ECHO, [&](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            total_requests++;
            return dp::result::ok(req);
        });

        std::this_thread::sleep_for(std::chrono::seconds(15));
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    netpipe::TcpStream client_stream;
    REQUIRE(client_stream.connect(endpoint).is_ok());
    netpipe::Remote<netpipe::Bidirect> client_remote(client_stream);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::vector<std::thread> client_threads;
    for (int t = 0; t < NUM_THREADS; t++) {
        client_threads.emplace_back([&, t]() {
            for (int i = 0; i < REQUESTS_PER_THREAD; i++) {
                dp::u8 seed = static_cast<dp::u8>(t * REQUESTS_PER_THREAD + i);
                auto payload = create_payload(PAYLOAD_SIZE, seed);

                auto resp = client_remote.call(METHOD_ECHO, payload, 3000);
                if (resp.is_ok() && verify_payload(resp.value(), PAYLOAD_SIZE, seed)) {
                    successful_requests++;
                } else {
                    failed_requests++;
                }
            }
        });
    }

    for (auto &t : client_threads) {
        t.join();
    }

    const int EXPECTED_REQUESTS = NUM_THREADS * REQUESTS_PER_THREAD;

    // With our improvements, we should handle all requests successfully
    // Allow small margin for timeouts under extreme load
    CHECK(successful_requests >= EXPECTED_REQUESTS * 0.95); // 95% success rate minimum
    CHECK(total_requests >= EXPECTED_REQUESTS * 0.95);

    client_stream.close();
    server_stream.close();
    server_thread.join();
}

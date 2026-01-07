#include <atomic>
#include <chrono>
#include <doctest/doctest.h>
#include <netpipe/netpipe.hpp>
#include <thread>

TEST_CASE("RemoteMetrics - Basic metrics tracking") {
    SUBCASE("RemoteAsync with metrics enabled") {
        netpipe::TcpStream server;
        netpipe::TcpEndpoint endpoint{"127.0.0.1", 19000};

        auto listen_res = server.listen(endpoint);
        REQUIRE(listen_res.is_ok());

        std::atomic<int> requests_handled{0};

        // Server thread - use RemotePeer for easier lifecycle management
        std::thread server_thread([&]() {
            auto accept_res = server.accept();
            if (accept_res.is_ok()) {
                auto stream = std::move(accept_res.value());
                netpipe::RemotePeer peer(*stream);

                // Register echo method
                peer.register_method(1, [&](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
                    requests_handled++;
                    return dp::result::ok(req);
                });

                // Register slow method
                peer.register_method(2, [&](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
                    requests_handled++;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    return dp::result::ok(req);
                });

                // Register error method
                peer.register_method(3, [&](const netpipe::Message &) -> dp::Res<netpipe::Message> {
                    requests_handled++;
                    return dp::result::err(dp::Error::io_error("test error"));
                });

                // Wait for all requests to be handled
                while (requests_handled.load() < 4) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // Client with metrics enabled
        netpipe::TcpStream client;
        auto connect_res = client.connect(endpoint);
        REQUIRE(connect_res.is_ok());

        netpipe::RemoteAsync remote(client, 100, true); // Enable metrics

        // Make successful calls
        netpipe::Message req1 = {1, 2, 3, 4, 5};
        auto resp1 = remote.call(1, req1, 1000);
        REQUIRE(resp1.is_ok());
        CHECK(resp1.value() == req1);

        netpipe::Message req2 = {10, 20, 30};
        auto resp2 = remote.call(1, req2, 1000);
        REQUIRE(resp2.is_ok());
        CHECK(resp2.value() == req2);

        // Make slow call
        netpipe::Message req3 = {100};
        auto resp3 = remote.call(2, req3, 1000);
        REQUIRE(resp3.is_ok());

        // Make error call
        netpipe::Message req4 = {200};
        auto resp4 = remote.call(3, req4, 1000);
        REQUIRE(resp4.is_err());

        // Check metrics
        const auto &metrics = remote.get_metrics();

        CHECK(metrics.total_requests.load() == 4);
        CHECK(metrics.successful_requests.load() == 3);
        CHECK(metrics.failed_requests.load() == 1);
        CHECK(metrics.in_flight_requests.load() == 0);
        CHECK(metrics.peak_in_flight_requests.load() >= 1);

        // Check latency
        CHECK(metrics.avg_latency_us() > 0);
        CHECK(metrics.min_latency_us.load() > 0);
        CHECK(metrics.max_latency_us.load() >= metrics.min_latency_us.load());

        // Check success rate
        CHECK(metrics.success_rate() > 0.7); // 3/4 = 0.75
        CHECK(metrics.failure_rate() > 0.2); // 1/4 = 0.25

        // Check message sizes
        CHECK(metrics.total_request_bytes.load() > 0);
        CHECK(metrics.total_response_bytes.load() > 0);
        CHECK(metrics.avg_request_bytes() > 0);
        CHECK(metrics.avg_response_bytes() > 0);

        server_thread.join();
        client.close();
        server.close();
    }

    SUBCASE("RemotePeer with metrics - bidirectional") {
        netpipe::TcpStream server;
        netpipe::TcpEndpoint endpoint{"127.0.0.1", 19001};

        auto listen_res = server.listen(endpoint);
        REQUIRE(listen_res.is_ok());

        std::atomic<int> peer1_calls{0};
        std::atomic<int> peer2_calls{0};

        // Peer 1
        std::thread peer1_thread([&]() {
            auto accept_res = server.accept();
            if (accept_res.is_ok()) {
                auto stream = std::move(accept_res.value());
                netpipe::RemotePeer peer(*stream, 100, true); // Enable metrics

                // Register method
                peer.register_method(1, [&](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
                    peer1_calls++;
                    return dp::result::ok(req);
                });

                // Wait for peer2 to call us
                while (peer1_calls.load() == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }

                // Call peer2
                netpipe::Message req = {0x02};
                auto resp = peer.call(2, req, 1000);
                CHECK(resp.is_ok());

                // Wait a bit for metrics to settle
                std::this_thread::sleep_for(std::chrono::milliseconds(100));

                // Check server metrics (incoming requests)
                const auto &server_metrics = peer.get_server_metrics();
                CHECK(server_metrics.total_requests.load() == 1);
                CHECK(server_metrics.successful_requests.load() == 1);
                CHECK(server_metrics.handler_invocations.load() == 1);
                CHECK(server_metrics.avg_handler_time_us() > 0);

                // Check client metrics (outgoing calls)
                const auto &client_metrics = peer.get_client_metrics();
                CHECK(client_metrics.total_requests.load() == 1);
                CHECK(client_metrics.successful_requests.load() == 1);

                // Wait for peer2 to finish
                while (peer2_calls.load() == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
        });

        // Peer 2
        std::thread peer2_thread([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            netpipe::TcpStream stream;
            auto connect_res = stream.connect(endpoint);
            if (connect_res.is_ok()) {
                netpipe::RemotePeer peer(stream, 100, true); // Enable metrics

                // Register method
                peer.register_method(2, [&](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
                    peer2_calls++;
                    return dp::result::ok(req);
                });

                // Call peer1
                netpipe::Message req = {0x01};
                auto resp = peer.call(1, req, 1000);
                CHECK(resp.is_ok());

                // Wait for peer1 to call us back
                while (peer2_calls.load() == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }

                // Check metrics
                const auto &server_metrics = peer.get_server_metrics();
                CHECK(server_metrics.total_requests.load() == 1);
                CHECK(server_metrics.successful_requests.load() == 1);

                const auto &client_metrics = peer.get_client_metrics();
                CHECK(client_metrics.total_requests.load() == 1);
                CHECK(client_metrics.successful_requests.load() == 1);

                // Wait for peer1 to finish checking metrics
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });

        peer1_thread.join();
        peer2_thread.join();
        server.close();

        CHECK(peer1_calls.load() == 1);
        CHECK(peer2_calls.load() == 1);
    }

    SUBCASE("Metrics reset") {
        netpipe::RemoteMetrics metrics;

        // Set some values
        metrics.total_requests = 10;
        metrics.successful_requests = 8;
        metrics.failed_requests = 2;
        metrics.total_latency_us = 1000;

        CHECK(metrics.total_requests.load() == 10);
        CHECK(metrics.avg_latency_us() == 100);

        // Reset
        metrics.reset();

        CHECK(metrics.total_requests.load() == 0);
        CHECK(metrics.successful_requests.load() == 0);
        CHECK(metrics.failed_requests.load() == 0);
        CHECK(metrics.total_latency_us.load() == 0);
        CHECK(metrics.avg_latency_us() == 0);
    }

    SUBCASE("Metrics calculations") {
        netpipe::RemoteMetrics metrics;

        // Test with no data
        CHECK(metrics.avg_latency_us() == 0);
        CHECK(metrics.success_rate() == 0.0);
        CHECK(metrics.failure_rate() == 0.0);

        // Add some data
        metrics.total_requests = 100;
        metrics.successful_requests = 80;
        metrics.failed_requests = 15;
        metrics.timeout_requests = 5;
        metrics.total_latency_us = 500000; // 500ms total

        CHECK(metrics.avg_latency_us() == 5000); // 5ms average
        CHECK(metrics.success_rate() == 0.8);
        CHECK(metrics.failure_rate() == 0.15);
        CHECK(metrics.timeout_rate() == 0.05);

        // Test message sizes
        metrics.total_request_bytes = 10000;
        metrics.total_response_bytes = 8000;

        CHECK(metrics.avg_request_bytes() == 100);
        CHECK(metrics.avg_response_bytes() == 100); // 8000 / 80 successful
    }
}

#include <chrono>
#include <doctest/doctest.h>
#include <netpipe/netpipe.hpp>
#include <thread>

// This test file validates bidirectional RPC communication over shared memory transport.
//
// ShmStream uses TWO SPSC ring buffers for bidirectional communication:
// - Server writes to s2c buffer, reads from c2s buffer
// - Client writes to c2s buffer, reads from s2c buffer
//
// This allows Remote<Bidirect> to work properly - each side can both send requests
// and handle incoming requests simultaneously without deadlock.

TEST_CASE("ShmStream + Remote<Bidirect> - 1MB payload") {
    netpipe::ShmStream server_stream;
    netpipe::ShmEndpoint endpoint{"shm_bidirect_1mb", 1536 * 1024 * 1024}; // 1.5GB buffer

    auto listen_res = server_stream.listen_shm(endpoint);
    REQUIRE(listen_res.is_ok());

    std::atomic<int> finished_count{0};

    // Server thread: registers handler and makes calls to client
    std::thread server_thread([&]() {
        netpipe::Remote<netpipe::Bidirect> server_remote(server_stream);
        // Wait for receiver thread to start
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Server registers method 1: echo handler
        server_remote.register_method(1, [](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            echo::debug("server handling echo request of size ", req.size());
            // Echo back the same message
            return dp::result::ok(req);
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        // Server calls client's method 2 with 1MB payload
        netpipe::Message server_request(1024 * 1024, 0xAA); // 1MB of 0xAA
        auto server_resp = server_remote.call(2, server_request, 5000);
        REQUIRE(server_resp.is_ok());
        CHECK(server_resp.value().size() == 1024 * 1024);
        CHECK(server_resp.value()[0] == 0xAA);
        CHECK(server_resp.value()[1024 * 1024 - 1] == 0xAA);
        echo::info("server received 1MB response from client");

        // Signal we're done
        finished_count++;

        // Wait for BOTH peers to finish
        while (finished_count < 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // Now both are done, safe to exit and destroy streams
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    });

    // Client thread: registers handler and makes calls to server
    std::thread client_thread([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        netpipe::ShmStream client_stream;
        auto connect_res = client_stream.connect_shm(endpoint);
        REQUIRE(connect_res.is_ok());

        netpipe::Remote<netpipe::Bidirect> client_remote(client_stream);
        // Wait for receiver thread to start
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Client registers method 2: echo handler
        client_remote.register_method(2, [](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            echo::debug("client handling echo request of size ", req.size());
            // Echo back the same message
            return dp::result::ok(req);
        });

        // Client calls server's method 1 with 1MB payload
        netpipe::Message client_request(1024 * 1024, 0xBB); // 1MB of 0xBB
        auto client_resp = client_remote.call(1, client_request, 5000);
        REQUIRE(client_resp.is_ok());
        CHECK(client_resp.value().size() == 1024 * 1024);
        CHECK(client_resp.value()[0] == 0xBB);
        CHECK(client_resp.value()[1024 * 1024 - 1] == 0xBB);
        echo::info("client received 1MB response from server");

        // Keep client_remote alive until server thread completes
        std::this_thread::sleep_for(std::chrono::milliseconds(700));

        // Signal we're done
        finished_count++;

        // Wait for BOTH peers to finish
        while (finished_count < 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // Now both are done, safe to exit and destroy streams
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    });

    server_thread.join();
    client_thread.join();
    server_stream.close();
}

TEST_CASE("ShmStream + Remote<Bidirect> - 10MB payload") {
    netpipe::ShmStream server_stream;
    netpipe::ShmEndpoint endpoint{"shm_bidirect_10mb", 1536 * 1024 * 1024}; // 1.5GB buffer

    auto listen_res = server_stream.listen_shm(endpoint);
    REQUIRE(listen_res.is_ok());

    std::atomic<int> finished_count{0};

    // Server thread: registers handler and makes calls to client
    std::thread server_thread([&]() {
        netpipe::Remote<netpipe::Bidirect> server_remote(server_stream);
        // Wait for receiver thread to start
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Server registers method 1: echo handler
        server_remote.register_method(1, [](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            echo::debug("server handling echo request of size ", req.size());
            // Echo back the same message
            return dp::result::ok(req);
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        // Server calls client's method 2 with 10MB payload
        netpipe::Message server_request(10 * 1024 * 1024, 0xCC); // 10MB of 0xCC
        auto server_resp = server_remote.call(2, server_request, 10000);
        REQUIRE(server_resp.is_ok());
        CHECK(server_resp.value().size() == 10 * 1024 * 1024);
        CHECK(server_resp.value()[0] == 0xCC);
        CHECK(server_resp.value()[10 * 1024 * 1024 - 1] == 0xCC);
        echo::info("server received 10MB response from client");

        // Signal we're done
        finished_count++;

        // Wait for BOTH peers to finish
        while (finished_count < 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // Now both are done, safe to exit and destroy streams
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    });

    // Client thread: registers handler and makes calls to server
    std::thread client_thread([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        netpipe::ShmStream client_stream;
        auto connect_res = client_stream.connect_shm(endpoint);
        REQUIRE(connect_res.is_ok());

        netpipe::Remote<netpipe::Bidirect> client_remote(client_stream);
        // Wait for receiver thread to start
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Client registers method 2: echo handler
        client_remote.register_method(2, [](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            echo::debug("client handling echo request of size ", req.size());
            // Echo back the same message
            return dp::result::ok(req);
        });

        // Client calls server's method 1 with 10MB payload
        netpipe::Message client_request(10 * 1024 * 1024, 0xDD); // 10MB of 0xDD
        auto client_resp = client_remote.call(1, client_request, 10000);
        REQUIRE(client_resp.is_ok());
        CHECK(client_resp.value().size() == 10 * 1024 * 1024);
        CHECK(client_resp.value()[0] == 0xDD);
        CHECK(client_resp.value()[10 * 1024 * 1024 - 1] == 0xDD);
        echo::info("client received 10MB response from server");

        // Keep client_remote alive until server thread completes
        std::this_thread::sleep_for(std::chrono::milliseconds(700));

        // Signal we're done
        finished_count++;

        // Wait for BOTH peers to finish
        while (finished_count < 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // Now both are done, safe to exit and destroy streams
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    });

    server_thread.join();
    client_thread.join();
    server_stream.close();
}

TEST_CASE("ShmStream + Remote<Bidirect> - 100MB payload") {
    netpipe::ShmStream server_stream;
    netpipe::ShmEndpoint endpoint{"shm_bidirect_100mb", 1536 * 1024 * 1024}; // 1.5GB buffer

    auto listen_res = server_stream.listen_shm(endpoint);
    REQUIRE(listen_res.is_ok());

    std::atomic<int> finished_count{0};

    // Server thread: registers handler and makes calls to client
    std::thread server_thread([&]() {
        netpipe::Remote<netpipe::Bidirect> server_remote(server_stream);
        // Wait for receiver thread to start
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Server registers method 1: echo handler
        server_remote.register_method(1, [](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            echo::debug("server handling echo request of size ", req.size());
            // Echo back the same message
            return dp::result::ok(req);
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        // Server calls client's method 2 with 100MB payload
        netpipe::Message server_request(100 * 1024 * 1024, 0xEE); // 100MB of 0xEE
        auto server_resp = server_remote.call(2, server_request, 30000);
        REQUIRE(server_resp.is_ok());
        CHECK(server_resp.value().size() == 100 * 1024 * 1024);
        CHECK(server_resp.value()[0] == 0xEE);
        CHECK(server_resp.value()[100 * 1024 * 1024 - 1] == 0xEE);
        echo::info("server received 100MB response from client");

        // Signal we're done
        finished_count++;

        // Wait for BOTH peers to finish
        while (finished_count < 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // Now both are done, safe to exit and destroy streams
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    });

    // Client thread: registers handler and makes calls to server
    std::thread client_thread([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        netpipe::ShmStream client_stream;
        auto connect_res = client_stream.connect_shm(endpoint);
        REQUIRE(connect_res.is_ok());

        netpipe::Remote<netpipe::Bidirect> client_remote(client_stream);
        // Wait for receiver thread to start
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Client registers method 2: echo handler
        client_remote.register_method(2, [](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            echo::debug("client handling echo request of size ", req.size());
            // Echo back the same message
            return dp::result::ok(req);
        });

        // Client calls server's method 1 with 100MB payload
        netpipe::Message client_request(100 * 1024 * 1024, 0xFF); // 100MB of 0xFF
        auto client_resp = client_remote.call(1, client_request, 30000);
        REQUIRE(client_resp.is_ok());
        CHECK(client_resp.value().size() == 100 * 1024 * 1024);
        CHECK(client_resp.value()[0] == 0xFF);
        CHECK(client_resp.value()[100 * 1024 * 1024 - 1] == 0xFF);
        echo::info("client received 100MB response from server");

        // Keep client_remote alive until server thread completes
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));

        // Signal we're done
        finished_count++;

        // Wait for BOTH peers to finish
        while (finished_count < 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // Now both are done, safe to exit and destroy streams
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    });

    server_thread.join();
    client_thread.join();
    server_stream.close();
}

TEST_CASE("ShmStream + Remote<Bidirect> - 1GB payload" * doctest::skip()) {
    netpipe::ShmStream server_stream;
    netpipe::ShmEndpoint endpoint{"shm_bidirect_1gb", 1536 * 1024 * 1024}; // 1.5GB buffer

    auto listen_res = server_stream.listen_shm(endpoint);
    REQUIRE(listen_res.is_ok());

    std::atomic<int> finished_count{0};

    // Server thread: registers handler and makes calls to client
    std::thread server_thread([&]() {
        netpipe::Remote<netpipe::Bidirect> server_remote(server_stream);
        // Wait for receiver thread to start
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Server registers method 1: echo handler
        server_remote.register_method(1, [](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            echo::debug("server handling echo request of size ", req.size());
            // Echo back the same message
            return dp::result::ok(req);
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        // Server calls client's method 2 with 1GB payload
        netpipe::Message server_request(1024 * 1024 * 1024, 0xAA); // 1GB of 0xAA
        auto server_resp = server_remote.call(2, server_request, 60000);
        REQUIRE(server_resp.is_ok());
        CHECK(server_resp.value().size() == 1024 * 1024 * 1024);
        CHECK(server_resp.value()[0] == 0xAA);
        CHECK(server_resp.value()[1024 * 1024 * 1024 - 1] == 0xAA);
        echo::info("server received 1GB response from client");

        // Signal we're done
        finished_count++;

        // Wait for BOTH peers to finish
        while (finished_count < 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // Now both are done, safe to exit and destroy streams
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    });

    // Client thread: registers handler and makes calls to server
    std::thread client_thread([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        netpipe::ShmStream client_stream;
        auto connect_res = client_stream.connect_shm(endpoint);
        REQUIRE(connect_res.is_ok());

        netpipe::Remote<netpipe::Bidirect> client_remote(client_stream);
        // Wait for receiver thread to start
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Client registers method 2: echo handler
        client_remote.register_method(2, [](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            echo::debug("client handling echo request of size ", req.size());
            // Echo back the same message
            return dp::result::ok(req);
        });

        // Client calls server's method 1 with 1GB payload
        netpipe::Message client_request(1024 * 1024 * 1024, 0xBB); // 1GB of 0xBB
        auto client_resp = client_remote.call(1, client_request, 60000);
        REQUIRE(client_resp.is_ok());
        CHECK(client_resp.value().size() == 1024 * 1024 * 1024);
        CHECK(client_resp.value()[0] == 0xBB);
        CHECK(client_resp.value()[1024 * 1024 * 1024 - 1] == 0xBB);
        echo::info("client received 1GB response from server");

        // Keep client_remote alive until server thread completes
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));

        // Signal we're done
        finished_count++;

        // Wait for BOTH peers to finish
        while (finished_count < 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // Now both are done, safe to exit and destroy streams
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    });

    server_thread.join();
    client_thread.join();
    server_stream.close();
}

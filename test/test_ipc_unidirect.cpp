#include <chrono>
#include <doctest/doctest.h>
#include <netpipe/netpipe.hpp>
#include <thread>

TEST_CASE("IpcStream + Remote<Unidirect> - 1MB payload") {
    netpipe::IpcStream server;
    netpipe::IpcEndpoint endpoint{"/tmp/netpipe_test_ipc_unidirect_1mb.sock"};

    auto listen_res = server.listen_ipc(endpoint);
    REQUIRE(listen_res.is_ok());

    // Server thread
    std::thread server_thread([&]() {
        auto accept_res = server.accept();
        REQUIRE(accept_res.is_ok());
        auto client_stream = std::move(accept_res.value());

        netpipe::Remote<netpipe::Unidirect> remote(*client_stream);

        // Register echo handler
        remote.register_method(1, [](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            return dp::result::ok(req); // Echo back
        });

        // Serve one request
        auto serve_res = remote.serve();
        // serve() will fail when client closes connection, which is expected
    });

    // Client thread
    std::thread client_thread([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        netpipe::IpcStream client;
        auto connect_res = client.connect_ipc(endpoint);
        REQUIRE(connect_res.is_ok());

        netpipe::Remote<netpipe::Unidirect> remote(client);

        // Create 1MB payload
        dp::usize size = 1 * 1024 * 1024;
        netpipe::Message request(size);
        for (dp::usize i = 0; i < size; i++) {
            request[i] = static_cast<dp::u8>(i % 256);
        }

        auto start = std::chrono::high_resolution_clock::now();
        auto response = remote.call(1, request, 10000);
        auto end = std::chrono::high_resolution_clock::now();

        REQUIRE(response.is_ok());
        CHECK(response.value().size() == size);

        // Verify pattern
        for (dp::usize i = 0; i < size; i++) {
            CHECK(response.value()[i] == static_cast<dp::u8>(i % 256));
        }

        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        echo::info("1MB payload processed in ", duration, "ms");

        client.close();
    });

    client_thread.join();
    server_thread.join();
    server.close();
}

TEST_CASE("IpcStream + Remote<Unidirect> - 10MB payload") {
    netpipe::IpcStream server;
    netpipe::IpcEndpoint endpoint{"/tmp/netpipe_test_ipc_unidirect_10mb.sock"};

    auto listen_res = server.listen_ipc(endpoint);
    REQUIRE(listen_res.is_ok());

    // Server thread
    std::thread server_thread([&]() {
        auto accept_res = server.accept();
        REQUIRE(accept_res.is_ok());
        auto client_stream = std::move(accept_res.value());

        netpipe::Remote<netpipe::Unidirect> remote(*client_stream);

        // Register echo handler
        remote.register_method(1, [](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            return dp::result::ok(req); // Echo back
        });

        // Serve one request
        auto serve_res = remote.serve();
        // serve() will fail when client closes connection, which is expected
    });

    // Client thread
    std::thread client_thread([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        netpipe::IpcStream client;
        auto connect_res = client.connect_ipc(endpoint);
        REQUIRE(connect_res.is_ok());

        netpipe::Remote<netpipe::Unidirect> remote(client);

        // Create 10MB payload
        dp::usize size = 10 * 1024 * 1024;
        netpipe::Message request(size);
        for (dp::usize i = 0; i < size; i++) {
            request[i] = static_cast<dp::u8>(i % 256);
        }

        auto start = std::chrono::high_resolution_clock::now();
        auto response = remote.call(1, request, 10000);
        auto end = std::chrono::high_resolution_clock::now();

        REQUIRE(response.is_ok());
        CHECK(response.value().size() == size);

        // Verify pattern (spot check)
        for (dp::usize i = 0; i < 1000; i++) {
            CHECK(response.value()[i] == static_cast<dp::u8>(i % 256));
        }

        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        echo::info("10MB payload processed in ", duration, "ms");

        client.close();
    });

    client_thread.join();
    server_thread.join();
    server.close();
}

TEST_CASE("IpcStream + Remote<Unidirect> - 100MB payload") {
    netpipe::IpcStream server;
    netpipe::IpcEndpoint endpoint{"/tmp/netpipe_test_ipc_unidirect_100mb.sock"};

    auto listen_res = server.listen_ipc(endpoint);
    REQUIRE(listen_res.is_ok());

    // Server thread
    std::thread server_thread([&]() {
        auto accept_res = server.accept();
        REQUIRE(accept_res.is_ok());
        auto client_stream = std::move(accept_res.value());

        netpipe::Remote<netpipe::Unidirect> remote(*client_stream);

        // Register echo handler
        remote.register_method(1, [](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            return dp::result::ok(req); // Echo back
        });

        // Serve one request
        auto serve_res = remote.serve();
        // serve() will fail when client closes connection, which is expected
    });

    // Client thread
    std::thread client_thread([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        netpipe::IpcStream client;
        auto connect_res = client.connect_ipc(endpoint);
        REQUIRE(connect_res.is_ok());

        netpipe::Remote<netpipe::Unidirect> remote(client);

        // Create 100MB payload
        dp::usize size = 100 * 1024 * 1024;
        netpipe::Message request(size);
        for (dp::usize i = 0; i < size; i++) {
            request[i] = static_cast<dp::u8>(i % 256);
        }

        auto start = std::chrono::high_resolution_clock::now();
        auto response = remote.call(1, request, 15000);
        auto end = std::chrono::high_resolution_clock::now();

        REQUIRE(response.is_ok());
        CHECK(response.value().size() == size);

        // Verify pattern (spot check)
        for (dp::usize i = 0; i < 1000; i++) {
            CHECK(response.value()[i] == static_cast<dp::u8>(i % 256));
        }

        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        echo::info("100MB payload processed in ", duration, "ms");

        client.close();
    });

    client_thread.join();
    server_thread.join();
    server.close();
}

TEST_CASE("IpcStream + Remote<Unidirect> - 1GB payload") {
    netpipe::IpcStream server;
    netpipe::IpcEndpoint endpoint{"/tmp/netpipe_test_ipc_unidirect_1gb.sock"};

    auto listen_res = server.listen_ipc(endpoint);
    REQUIRE(listen_res.is_ok());

    // Server thread
    std::thread server_thread([&]() {
        auto accept_res = server.accept();
        REQUIRE(accept_res.is_ok());
        auto client_stream = std::move(accept_res.value());

        netpipe::Remote<netpipe::Unidirect> remote(*client_stream);

        // Register echo handler
        remote.register_method(1, [](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            return dp::result::ok(req); // Echo back
        });

        // Serve one request
        auto serve_res = remote.serve();
        // serve() will fail when client closes connection, which is expected
    });

    // Client thread
    std::thread client_thread([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        netpipe::IpcStream client;
        auto connect_res = client.connect_ipc(endpoint);
        REQUIRE(connect_res.is_ok());

        netpipe::Remote<netpipe::Unidirect> remote(client);

        // Create 1GB payload
        dp::usize size = 1024 * 1024 * 1024;
        netpipe::Message request(size);
        for (dp::usize i = 0; i < size; i++) {
            request[i] = static_cast<dp::u8>(i % 256);
        }

        auto start = std::chrono::high_resolution_clock::now();
        auto response = remote.call(1, request, 30000);
        auto end = std::chrono::high_resolution_clock::now();

        REQUIRE(response.is_ok());
        CHECK(response.value().size() == size);

        // Verify pattern (spot check)
        for (dp::usize i = 0; i < 1000; i++) {
            CHECK(response.value()[i] == static_cast<dp::u8>(i % 256));
        }

        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        echo::info("1GB payload processed in ", duration, "ms");

        client.close();
    });

    client_thread.join();
    server_thread.join();
    server.close();
}

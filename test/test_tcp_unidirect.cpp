#include <chrono>
#include <doctest/doctest.h>
#include <netpipe/netpipe.hpp>
#include <thread>

TEST_CASE("TCP Unidirect - 1MB payload") {
    netpipe::TcpStream server;
    netpipe::TcpEndpoint endpoint{"127.0.0.1", 18101};

    auto listen_res = server.listen(endpoint);
    REQUIRE(listen_res.is_ok());

    // Server thread
    std::thread server_thread([&]() {
        auto accept_res = server.accept();
        REQUIRE(accept_res.is_ok());
        auto client_stream = std::move(accept_res.value());

        netpipe::Remote<netpipe::Unidirect> remote(*client_stream);

        // Register echo method
        auto register_res = remote.register_method(1, [](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            return dp::result::ok(req); // Echo back
        });
        REQUIRE(register_res.is_ok());

        // Serve one request
        auto recv_res = client_stream->recv();
        if (recv_res.is_ok()) {
            auto decode_res = netpipe::remote::decode_remote_message_v2(recv_res.value());
            REQUIRE(decode_res.is_ok());
            auto decoded = decode_res.value();

            auto handler_res = remote.register_method(
                1, [](const netpipe::Message &req) -> dp::Res<netpipe::Message> { return dp::result::ok(req); });

            netpipe::Message resp = decoded.payload;
            auto response = netpipe::remote::encode_remote_message_v2(decoded.request_id, decoded.method_id, resp,
                                                                      netpipe::remote::MessageType::Response);
            client_stream->send(response);
        }
    });

    // Client thread
    std::thread client_thread([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        netpipe::TcpStream client;
        auto connect_res = client.connect(endpoint);
        REQUIRE(connect_res.is_ok());

        netpipe::Remote<netpipe::Unidirect> remote(client);

        // Create 1MB payload
        dp::usize size = 1 * 1024 * 1024;
        netpipe::Message request(size);
        for (dp::usize i = 0; i < size; i++) {
            request[i] = static_cast<dp::u8>(i % 256);
        }

        auto response = remote.call(1, request, 30000);
        REQUIRE(response.is_ok());
        CHECK(response.value().size() == size);

        // Verify payload matches
        for (dp::usize i = 0; i < size; i++) {
            if (response.value()[i] != static_cast<dp::u8>(i % 256)) {
                CHECK(false); // Payload mismatch
                break;
            }
        }

        client.close();
    });

    server_thread.join();
    client_thread.join();
    server.close();
}

TEST_CASE("TCP Unidirect - 10MB payload") {
    netpipe::TcpStream server;
    netpipe::TcpEndpoint endpoint{"127.0.0.1", 18102};

    auto listen_res = server.listen(endpoint);
    REQUIRE(listen_res.is_ok());

    // Server thread
    std::thread server_thread([&]() {
        auto accept_res = server.accept();
        REQUIRE(accept_res.is_ok());
        auto client_stream = std::move(accept_res.value());

        netpipe::Remote<netpipe::Unidirect> remote(*client_stream);

        // Register echo method
        auto register_res = remote.register_method(1, [](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            return dp::result::ok(req); // Echo back
        });
        REQUIRE(register_res.is_ok());

        // Serve one request
        auto recv_res = client_stream->recv();
        if (recv_res.is_ok()) {
            auto decode_res = netpipe::remote::decode_remote_message_v2(recv_res.value());
            REQUIRE(decode_res.is_ok());
            auto decoded = decode_res.value();

            netpipe::Message resp = decoded.payload;
            auto response = netpipe::remote::encode_remote_message_v2(decoded.request_id, decoded.method_id, resp,
                                                                      netpipe::remote::MessageType::Response);
            client_stream->send(response);
        }
    });

    // Client thread
    std::thread client_thread([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        netpipe::TcpStream client;
        auto connect_res = client.connect(endpoint);
        REQUIRE(connect_res.is_ok());

        netpipe::Remote<netpipe::Unidirect> remote(client);

        // Create 10MB payload
        dp::usize size = 10 * 1024 * 1024;
        netpipe::Message request(size);
        for (dp::usize i = 0; i < size; i++) {
            request[i] = static_cast<dp::u8>(i % 256);
        }

        auto response = remote.call(1, request, 30000);
        REQUIRE(response.is_ok());
        CHECK(response.value().size() == size);

        // Verify payload matches
        for (dp::usize i = 0; i < size; i++) {
            if (response.value()[i] != static_cast<dp::u8>(i % 256)) {
                CHECK(false); // Payload mismatch
                break;
            }
        }

        client.close();
    });

    server_thread.join();
    client_thread.join();
    server.close();
}

#ifdef BIG_TRANSFER
TEST_CASE("TCP Unidirect - 100MB payload") {
    netpipe::TcpStream server;
    netpipe::TcpEndpoint endpoint{"127.0.0.1", 18103};

    auto listen_res = server.listen(endpoint);
    REQUIRE(listen_res.is_ok());

    // Server thread
    std::thread server_thread([&]() {
        auto accept_res = server.accept();
        REQUIRE(accept_res.is_ok());
        auto client_stream = std::move(accept_res.value());

        netpipe::Remote<netpipe::Unidirect> remote(*client_stream);

        // Register echo method
        auto register_res = remote.register_method(1, [](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            return dp::result::ok(req); // Echo back
        });
        REQUIRE(register_res.is_ok());

        // Serve one request
        auto recv_res = client_stream->recv();
        if (recv_res.is_ok()) {
            auto decode_res = netpipe::remote::decode_remote_message_v2(recv_res.value());
            REQUIRE(decode_res.is_ok());
            auto decoded = decode_res.value();

            netpipe::Message resp = decoded.payload;
            auto response = netpipe::remote::encode_remote_message_v2(decoded.request_id, decoded.method_id, resp,
                                                                      netpipe::remote::MessageType::Response);
            client_stream->send(response);
        }
    });

    // Client thread
    std::thread client_thread([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        netpipe::TcpStream client;
        auto connect_res = client.connect(endpoint);
        REQUIRE(connect_res.is_ok());

        netpipe::Remote<netpipe::Unidirect> remote(client);

        // Create 100MB payload
        dp::usize size = 100 * 1024 * 1024;
        netpipe::Message request(size);
        for (dp::usize i = 0; i < size; i++) {
            request[i] = static_cast<dp::u8>(i % 256);
        }

        auto response = remote.call(1, request, 30000);
        REQUIRE(response.is_ok());
        CHECK(response.value().size() == size);

        // Verify payload matches
        for (dp::usize i = 0; i < size; i++) {
            if (response.value()[i] != static_cast<dp::u8>(i % 256)) {
                CHECK(false); // Payload mismatch
                break;
            }
        }

        client.close();
    });

    server_thread.join();
    client_thread.join();
    server.close();
}

TEST_CASE("TCP Unidirect - 1GB payload") {
    netpipe::TcpStream server;
    netpipe::TcpEndpoint endpoint{"127.0.0.1", 18104};

    auto listen_res = server.listen(endpoint);
    REQUIRE(listen_res.is_ok());

    // Server thread
    std::thread server_thread([&]() {
        auto accept_res = server.accept();
        REQUIRE(accept_res.is_ok());
        auto client_stream = std::move(accept_res.value());

        netpipe::Remote<netpipe::Unidirect> remote(*client_stream);

        // Register echo method
        auto register_res = remote.register_method(1, [](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            return dp::result::ok(req); // Echo back
        });
        REQUIRE(register_res.is_ok());

        // Serve one request
        auto recv_res = client_stream->recv();
        if (recv_res.is_ok()) {
            auto decode_res = netpipe::remote::decode_remote_message_v2(recv_res.value());
            REQUIRE(decode_res.is_ok());
            auto decoded = decode_res.value();

            netpipe::Message resp = decoded.payload;
            auto response = netpipe::remote::encode_remote_message_v2(decoded.request_id, decoded.method_id, resp,
                                                                      netpipe::remote::MessageType::Response);
            client_stream->send(response);
        }
    });

    // Client thread
    std::thread client_thread([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        netpipe::TcpStream client;
        auto connect_res = client.connect(endpoint);
        REQUIRE(connect_res.is_ok());

        netpipe::Remote<netpipe::Unidirect> remote(client);

        // Create 1GB payload (minus protocol overhead to stay under MAX_MESSAGE_SIZE)
        // Protocol adds 16 bytes overhead, so max payload is 1GB - 16 bytes
        dp::usize size = 1024 * 1024 * 1024 - 16;
        netpipe::Message request(size);
        for (dp::usize i = 0; i < size; i++) {
            request[i] = static_cast<dp::u8>(i % 256);
        }

        auto response = remote.call(1, request, 30000);
        REQUIRE(response.is_ok());
        CHECK(response.value().size() == size);

        // Verify payload matches
        for (dp::usize i = 0; i < size; i++) {
            if (response.value()[i] != static_cast<dp::u8>(i % 256)) {
                CHECK(false); // Payload mismatch
                break;
            }
        }

        client.close();
    });

    server_thread.join();
    client_thread.join();
    server.close();
}
#endif // BIG_TRANSFER

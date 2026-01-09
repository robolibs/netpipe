#include <chrono>
#include <doctest/doctest.h>
#include <netpipe/netpipe.hpp>
#include <netpipe/stream/shm.hpp>
#include <thread>

TEST_CASE("SHM + Remote<Unidirect> - 1MB payload") {
    constexpr dp::usize PAYLOAD_SIZE = 1 * 1024 * 1024;
    constexpr dp::usize BUFFER_SIZE = 8 * 1024 * 1024;
    constexpr dp::u32 METHOD_ID = 100;

    netpipe::ShmStream listener;
    netpipe::ShmEndpoint endpoint{"test_shm_unidirect_1mb", BUFFER_SIZE};

    auto listen_res = listener.listen_shm(endpoint);
    REQUIRE(listen_res.is_ok());

    std::unique_ptr<netpipe::Stream> server_conn;

    std::thread server_thread([&]() {
        auto accept_res = listener.accept();
        REQUIRE(accept_res.is_ok());
        server_conn = std::move(accept_res.value());

        netpipe::Remote<netpipe::Unidirect> server_remote(*server_conn);

        // Register handler that echoes the payload
        auto register_res = server_remote.register_method(METHOD_ID, [](const netpipe::Message &request) {
            return dp::result::ok(request);
        });
        REQUIRE(register_res.is_ok());

        // Serve one request
        auto recv_res = server_conn->recv();
        if (recv_res.is_ok()) {
            auto decode_res = netpipe::remote::decode_remote_message_v2(recv_res.value());
            if (decode_res.is_ok()) {
                auto decoded = decode_res.value();
                netpipe::Message response_payload = decoded.payload;
                netpipe::Message remote_response = netpipe::remote::encode_remote_message_v2(
                    decoded.request_id, decoded.method_id, response_payload, netpipe::remote::MessageType::Response);
                server_conn->send(remote_response);
            }
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Client connects and makes RPC call
    netpipe::ShmStream client_stream;
    auto connect_res = client_stream.connect_shm(endpoint);
    REQUIRE(connect_res.is_ok());

    netpipe::Remote<netpipe::Unidirect> client_remote(client_stream);

    // Create 1MB payload
    netpipe::Message request(PAYLOAD_SIZE);
    for (dp::usize i = 0; i < PAYLOAD_SIZE; i++) {
        request[i] = static_cast<dp::u8>(i % 256);
    }

    auto start = std::chrono::high_resolution_clock::now();
    auto call_res = client_remote.call(METHOD_ID, request, 10000);
    auto end = std::chrono::high_resolution_clock::now();

    REQUIRE(call_res.is_ok());
    auto response = call_res.value();

    CHECK(response.size() == PAYLOAD_SIZE);
    CHECK(response[0] == 0);
    CHECK(response[PAYLOAD_SIZE - 1] == static_cast<dp::u8>((PAYLOAD_SIZE - 1) % 256));

    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    echo::info("1MB payload round-trip time: ", duration, "ms");

    server_thread.join();
    client_stream.close();
    listener.close();
}

TEST_CASE("SHM + Remote<Unidirect> - 10MB payload") {
    constexpr dp::usize PAYLOAD_SIZE = 10 * 1024 * 1024;
    constexpr dp::usize BUFFER_SIZE = 64 * 1024 * 1024;
    constexpr dp::u32 METHOD_ID = 101;

    netpipe::ShmStream listener;
    netpipe::ShmEndpoint endpoint{"test_shm_unidirect_10mb", BUFFER_SIZE};

    auto listen_res = listener.listen_shm(endpoint);
    REQUIRE(listen_res.is_ok());

    std::unique_ptr<netpipe::Stream> server_conn;

    std::thread server_thread([&]() {
        auto accept_res = listener.accept();
        REQUIRE(accept_res.is_ok());
        server_conn = std::move(accept_res.value());

        netpipe::Remote<netpipe::Unidirect> server_remote(*server_conn);

        auto register_res = server_remote.register_method(
            METHOD_ID, [](const netpipe::Message &request) { return dp::result::ok(request); });
        REQUIRE(register_res.is_ok());

        auto recv_res = server_conn->recv();
        if (recv_res.is_ok()) {
            auto decode_res = netpipe::remote::decode_remote_message_v2(recv_res.value());
            if (decode_res.is_ok()) {
                auto decoded = decode_res.value();
                netpipe::Message response_payload = decoded.payload;
                netpipe::Message remote_response = netpipe::remote::encode_remote_message_v2(
                    decoded.request_id, decoded.method_id, response_payload, netpipe::remote::MessageType::Response);
                server_conn->send(remote_response);
            }
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    netpipe::ShmStream client_stream;
    auto connect_res = client_stream.connect_shm(endpoint);
    REQUIRE(connect_res.is_ok());

    netpipe::Remote<netpipe::Unidirect> client_remote(client_stream);

    netpipe::Message request(PAYLOAD_SIZE);
    for (dp::usize i = 0; i < PAYLOAD_SIZE; i++) {
        request[i] = static_cast<dp::u8>(i % 256);
    }

    auto start = std::chrono::high_resolution_clock::now();
    auto call_res = client_remote.call(METHOD_ID, request, 10000);
    auto end = std::chrono::high_resolution_clock::now();

    REQUIRE(call_res.is_ok());
    auto response = call_res.value();

    CHECK(response.size() == PAYLOAD_SIZE);
    CHECK(response[0] == 0);
    CHECK(response[PAYLOAD_SIZE - 1] == static_cast<dp::u8>((PAYLOAD_SIZE - 1) % 256));

    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    echo::info("10MB payload round-trip time: ", duration, "ms");

    server_thread.join();
    client_stream.close();
    listener.close();
}

#ifdef BIG_TRANSFER
TEST_CASE("SHM + Remote<Unidirect> - 100MB payload") {
    constexpr dp::usize PAYLOAD_SIZE = 100 * 1024 * 1024;
    constexpr dp::usize BUFFER_SIZE = 512 * 1024 * 1024;
    constexpr dp::u32 METHOD_ID = 102;

    netpipe::ShmStream listener;
    netpipe::ShmEndpoint endpoint{"test_shm_unidirect_100mb", BUFFER_SIZE};

    auto listen_res = listener.listen_shm(endpoint);
    REQUIRE(listen_res.is_ok());

    std::unique_ptr<netpipe::Stream> server_conn;

    std::thread server_thread([&]() {
        auto accept_res = listener.accept();
        REQUIRE(accept_res.is_ok());
        server_conn = std::move(accept_res.value());

        netpipe::Remote<netpipe::Unidirect> server_remote(*server_conn);

        auto register_res = server_remote.register_method(
            METHOD_ID, [](const netpipe::Message &request) { return dp::result::ok(request); });
        REQUIRE(register_res.is_ok());

        auto recv_res = server_conn->recv();
        if (recv_res.is_ok()) {
            auto decode_res = netpipe::remote::decode_remote_message_v2(recv_res.value());
            if (decode_res.is_ok()) {
                auto decoded = decode_res.value();
                netpipe::Message response_payload = decoded.payload;
                netpipe::Message remote_response = netpipe::remote::encode_remote_message_v2(
                    decoded.request_id, decoded.method_id, response_payload, netpipe::remote::MessageType::Response);
                server_conn->send(remote_response);
            }
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    netpipe::ShmStream client_stream;
    auto connect_res = client_stream.connect_shm(endpoint);
    REQUIRE(connect_res.is_ok());

    netpipe::Remote<netpipe::Unidirect> client_remote(client_stream);

    netpipe::Message request(PAYLOAD_SIZE);
    for (dp::usize i = 0; i < PAYLOAD_SIZE; i++) {
        request[i] = static_cast<dp::u8>(i % 256);
    }

    auto start = std::chrono::high_resolution_clock::now();
    auto call_res = client_remote.call(METHOD_ID, request, 30000);
    auto end = std::chrono::high_resolution_clock::now();

    REQUIRE(call_res.is_ok());
    auto response = call_res.value();

    CHECK(response.size() == PAYLOAD_SIZE);
    CHECK(response[0] == 0);
    CHECK(response[PAYLOAD_SIZE - 1] == static_cast<dp::u8>((PAYLOAD_SIZE - 1) % 256));

    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    echo::info("100MB payload round-trip time: ", duration, "ms");

    server_thread.join();
    client_stream.close();
    listener.close();
}

TEST_CASE("SHM + Remote<Unidirect> - 1GB payload") {
    constexpr dp::usize PAYLOAD_SIZE = 1024ULL * 1024 * 1024;
    constexpr dp::usize BUFFER_SIZE = 3ULL * 1024 * 1024 * 1024;
    constexpr dp::u32 METHOD_ID = 103;

    netpipe::ShmStream listener;
    netpipe::ShmEndpoint endpoint{"test_shm_unidirect_1gb", BUFFER_SIZE};

    auto listen_res = listener.listen_shm(endpoint);
    REQUIRE(listen_res.is_ok());

    std::unique_ptr<netpipe::Stream> server_conn;

    std::thread server_thread([&]() {
        auto accept_res = listener.accept();
        REQUIRE(accept_res.is_ok());
        server_conn = std::move(accept_res.value());

        netpipe::Remote<netpipe::Unidirect> server_remote(*server_conn);

        auto register_res = server_remote.register_method(
            METHOD_ID, [](const netpipe::Message &request) { return dp::result::ok(request); });
        REQUIRE(register_res.is_ok());

        auto recv_res = server_conn->recv();
        if (recv_res.is_ok()) {
            auto decode_res = netpipe::remote::decode_remote_message_v2(recv_res.value());
            if (decode_res.is_ok()) {
                auto decoded = decode_res.value();
                netpipe::Message response_payload = decoded.payload;
                netpipe::Message remote_response = netpipe::remote::encode_remote_message_v2(
                    decoded.request_id, decoded.method_id, response_payload, netpipe::remote::MessageType::Response);
                server_conn->send(remote_response);
            }
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    netpipe::ShmStream client_stream;
    auto connect_res = client_stream.connect_shm(endpoint);
    REQUIRE(connect_res.is_ok());

    netpipe::Remote<netpipe::Unidirect> client_remote(client_stream);

    netpipe::Message request(PAYLOAD_SIZE);
    for (dp::usize i = 0; i < PAYLOAD_SIZE; i++) {
        request[i] = static_cast<dp::u8>(i % 256);
    }

    auto start = std::chrono::high_resolution_clock::now();
    auto call_res = client_remote.call(METHOD_ID, request, 120000);
    auto end = std::chrono::high_resolution_clock::now();

    REQUIRE(call_res.is_ok());
    auto response = call_res.value();

    CHECK(response.size() == PAYLOAD_SIZE);
    CHECK(response[0] == 0);
    CHECK(response[PAYLOAD_SIZE - 1] == static_cast<dp::u8>((PAYLOAD_SIZE - 1) % 256));

    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    echo::info("1GB payload round-trip time: ", duration, "ms");

    server_thread.join();
    client_stream.close();
    listener.close();
}
#endif // BIG_TRANSFER

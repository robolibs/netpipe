#include <chrono>
#include <doctest/doctest.h>
#include <netpipe/netpipe.hpp>
#include <thread>

TEST_CASE("Remote - Basic call/serve functionality") {
    SUBCASE("Simple echo request/response") {
        netpipe::TcpStream server;
        netpipe::TcpEndpoint endpoint{"127.0.0.1", 18001};

        auto listen_res = server.listen(endpoint);
        REQUIRE(listen_res.is_ok());

        // Server thread
        std::thread server_thread([&]() {
            auto accept_res = server.accept();
            REQUIRE(accept_res.is_ok());
            auto client_stream = std::move(accept_res.value());

            netpipe::Remote remote(*client_stream);

            // Echo handler
            auto handler = [](const netpipe::Message &request) -> dp::Res<netpipe::Message> {
                return dp::result::ok(request); // Echo back
            };

            // Serve one request then exit (manually to avoid infinite loop)
            auto recv_res = client_stream->recv();
            if (recv_res.is_ok()) {
                auto decode_res = netpipe::remote::decode_remote_message(recv_res.value());
                if (decode_res.is_ok()) {
                    auto decoded = decode_res.value();
                    auto handler_res = handler(decoded.payload);
                    netpipe::Message response;
                    if (handler_res.is_ok()) {
                        response =
                            netpipe::remote::encode_remote_message(decoded.request_id, handler_res.value(), false);
                    } else {
                        netpipe::Message error_msg(handler_res.error().message.begin(),
                                                   handler_res.error().message.end());
                        response = netpipe::remote::encode_remote_message(decoded.request_id, error_msg, true);
                    }
                    client_stream->send(response);
                }
            }
        });

        // Client
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        netpipe::TcpStream client;
        auto connect_res = client.connect(endpoint);
        REQUIRE(connect_res.is_ok());

        netpipe::Remote remote(client);

        netpipe::Message request = {0x01, 0x02, 0x03};
        auto call_res = remote.call(request, 1000);
        REQUIRE(call_res.is_ok());

        auto response = call_res.value();
        CHECK(response.size() == 3);
        CHECK(response[0] == 0x01);
        CHECK(response[1] == 0x02);
        CHECK(response[2] == 0x03);

        client.close();
        server_thread.join();
        server.close();
    }

    SUBCASE("Multiple sequential calls") {
        netpipe::TcpStream server;
        netpipe::TcpEndpoint endpoint{"127.0.0.1", 18002};

        auto listen_res = server.listen(endpoint);
        REQUIRE(listen_res.is_ok());

        // Server thread
        std::thread server_thread([&]() {
            auto accept_res = server.accept();
            REQUIRE(accept_res.is_ok());
            auto client_stream = std::move(accept_res.value());

            netpipe::Remote remote(*client_stream);

            // Handler that increments each byte
            auto handler = [](const netpipe::Message &request) -> dp::Res<netpipe::Message> {
                netpipe::Message response = request;
                for (auto &byte : response) {
                    byte++;
                }
                return dp::result::ok(response);
            };

            // Serve 3 requests
            for (int i = 0; i < 3; i++) {
                auto recv_res = client_stream->recv();
                if (recv_res.is_ok()) {
                    auto decode_res = netpipe::remote::decode_remote_message(recv_res.value());
                    if (decode_res.is_ok()) {
                        auto decoded = decode_res.value();
                        auto handler_res = handler(decoded.payload);
                        if (handler_res.is_ok()) {
                            auto response =
                                netpipe::remote::encode_remote_message(decoded.request_id, handler_res.value());
                            client_stream->send(response);
                        }
                    }
                }
            }
        });

        // Client makes 3 calls
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        netpipe::TcpStream client;
        auto connect_res = client.connect(endpoint);
        REQUIRE(connect_res.is_ok());

        netpipe::Remote remote(client);

        for (int i = 0; i < 3; i++) {
            netpipe::Message request = {static_cast<dp::u8>(i), static_cast<dp::u8>(i + 1)};
            auto call_res = remote.call(request, 1000);
            REQUIRE(call_res.is_ok());

            auto response = call_res.value();
            CHECK(response.size() == 2);
            CHECK(response[0] == static_cast<dp::u8>(i + 1));
            CHECK(response[1] == static_cast<dp::u8>(i + 2));
        }

        client.close();
        server_thread.join();
        server.close();
    }
}

TEST_CASE("Remote - Timeout behavior") {
    SUBCASE("Call completes before timeout") {
        netpipe::TcpStream server;
        netpipe::TcpEndpoint endpoint{"127.0.0.1", 18003};

        auto listen_res = server.listen(endpoint);
        REQUIRE(listen_res.is_ok());

        // Server thread - responds quickly
        std::thread server_thread([&]() {
            auto accept_res = server.accept();
            REQUIRE(accept_res.is_ok());
            auto client_stream = std::move(accept_res.value());

            auto recv_res = client_stream->recv();
            if (recv_res.is_ok()) {
                auto decode_res = netpipe::remote::decode_remote_message(recv_res.value());
                if (decode_res.is_ok()) {
                    auto decoded = decode_res.value();
                    auto response = netpipe::remote::encode_remote_message(decoded.request_id, decoded.payload);
                    client_stream->send(response);
                }
            }
        });

        // Client with generous timeout
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        netpipe::TcpStream client;
        auto connect_res = client.connect(endpoint);
        REQUIRE(connect_res.is_ok());

        netpipe::Remote remote(client);

        netpipe::Message request = {0xAA, 0xBB};
        auto call_res = remote.call(request, 5000); // 5 second timeout
        CHECK(call_res.is_ok());

        client.close();
        server_thread.join();
        server.close();
    }

    SUBCASE("Call times out when server doesn't respond") {
        netpipe::TcpStream server;
        netpipe::TcpEndpoint endpoint{"127.0.0.1", 18004};

        auto listen_res = server.listen(endpoint);
        REQUIRE(listen_res.is_ok());

        // Server thread - accepts but never responds
        std::thread server_thread([&]() {
            auto accept_res = server.accept();
            REQUIRE(accept_res.is_ok());
            auto client_stream = std::move(accept_res.value());

            // Read request but don't respond
            auto recv_res = client_stream->recv();

            // Sleep to ensure client times out
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        });

        // Client with short timeout
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        netpipe::TcpStream client;
        auto connect_res = client.connect(endpoint);
        REQUIRE(connect_res.is_ok());

        netpipe::Remote remote(client);

        netpipe::Message request = {0x01};
        auto call_res = remote.call(request, 500); // 500ms timeout

        // Should timeout
        CHECK(call_res.is_err());
        // Note: Error type should be timeout, but we check for any error

        client.close();
        server_thread.join();
        server.close();
    }
}

TEST_CASE("Remote - Request ID matching") {
    SUBCASE("Correct request ID in response") {
        netpipe::TcpStream server;
        netpipe::TcpEndpoint endpoint{"127.0.0.1", 18005};

        auto listen_res = server.listen(endpoint);
        REQUIRE(listen_res.is_ok());

        // Server thread
        std::thread server_thread([&]() {
            auto accept_res = server.accept();
            REQUIRE(accept_res.is_ok());
            auto client_stream = std::move(accept_res.value());

            auto recv_res = client_stream->recv();
            if (recv_res.is_ok()) {
                auto decode_res = netpipe::remote::decode_remote_message(recv_res.value());
                if (decode_res.is_ok()) {
                    auto decoded = decode_res.value();
                    // Echo back with same request_id
                    auto response = netpipe::remote::encode_remote_message(decoded.request_id, decoded.payload);
                    client_stream->send(response);
                }
            }
        });

        // Client
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        netpipe::TcpStream client;
        auto connect_res = client.connect(endpoint);
        REQUIRE(connect_res.is_ok());

        netpipe::Remote remote(client);

        netpipe::Message request = {0xFF};
        auto call_res = remote.call(request, 1000);
        CHECK(call_res.is_ok());

        client.close();
        server_thread.join();
        server.close();
    }

    SUBCASE("Mismatched request ID causes error") {
        netpipe::TcpStream server;
        netpipe::TcpEndpoint endpoint{"127.0.0.1", 18006};

        auto listen_res = server.listen(endpoint);
        REQUIRE(listen_res.is_ok());

        // Server thread - sends wrong request_id
        std::thread server_thread([&]() {
            auto accept_res = server.accept();
            REQUIRE(accept_res.is_ok());
            auto client_stream = std::move(accept_res.value());

            auto recv_res = client_stream->recv();
            if (recv_res.is_ok()) {
                auto decode_res = netpipe::remote::decode_remote_message(recv_res.value());
                if (decode_res.is_ok()) {
                    auto decoded = decode_res.value();
                    // Send response with WRONG request_id
                    auto response = netpipe::remote::encode_remote_message(decoded.request_id + 999, decoded.payload);
                    client_stream->send(response);
                }
            }
        });

        // Client
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        netpipe::TcpStream client;
        auto connect_res = client.connect(endpoint);
        REQUIRE(connect_res.is_ok());

        netpipe::Remote remote(client);

        netpipe::Message request = {0x42};
        auto call_res = remote.call(request, 1000);

        // Should fail due to request_id mismatch
        CHECK(call_res.is_err());

        client.close();
        server_thread.join();
        server.close();
    }
}

TEST_CASE("Remote - Large message handling") {
    SUBCASE("Send and receive large message") {
        netpipe::TcpStream server;
        netpipe::TcpEndpoint endpoint{"127.0.0.1", 18007};

        auto listen_res = server.listen(endpoint);
        REQUIRE(listen_res.is_ok());

        // Server thread
        std::thread server_thread([&]() {
            auto accept_res = server.accept();
            REQUIRE(accept_res.is_ok());
            auto client_stream = std::move(accept_res.value());

            auto recv_res = client_stream->recv();
            if (recv_res.is_ok()) {
                auto decode_res = netpipe::remote::decode_remote_message(recv_res.value());
                if (decode_res.is_ok()) {
                    auto decoded = decode_res.value();
                    // Echo back
                    auto response = netpipe::remote::encode_remote_message(decoded.request_id, decoded.payload);
                    client_stream->send(response);
                }
            }
        });

        // Client sends large message (1MB)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        netpipe::TcpStream client;
        auto connect_res = client.connect(endpoint);
        REQUIRE(connect_res.is_ok());

        netpipe::Remote remote(client);

        // Create 1MB message
        netpipe::Message large_request(1024 * 1024);
        for (dp::usize i = 0; i < large_request.size(); i++) {
            large_request[i] = static_cast<dp::u8>(i % 256);
        }

        auto call_res = remote.call(large_request, 5000);
        REQUIRE(call_res.is_ok());

        auto response = call_res.value();
        CHECK(response.size() == large_request.size());

        // Verify content
        bool content_matches = true;
        for (dp::usize i = 0; i < response.size(); i++) {
            if (response[i] != large_request[i]) {
                content_matches = false;
                break;
            }
        }
        CHECK(content_matches);

        client.close();
        server_thread.join();
        server.close();
    }
}

TEST_CASE("Remote - Error handling") {
    SUBCASE("Call on disconnected stream fails") {
        netpipe::TcpStream client;
        // Not connected

        netpipe::Remote remote(client);

        netpipe::Message request = {0x01};
        auto call_res = remote.call(request, 1000);

        CHECK(call_res.is_err());
    }

    SUBCASE("Malformed message causes decode error") {
        // This test verifies protocol decode error handling
        netpipe::Message malformed = {0x00, 0x01}; // Too short (< 9 bytes)

        auto decode_res = netpipe::remote::decode_remote_message(malformed);
        CHECK(decode_res.is_err());
    }

    SUBCASE("Message size mismatch causes decode error") {
        // Create message with incorrect length field
        netpipe::Message msg;

        // request_id = 0
        msg.push_back(0x00);
        msg.push_back(0x00);
        msg.push_back(0x00);
        msg.push_back(0x00);

        // is_error = 0
        msg.push_back(0x00);

        // length = 10 (but we only provide 2 bytes)
        msg.push_back(0x00);
        msg.push_back(0x00);
        msg.push_back(0x00);
        msg.push_back(0x0A);

        // payload (only 2 bytes instead of 10)
        msg.push_back(0xAA);
        msg.push_back(0xBB);

        auto decode_res = netpipe::remote::decode_remote_message(msg);
        CHECK(decode_res.is_err());
    }
}

TEST_CASE("Remote - Protocol encoding/decoding") {
    SUBCASE("Encode and decode round trip") {
        dp::u32 request_id = 12345;
        netpipe::Message payload = {0x01, 0x02, 0x03, 0x04, 0x05};

        auto encoded = netpipe::remote::encode_remote_message(request_id, payload);

        // Verify encoded format: [request_id:4][is_error:1][length:4][payload:N]
        CHECK(encoded.size() == 9 + payload.size());

        auto decode_res = netpipe::remote::decode_remote_message(encoded);
        REQUIRE(decode_res.is_ok());

        auto decoded = decode_res.value();
        CHECK(decoded.request_id == request_id);
        CHECK(decoded.is_error == false);
        CHECK(decoded.payload.size() == payload.size());

        for (dp::usize i = 0; i < payload.size(); i++) {
            CHECK(decoded.payload[i] == payload[i]);
        }
    }

    SUBCASE("Empty payload") {
        dp::u32 request_id = 0;
        netpipe::Message empty_payload;

        auto encoded = netpipe::remote::encode_remote_message(request_id, empty_payload);
        CHECK(encoded.size() == 9); // Header with is_error flag, no payload

        auto decode_res = netpipe::remote::decode_remote_message(encoded);
        REQUIRE(decode_res.is_ok());

        auto decoded = decode_res.value();
        CHECK(decoded.request_id == 0);
        CHECK(decoded.is_error == false);
        CHECK(decoded.payload.size() == 0);
    }

    SUBCASE("Error response encoding") {
        dp::u32 request_id = 42;
        dp::String error_msg("Something went wrong");
        netpipe::Message error_payload(error_msg.begin(), error_msg.end());

        auto encoded = netpipe::remote::encode_remote_message(request_id, error_payload, true);

        auto decode_res = netpipe::remote::decode_remote_message(encoded);
        REQUIRE(decode_res.is_ok());

        auto decoded = decode_res.value();
        CHECK(decoded.request_id == 42);
        CHECK(decoded.is_error == true);
        CHECK(decoded.payload.size() == error_msg.size());

        dp::String decoded_error(reinterpret_cast<const char *>(decoded.payload.data()), decoded.payload.size());
        CHECK(decoded_error == error_msg);
    }
}

TEST_CASE("Remote - Handler error responses") {
    SUBCASE("Handler returns error") {
        netpipe::TcpStream server;
        netpipe::TcpEndpoint endpoint{"127.0.0.1", 18008};

        auto listen_res = server.listen(endpoint);
        REQUIRE(listen_res.is_ok());

        // Server thread with error-returning handler
        std::thread server_thread([&]() {
            auto accept_res = server.accept();
            REQUIRE(accept_res.is_ok());
            auto client_stream = std::move(accept_res.value());

            netpipe::Remote remote(*client_stream);

            // Handler that returns error for certain requests
            auto handler = [](const netpipe::Message &request) -> dp::Res<netpipe::Message> {
                if (request.size() > 0 && request[0] == 0xFF) {
                    return dp::result::err(dp::Error::invalid_argument("Invalid request byte"));
                }
                return dp::result::ok(request);
            };

            // Serve two requests
            for (int i = 0; i < 2; i++) {
                auto recv_res = client_stream->recv();
                if (recv_res.is_ok()) {
                    auto decode_res = netpipe::remote::decode_remote_message(recv_res.value());
                    if (decode_res.is_ok()) {
                        auto decoded = decode_res.value();
                        auto handler_res = handler(decoded.payload);
                        netpipe::Message response;
                        if (handler_res.is_ok()) {
                            response =
                                netpipe::remote::encode_remote_message(decoded.request_id, handler_res.value(), false);
                        } else {
                            netpipe::Message error_msg(handler_res.error().message.begin(),
                                                       handler_res.error().message.end());
                            response = netpipe::remote::encode_remote_message(decoded.request_id, error_msg, true);
                        }
                        client_stream->send(response);
                    }
                }
            }
        });

        // Client
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        netpipe::TcpStream client;
        auto connect_res = client.connect(endpoint);
        REQUIRE(connect_res.is_ok());

        netpipe::Remote remote(client);

        // First call - should succeed
        netpipe::Message good_request = {0x01, 0x02};
        auto call_res1 = remote.call(good_request, 1000);
        CHECK(call_res1.is_ok());

        // Second call - should fail with error
        netpipe::Message bad_request = {0xFF};
        auto call_res2 = remote.call(bad_request, 1000);
        CHECK(call_res2.is_err());

        client.close();
        server_thread.join();
        server.close();
    }
}

TEST_CASE("Remote - Protocol V2") {
    SUBCASE("V2 encode and decode round trip") {
        dp::u32 request_id = 12345;
        dp::u32 method_id = 42;
        netpipe::Message payload = {0x01, 0x02, 0x03, 0x04, 0x05};

        auto encoded = netpipe::remote::encode_remote_message_v2(request_id, method_id, payload,
                                                                  netpipe::remote::MessageType::Request,
                                                                  netpipe::remote::MessageFlags::None);

        // Verify encoded format: [version:1][type:1][flags:2][request_id:4][method_id:4][length:4][payload:N]
        CHECK(encoded.size() == 16 + payload.size());
        CHECK(encoded[0] == netpipe::remote::PROTOCOL_VERSION_2);
        CHECK(encoded[1] == static_cast<dp::u8>(netpipe::remote::MessageType::Request));

        auto decode_res = netpipe::remote::decode_remote_message_v2(encoded);
        REQUIRE(decode_res.is_ok());

        auto decoded = decode_res.value();
        CHECK(decoded.version == netpipe::remote::PROTOCOL_VERSION_2);
        CHECK(decoded.type == netpipe::remote::MessageType::Request);
        CHECK(decoded.flags == netpipe::remote::MessageFlags::None);
        CHECK(decoded.request_id == request_id);
        CHECK(decoded.method_id == method_id);
        CHECK(decoded.payload.size() == payload.size());

        for (dp::usize i = 0; i < payload.size(); i++) {
            CHECK(decoded.payload[i] == payload[i]);
        }
    }

    SUBCASE("V2 different message types") {
        dp::u32 request_id = 100;
        dp::u32 method_id = 5;
        netpipe::Message payload = {0xAA, 0xBB};

        // Test Response type
        auto response_msg = netpipe::remote::encode_remote_message_v2(
            request_id, method_id, payload, netpipe::remote::MessageType::Response);
        auto response_res = netpipe::remote::decode_remote_message_v2(response_msg);
        REQUIRE(response_res.is_ok());
        CHECK(response_res.value().type == netpipe::remote::MessageType::Response);

        // Test Error type
        auto error_msg = netpipe::remote::encode_remote_message_v2(request_id, method_id, payload,
                                                                    netpipe::remote::MessageType::Error);
        auto error_res = netpipe::remote::decode_remote_message_v2(error_msg);
        REQUIRE(error_res.is_ok());
        CHECK(error_res.value().type == netpipe::remote::MessageType::Error);

        // Test Notification type
        auto notif_msg = netpipe::remote::encode_remote_message_v2(request_id, method_id, payload,
                                                                    netpipe::remote::MessageType::Notification);
        auto notif_res = netpipe::remote::decode_remote_message_v2(notif_msg);
        REQUIRE(notif_res.is_ok());
        CHECK(notif_res.value().type == netpipe::remote::MessageType::Notification);
    }

    SUBCASE("V2 message flags") {
        dp::u32 request_id = 200;
        dp::u32 method_id = 10;
        netpipe::Message payload = {0x01};

        // Test with flags
        dp::u16 flags = netpipe::remote::MessageFlags::Compressed | netpipe::remote::MessageFlags::RequiresAck;
        auto encoded = netpipe::remote::encode_remote_message_v2(request_id, method_id, payload,
                                                                  netpipe::remote::MessageType::Request, flags);

        auto decode_res = netpipe::remote::decode_remote_message_v2(encoded);
        REQUIRE(decode_res.is_ok());

        auto decoded = decode_res.value();
        CHECK(decoded.flags == flags);
        CHECK((decoded.flags & netpipe::remote::MessageFlags::Compressed) != 0);
        CHECK((decoded.flags & netpipe::remote::MessageFlags::RequiresAck) != 0);
        CHECK((decoded.flags & netpipe::remote::MessageFlags::Streaming) == 0);
    }

    SUBCASE("Auto-detect V2 protocol") {
        dp::u32 request_id = 300;
        dp::u32 method_id = 15;
        netpipe::Message payload = {0xFF, 0xEE};

        auto encoded = netpipe::remote::encode_remote_message_v2(request_id, method_id, payload);

        auto decode_res = netpipe::remote::decode_remote_message_auto(encoded);
        REQUIRE(decode_res.is_ok());

        auto decoded = decode_res.value();
        CHECK(decoded.version == netpipe::remote::PROTOCOL_VERSION_2);
        CHECK(decoded.request_id == request_id);
        CHECK(decoded.method_id == method_id);
    }

    SUBCASE("Auto-detect V1 protocol (backward compatibility)") {
        dp::u32 request_id = 400;
        netpipe::Message payload = {0x11, 0x22, 0x33};

        // Encode with V1 protocol
        auto encoded = netpipe::remote::encode_remote_message(request_id, payload, false);

        // Auto-detect should recognize V1 and convert to V2 format
        auto decode_res = netpipe::remote::decode_remote_message_auto(encoded);
        REQUIRE(decode_res.is_ok());

        auto decoded = decode_res.value();
        CHECK(decoded.version == netpipe::remote::PROTOCOL_VERSION_1);
        CHECK(decoded.type == netpipe::remote::MessageType::Response);
        CHECK(decoded.request_id == request_id);
        CHECK(decoded.method_id == 0); // V1 doesn't have method_id
        CHECK(decoded.payload.size() == payload.size());
    }

    SUBCASE("V2 empty payload") {
        dp::u32 request_id = 500;
        dp::u32 method_id = 20;
        netpipe::Message empty_payload;

        auto encoded = netpipe::remote::encode_remote_message_v2(request_id, method_id, empty_payload);
        CHECK(encoded.size() == 16); // Just header, no payload

        auto decode_res = netpipe::remote::decode_remote_message_v2(encoded);
        REQUIRE(decode_res.is_ok());

        auto decoded = decode_res.value();
        CHECK(decoded.request_id == request_id);
        CHECK(decoded.method_id == method_id);
        CHECK(decoded.payload.size() == 0);
    }

    SUBCASE("V2 large method_id") {
        dp::u32 request_id = 600;
        dp::u32 method_id = 0xFFFFFFFF; // Max u32
        netpipe::Message payload = {0xAB};

        auto encoded = netpipe::remote::encode_remote_message_v2(request_id, method_id, payload);
        auto decode_res = netpipe::remote::decode_remote_message_v2(encoded);
        REQUIRE(decode_res.is_ok());

        CHECK(decode_res.value().method_id == method_id);
    }
}

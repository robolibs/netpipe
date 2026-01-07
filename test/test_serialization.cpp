#include <doctest/doctest.h>
#include <netpipe/netpipe.hpp>

TEST_CASE("Serialization - Basic types") {
    SUBCASE("Integral types") {
        // u32
        dp::u32 val_u32 = 12345;
        auto msg_u32 = netpipe::Serializer<dp::u32>::serialize(val_u32);
        auto res_u32 = netpipe::Serializer<dp::u32>::deserialize(msg_u32);
        REQUIRE(res_u32.is_ok());
        CHECK(res_u32.value() == val_u32);

        // i64
        dp::i64 val_i64 = -9876543210;
        auto msg_i64 = netpipe::Serializer<dp::i64>::serialize(val_i64);
        auto res_i64 = netpipe::Serializer<dp::i64>::deserialize(msg_i64);
        REQUIRE(res_i64.is_ok());
        CHECK(res_i64.value() == val_i64);

        // u8
        dp::u8 val_u8 = 255;
        auto msg_u8 = netpipe::Serializer<dp::u8>::serialize(val_u8);
        auto res_u8 = netpipe::Serializer<dp::u8>::deserialize(msg_u8);
        REQUIRE(res_u8.is_ok());
        CHECK(res_u8.value() == val_u8);
    }

    SUBCASE("Floating point types") {
        // float
        float val_f = 3.14159f;
        auto msg_f = netpipe::Serializer<float>::serialize(val_f);
        auto res_f = netpipe::Serializer<float>::deserialize(msg_f);
        REQUIRE(res_f.is_ok());
        CHECK(res_f.value() == val_f);

        // double
        double val_d = 2.71828182845904523536;
        auto msg_d = netpipe::Serializer<double>::serialize(val_d);
        auto res_d = netpipe::Serializer<double>::deserialize(msg_d);
        REQUIRE(res_d.is_ok());
        CHECK(res_d.value() == val_d);
    }

    SUBCASE("Boolean") {
        bool val_true = true;
        auto msg_true = netpipe::Serializer<bool>::serialize(val_true);
        auto res_true = netpipe::Serializer<bool>::deserialize(msg_true);
        REQUIRE(res_true.is_ok());
        CHECK(res_true.value() == true);

        bool val_false = false;
        auto msg_false = netpipe::Serializer<bool>::serialize(val_false);
        auto res_false = netpipe::Serializer<bool>::deserialize(msg_false);
        REQUIRE(res_false.is_ok());
        CHECK(res_false.value() == false);
    }

    SUBCASE("String types") {
        // dp::String
        dp::String val_dp = "Hello, World!";
        auto msg_dp = netpipe::Serializer<dp::String>::serialize(val_dp);
        auto res_dp = netpipe::Serializer<dp::String>::deserialize(msg_dp);
        REQUIRE(res_dp.is_ok());
        CHECK(res_dp.value() == val_dp);

        // std::string
        std::string val_std = "Test String";
        auto msg_std = netpipe::Serializer<std::string>::serialize(val_std);
        auto res_std = netpipe::Serializer<std::string>::deserialize(msg_std);
        REQUIRE(res_std.is_ok());
        CHECK(res_std.value() == val_std);
    }
}

TEST_CASE("Serialization - Error handling") {
    SUBCASE("Size mismatch for integral types") {
        netpipe::Message wrong_size = {0x01, 0x02}; // 2 bytes, but u32 needs 4
        auto res = netpipe::Serializer<dp::u32>::deserialize(wrong_size);
        CHECK(res.is_err());
    }

    SUBCASE("Size mismatch for bool") {
        netpipe::Message wrong_size = {0x01, 0x02}; // 2 bytes, but bool needs 1
        auto res = netpipe::Serializer<bool>::deserialize(wrong_size);
        CHECK(res.is_err());
    }
}

TEST_CASE("TypedRemote - Type-safe calls") {
    SUBCASE("Type-safe call with integers") {
        netpipe::TcpStream server;
        netpipe::TcpEndpoint endpoint{"127.0.0.1", 18020};

        auto listen_res = server.listen(endpoint);
        REQUIRE(listen_res.is_ok());

        // Server thread
        std::thread server_thread([&]() {
            auto accept_res = server.accept();
            if (accept_res.is_ok()) {
                auto client_stream = std::move(accept_res.value());
                netpipe::RemoteRouter router(*client_stream);
                auto typed_router = netpipe::make_typed(router);

                // Register handler: takes u32, returns u32 (doubles the input)
                typed_router.register_method<dp::u32, dp::u32>(1, [](const dp::u32 &req) -> dp::Res<dp::u32> {
                    return dp::result::ok(req * 2);
                });

                // Manually serve one request
                auto recv_res = client_stream->recv();
                if (recv_res.is_ok()) {
                    auto decode_res = netpipe::remote::decode_remote_message_v2(recv_res.value());
                    if (decode_res.is_ok()) {
                        auto decoded = decode_res.value();
                        // Deserialize u32 request
                        auto req_res = netpipe::Serializer<dp::u32>::deserialize(decoded.payload);
                        if (req_res.is_ok()) {
                            dp::u32 result = req_res.value() * 2;
                            auto resp_msg = netpipe::Serializer<dp::u32>::serialize(result);
                            auto response = netpipe::remote::encode_remote_message_v2(
                                decoded.request_id, decoded.method_id, resp_msg,
                                netpipe::remote::MessageType::Response);
                            client_stream->send(response);
                        }
                    }
                }
            }
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        netpipe::TcpStream client;
        auto connect_res = client.connect(endpoint);
        REQUIRE(connect_res.is_ok());

        netpipe::RemoteRouter router(client);
        auto typed_client = netpipe::make_typed(router);

        // Type-safe call
        auto resp = typed_client.call<dp::u32, dp::u32>(1, 42, 1000);
        REQUIRE(resp.is_ok());
        CHECK(resp.value() == 84);

        client.close();
        server_thread.join();
        server.close();
    }

    SUBCASE("Type-safe call with strings") {
        netpipe::TcpStream server;
        netpipe::TcpEndpoint endpoint{"127.0.0.1", 18021};

        auto listen_res = server.listen(endpoint);
        REQUIRE(listen_res.is_ok());

        // Server thread
        std::thread server_thread([&]() {
            auto accept_res = server.accept();
            if (accept_res.is_ok()) {
                auto client_stream = std::move(accept_res.value());

                // Manually handle one request
                auto recv_res = client_stream->recv();
                if (recv_res.is_ok()) {
                    auto decode_res = netpipe::remote::decode_remote_message_v2(recv_res.value());
                    if (decode_res.is_ok()) {
                        auto decoded = decode_res.value();
                        // Deserialize string request
                        auto req_res = netpipe::Serializer<dp::String>::deserialize(decoded.payload);
                        if (req_res.is_ok()) {
                            dp::String result = dp::String("Echo: ") + req_res.value();
                            auto resp_msg = netpipe::Serializer<dp::String>::serialize(result);
                            auto response = netpipe::remote::encode_remote_message_v2(
                                decoded.request_id, decoded.method_id, resp_msg,
                                netpipe::remote::MessageType::Response);
                            client_stream->send(response);
                        }
                    }
                }
            }
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        netpipe::TcpStream client;
        auto connect_res = client.connect(endpoint);
        REQUIRE(connect_res.is_ok());

        netpipe::RemoteRouter router(client);
        auto typed_client = netpipe::make_typed(router);

        // Type-safe call with string
        dp::String request = "Hello";
        auto resp = typed_client.call<dp::String, dp::String>(1, request, 1000);
        REQUIRE(resp.is_ok());
        CHECK(resp.value() == "Echo: Hello");

        client.close();
        server_thread.join();
        server.close();
    }
}

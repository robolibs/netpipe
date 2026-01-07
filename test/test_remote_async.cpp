#include <atomic>
#include <chrono>
#include <doctest/doctest.h>
#include <netpipe/netpipe.hpp>
#include <thread>

TEST_CASE("RemoteAsync - Basic functionality") {
    SUBCASE("Single async call") {
        netpipe::TcpStream server;
        netpipe::TcpEndpoint endpoint{"127.0.0.1", 18011};

        auto listen_res = server.listen(endpoint);
        REQUIRE(listen_res.is_ok());

        // Server thread
        std::thread server_thread([&]() {
            auto accept_res = server.accept();
            if (accept_res.is_ok()) {
                auto client_stream = std::move(accept_res.value());

                // Read and respond to one request
                auto recv_res = client_stream->recv();
                if (recv_res.is_ok()) {
                    auto decode_res = netpipe::remote::decode_remote_message_v2(recv_res.value());
                    if (decode_res.is_ok()) {
                        auto decoded = decode_res.value();
                        auto response = netpipe::remote::encode_remote_message_v2(
                            decoded.request_id, decoded.method_id, decoded.payload,
                            netpipe::remote::MessageType::Response);
                        client_stream->send(response);
                    }
                }
            }
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        netpipe::TcpStream client;
        auto connect_res = client.connect(endpoint);
        REQUIRE(connect_res.is_ok());

        {
            netpipe::RemoteAsync async_client(client, 10);

            netpipe::Message req = {0x42};
            auto resp = async_client.call(1, req, 1000);
            REQUIRE(resp.is_ok());
            CHECK(resp.value()[0] == 0x42);
        } // RemoteAsync destructor called here

        server_thread.join();
        server.close();
    }

    SUBCASE("Sequential async calls") {
        netpipe::TcpStream server;
        netpipe::TcpEndpoint endpoint{"127.0.0.1", 18012};

        auto listen_res = server.listen(endpoint);
        REQUIRE(listen_res.is_ok());

        // Server thread
        std::thread server_thread([&]() {
            auto accept_res = server.accept();
            if (accept_res.is_ok()) {
                auto client_stream = std::move(accept_res.value());

                // Handle 3 requests sequentially
                for (int i = 0; i < 3; i++) {
                    auto recv_res = client_stream->recv();
                    if (recv_res.is_ok()) {
                        auto decode_res = netpipe::remote::decode_remote_message_v2(recv_res.value());
                        if (decode_res.is_ok()) {
                            auto decoded = decode_res.value();
                            auto response = netpipe::remote::encode_remote_message_v2(
                                decoded.request_id, decoded.method_id, decoded.payload,
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

        {
            netpipe::RemoteAsync async_client(client, 10);

            // Make 3 sequential calls
            for (int i = 0; i < 3; i++) {
                netpipe::Message req = {static_cast<dp::u8>(i)};
                auto resp = async_client.call(1, req, 1000);
                REQUIRE(resp.is_ok());
                CHECK(resp.value()[0] == static_cast<dp::u8>(i));
            }
        }

        server_thread.join();
        server.close();
    }

    SUBCASE("Pending count tracking") {
        netpipe::TcpStream server;
        netpipe::TcpEndpoint endpoint{"127.0.0.1", 18013};

        auto listen_res = server.listen(endpoint);
        REQUIRE(listen_res.is_ok());

        std::thread server_thread([&]() {
            auto accept_res = server.accept();
            if (accept_res.is_ok()) {
                auto client_stream = std::move(accept_res.value());

                // Read request, wait, then respond
                auto recv_res = client_stream->recv();
                std::this_thread::sleep_for(std::chrono::milliseconds(100));

                if (recv_res.is_ok()) {
                    auto decode_res = netpipe::remote::decode_remote_message_v2(recv_res.value());
                    if (decode_res.is_ok()) {
                        auto decoded = decode_res.value();
                        auto response = netpipe::remote::encode_remote_message_v2(
                            decoded.request_id, decoded.method_id, decoded.payload,
                            netpipe::remote::MessageType::Response);
                        client_stream->send(response);
                    }
                }
            }
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        netpipe::TcpStream client;
        auto connect_res = client.connect(endpoint);
        REQUIRE(connect_res.is_ok());

        {
            netpipe::RemoteAsync async_client(client);

            // Start a call in background
            std::atomic<bool> call_done{false};
            std::thread call_thread([&]() {
                netpipe::Message req = {0x01};
                async_client.call(1, req, 1000);
                call_done = true;
            });

            // Check pending count
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            CHECK(async_client.pending_count() == 1);

            call_thread.join();

            // Should be 0 after completion
            CHECK(call_done.load());
            CHECK(async_client.pending_count() == 0);
        }

        server_thread.join();
        server.close();
    }
}

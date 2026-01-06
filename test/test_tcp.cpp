#include <chrono>
#include <doctest/doctest.h>
#include <netpipe/stream/tcp.hpp>
#include <thread>

TEST_CASE("TcpStream - Basic connection") {
    SUBCASE("Server listen and client connect") {
        netpipe::TcpStream server;
        netpipe::TcpEndpoint endpoint{"127.0.0.1", 19001};

        // Server listens
        auto listen_res = server.listen(endpoint);
        REQUIRE(listen_res.is_ok());

        // Client connects in separate thread
        std::thread client_thread([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            netpipe::TcpStream client;
            auto connect_res = client.connect(endpoint);
            CHECK(connect_res.is_ok());
            CHECK(client.is_connected());
            client.close();
        });

        // Server accepts
        auto accept_res = server.accept();
        REQUIRE(accept_res.is_ok());

        auto client_stream = std::move(accept_res.value());
        CHECK(client_stream->is_connected());

        client_thread.join();
        server.close();
    }

    SUBCASE("Connect to non-existent server fails") {
        netpipe::TcpStream client;
        netpipe::TcpEndpoint endpoint{"127.0.0.1", 19999};

        auto connect_res = client.connect(endpoint);
        CHECK(connect_res.is_err());
        CHECK_FALSE(client.is_connected());
    }

    SUBCASE("Accept without listen fails") {
        netpipe::TcpStream server;
        auto accept_res = server.accept();
        CHECK(accept_res.is_err());
    }
}

TEST_CASE("TcpStream - Message exchange") {
    SUBCASE("Send and receive small message") {
        netpipe::TcpStream server;
        netpipe::TcpEndpoint endpoint{"127.0.0.1", 19002};

        auto listen_res = server.listen(endpoint);
        REQUIRE(listen_res.is_ok());

        std::thread client_thread([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            netpipe::TcpStream client;
            auto connect_res = client.connect(endpoint);
            REQUIRE(connect_res.is_ok());

            // Send message
            netpipe::Message msg = {0x01, 0x02, 0x03, 0x04, 0x05};
            auto send_res = client.send(msg);
            CHECK(send_res.is_ok());

            // Receive response
            auto recv_res = client.recv();
            REQUIRE(recv_res.is_ok());
            auto response = std::move(recv_res.value());
            CHECK(response.size() == 3);
            CHECK(response[0] == 0xAA);
            CHECK(response[1] == 0xBB);
            CHECK(response[2] == 0xCC);

            client.close();
        });

        // Server accepts and receives
        auto accept_res = server.accept();
        REQUIRE(accept_res.is_ok());
        auto client_stream = std::move(accept_res.value());

        auto recv_res = client_stream->recv();
        REQUIRE(recv_res.is_ok());
        auto msg = std::move(recv_res.value());
        CHECK(msg.size() == 5);
        CHECK(msg[0] == 0x01);
        CHECK(msg[4] == 0x05);

        // Send response
        netpipe::Message response = {0xAA, 0xBB, 0xCC};
        auto send_res = client_stream->send(response);
        CHECK(send_res.is_ok());

        client_thread.join();
        server.close();
    }

    SUBCASE("Send and receive large message") {
        netpipe::TcpStream server;
        netpipe::TcpEndpoint endpoint{"127.0.0.1", 19003};

        auto listen_res = server.listen(endpoint);
        REQUIRE(listen_res.is_ok());

        std::thread client_thread([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            netpipe::TcpStream client;
            auto connect_res = client.connect(endpoint);
            REQUIRE(connect_res.is_ok());

            // Send large message (10KB)
            netpipe::Message msg(10000);
            for (size_t i = 0; i < msg.size(); i++) {
                msg[i] = static_cast<dp::u8>(i % 256);
            }
            auto send_res = client.send(msg);
            CHECK(send_res.is_ok());

            client.close();
        });

        // Server receives
        auto accept_res = server.accept();
        REQUIRE(accept_res.is_ok());
        auto client_stream = std::move(accept_res.value());

        auto recv_res = client_stream->recv();
        REQUIRE(recv_res.is_ok());
        auto msg = std::move(recv_res.value());
        CHECK(msg.size() == 10000);

        // Verify pattern
        for (size_t i = 0; i < msg.size(); i++) {
            CHECK(msg[i] == static_cast<dp::u8>(i % 256));
        }

        client_thread.join();
        server.close();
    }

    SUBCASE("Send empty message") {
        netpipe::TcpStream server;
        netpipe::TcpEndpoint endpoint{"127.0.0.1", 19004};

        auto listen_res = server.listen(endpoint);
        REQUIRE(listen_res.is_ok());

        std::thread client_thread([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            netpipe::TcpStream client;
            auto connect_res = client.connect(endpoint);
            REQUIRE(connect_res.is_ok());

            // Send empty message
            netpipe::Message msg;
            auto send_res = client.send(msg);
            CHECK(send_res.is_ok());

            client.close();
        });

        // Server receives
        auto accept_res = server.accept();
        REQUIRE(accept_res.is_ok());
        auto client_stream = std::move(accept_res.value());

        auto recv_res = client_stream->recv();
        REQUIRE(recv_res.is_ok());
        auto msg = std::move(recv_res.value());
        CHECK(msg.empty());

        client_thread.join();
        server.close();
    }
}

TEST_CASE("TcpStream - Multiple messages") {
    SUBCASE("Send multiple messages in sequence") {
        netpipe::TcpStream server;
        netpipe::TcpEndpoint endpoint{"127.0.0.1", 19005};

        auto listen_res = server.listen(endpoint);
        REQUIRE(listen_res.is_ok());

        std::thread client_thread([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            netpipe::TcpStream client;
            auto connect_res = client.connect(endpoint);
            REQUIRE(connect_res.is_ok());

            // Send multiple messages
            for (int i = 0; i < 5; i++) {
                netpipe::Message msg = {static_cast<dp::u8>(i), static_cast<dp::u8>(i + 1)};
                auto send_res = client.send(msg);
                CHECK(send_res.is_ok());
            }

            client.close();
        });

        // Server receives all messages
        auto accept_res = server.accept();
        REQUIRE(accept_res.is_ok());
        auto client_stream = std::move(accept_res.value());

        for (int i = 0; i < 5; i++) {
            auto recv_res = client_stream->recv();
            REQUIRE(recv_res.is_ok());
            auto msg = std::move(recv_res.value());
            CHECK(msg.size() == 2);
            CHECK(msg[0] == static_cast<dp::u8>(i));
            CHECK(msg[1] == static_cast<dp::u8>(i + 1));
        }

        client_thread.join();
        server.close();
    }
}

TEST_CASE("TcpStream - Error conditions") {
    SUBCASE("Send without connection fails") {
        netpipe::TcpStream stream;
        netpipe::Message msg = {0x01, 0x02};
        auto send_res = stream.send(msg);
        CHECK(send_res.is_err());
    }

    SUBCASE("Recv without connection fails") {
        netpipe::TcpStream stream;
        auto recv_res = stream.recv();
        CHECK(recv_res.is_err());
    }

    SUBCASE("Close and check connection state") {
        netpipe::TcpStream server;
        netpipe::TcpEndpoint endpoint{"127.0.0.1", 19006};

        auto listen_res = server.listen(endpoint);
        REQUIRE(listen_res.is_ok());

        std::thread client_thread([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            netpipe::TcpStream client;
            auto connect_res = client.connect(endpoint);
            REQUIRE(connect_res.is_ok());
            CHECK(client.is_connected());

            client.close();
            CHECK_FALSE(client.is_connected());
        });

        auto accept_res = server.accept();
        REQUIRE(accept_res.is_ok());

        client_thread.join();
        server.close();
    }
}

TEST_CASE("TcpStream - Bind to different addresses") {
    SUBCASE("Bind to 0.0.0.0") {
        netpipe::TcpStream server;
        netpipe::TcpEndpoint endpoint{"0.0.0.0", 19007};

        auto listen_res = server.listen(endpoint);
        CHECK(listen_res.is_ok());

        server.close();
    }

    SUBCASE("Bind to 127.0.0.1") {
        netpipe::TcpStream server;
        netpipe::TcpEndpoint endpoint{"127.0.0.1", 19008};

        auto listen_res = server.listen(endpoint);
        CHECK(listen_res.is_ok());

        server.close();
    }
}

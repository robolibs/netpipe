#include <chrono>
#include <doctest/doctest.h>
#include <netpipe/stream/ipc.hpp>
#include <thread>

TEST_CASE("IpcStream - Basic connection") {
    SUBCASE("Server listen and client connect") {
        netpipe::IpcStream server;
        netpipe::IpcEndpoint endpoint{"/tmp/netpipe_test_ipc_1.sock"};

        // Server listens
        auto listen_res = server.listen_ipc(endpoint);
        REQUIRE(listen_res.is_ok());

        // Client connects in separate thread
        std::thread client_thread([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            netpipe::IpcStream client;
            auto connect_res = client.connect_ipc(endpoint);
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

    SUBCASE("Connect to non-existent socket fails") {
        netpipe::IpcStream client;
        netpipe::IpcEndpoint endpoint{"/tmp/nonexistent_socket.sock"};

        auto connect_res = client.connect_ipc(endpoint);
        CHECK(connect_res.is_err());
        CHECK_FALSE(client.is_connected());
    }

    SUBCASE("Accept without listen fails") {
        netpipe::IpcStream server;
        auto accept_res = server.accept();
        CHECK(accept_res.is_err());
    }
}

TEST_CASE("IpcStream - Message exchange") {
    SUBCASE("Send and receive small message") {
        netpipe::IpcStream server;
        netpipe::IpcEndpoint endpoint{"/tmp/netpipe_test_ipc_2.sock"};

        auto listen_res = server.listen_ipc(endpoint);
        REQUIRE(listen_res.is_ok());

        std::thread client_thread([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            netpipe::IpcStream client;
            auto connect_res = client.connect_ipc(endpoint);
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
        netpipe::IpcStream server;
        netpipe::IpcEndpoint endpoint{"/tmp/netpipe_test_ipc_3.sock"};

        auto listen_res = server.listen_ipc(endpoint);
        REQUIRE(listen_res.is_ok());

        std::thread client_thread([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            netpipe::IpcStream client;
            auto connect_res = client.connect_ipc(endpoint);
            REQUIRE(connect_res.is_ok());

            // Send large message (100KB)
            netpipe::Message msg(100000);
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
        CHECK(msg.size() == 100000);

        // Verify pattern
        for (size_t i = 0; i < msg.size(); i++) {
            CHECK(msg[i] == static_cast<dp::u8>(i % 256));
        }

        client_thread.join();
        server.close();
    }

    SUBCASE("Send empty message") {
        netpipe::IpcStream server;
        netpipe::IpcEndpoint endpoint{"/tmp/netpipe_test_ipc_4.sock"};

        auto listen_res = server.listen_ipc(endpoint);
        REQUIRE(listen_res.is_ok());

        std::thread client_thread([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            netpipe::IpcStream client;
            auto connect_res = client.connect_ipc(endpoint);
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

TEST_CASE("IpcStream - Multiple messages") {
    SUBCASE("Send multiple messages in sequence") {
        netpipe::IpcStream server;
        netpipe::IpcEndpoint endpoint{"/tmp/netpipe_test_ipc_5.sock"};

        auto listen_res = server.listen_ipc(endpoint);
        REQUIRE(listen_res.is_ok());

        std::thread client_thread([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            netpipe::IpcStream client;
            auto connect_res = client.connect_ipc(endpoint);
            REQUIRE(connect_res.is_ok());

            // Send multiple messages
            for (int i = 0; i < 10; i++) {
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

        for (int i = 0; i < 10; i++) {
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

TEST_CASE("IpcStream - Error conditions") {
    SUBCASE("Send without connection fails") {
        netpipe::IpcStream stream;
        netpipe::Message msg = {0x01, 0x02};
        auto send_res = stream.send(msg);
        CHECK(send_res.is_err());
    }

    SUBCASE("Recv without connection fails") {
        netpipe::IpcStream stream;
        auto recv_res = stream.recv();
        CHECK(recv_res.is_err());
    }

    SUBCASE("Path too long fails") {
        netpipe::IpcStream server;
        // Create a very long path (> 108 chars for most systems)
        char long_path[300];
        memset(long_path, 'a', 299);
        long_path[0] = '/';
        long_path[1] = 't';
        long_path[2] = 'm';
        long_path[3] = 'p';
        long_path[4] = '/';
        long_path[299] = '\0';
        netpipe::IpcEndpoint endpoint{dp::String(long_path)};

        auto listen_res = server.listen_ipc(endpoint);
        CHECK(listen_res.is_err());
    }

    SUBCASE("Close and check connection state") {
        netpipe::IpcStream server;
        netpipe::IpcEndpoint endpoint{"/tmp/netpipe_test_ipc_6.sock"};

        auto listen_res = server.listen_ipc(endpoint);
        REQUIRE(listen_res.is_ok());

        std::thread client_thread([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            netpipe::IpcStream client;
            auto connect_res = client.connect_ipc(endpoint);
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

TEST_CASE("IpcStream - Socket cleanup") {
    SUBCASE("Socket file is cleaned up on close") {
        netpipe::IpcStream server;
        netpipe::IpcEndpoint endpoint{"/tmp/netpipe_test_ipc_cleanup.sock"};

        auto listen_res = server.listen_ipc(endpoint);
        REQUIRE(listen_res.is_ok());

        server.close();

        // Should be able to bind again after cleanup
        netpipe::IpcStream server2;
        auto listen_res2 = server2.listen_ipc(endpoint);
        CHECK(listen_res2.is_ok());

        server2.close();
    }

    SUBCASE("Reuse socket path after server closes") {
        {
            netpipe::IpcStream server;
            netpipe::IpcEndpoint endpoint{"/tmp/netpipe_test_ipc_reuse.sock"};
            auto listen_res = server.listen_ipc(endpoint);
            REQUIRE(listen_res.is_ok());
            // server goes out of scope and closes
        }

        // Should be able to create new server with same path
        netpipe::IpcStream server2;
        netpipe::IpcEndpoint endpoint{"/tmp/netpipe_test_ipc_reuse.sock"};
        auto listen_res2 = server2.listen_ipc(endpoint);
        CHECK(listen_res2.is_ok());

        server2.close();
    }
}

TEST_CASE("IpcStream - Bidirectional communication") {
    SUBCASE("Both sides send and receive") {
        netpipe::IpcStream server;
        netpipe::IpcEndpoint endpoint{"/tmp/netpipe_test_ipc_bidir.sock"};

        auto listen_res = server.listen_ipc(endpoint);
        REQUIRE(listen_res.is_ok());

        std::thread client_thread([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            netpipe::IpcStream client;
            auto connect_res = client.connect_ipc(endpoint);
            REQUIRE(connect_res.is_ok());

            // Client sends first
            netpipe::Message msg1 = {0x11, 0x22};
            auto send_res1 = client.send(msg1);
            CHECK(send_res1.is_ok());

            // Client receives
            auto recv_res = client.recv();
            REQUIRE(recv_res.is_ok());
            auto response = std::move(recv_res.value());
            CHECK(response.size() == 2);
            CHECK(response[0] == 0x33);
            CHECK(response[1] == 0x44);

            // Client sends again
            netpipe::Message msg2 = {0x55, 0x66};
            auto send_res2 = client.send(msg2);
            CHECK(send_res2.is_ok());

            client.close();
        });

        // Server accepts
        auto accept_res = server.accept();
        REQUIRE(accept_res.is_ok());
        auto client_stream = std::move(accept_res.value());

        // Server receives first message
        auto recv_res1 = client_stream->recv();
        REQUIRE(recv_res1.is_ok());
        auto msg1 = std::move(recv_res1.value());
        CHECK(msg1[0] == 0x11);
        CHECK(msg1[1] == 0x22);

        // Server sends response
        netpipe::Message response = {0x33, 0x44};
        auto send_res = client_stream->send(response);
        CHECK(send_res.is_ok());

        // Server receives second message
        auto recv_res2 = client_stream->recv();
        REQUIRE(recv_res2.is_ok());
        auto msg2 = std::move(recv_res2.value());
        CHECK(msg2[0] == 0x55);
        CHECK(msg2[1] == 0x66);

        client_thread.join();
        server.close();
    }
}

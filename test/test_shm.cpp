#include <chrono>
#include <doctest/doctest.h>
#include <netpipe/stream/shm.hpp>
#include <thread>

TEST_CASE("ShmStream - Basic connection") {
    SUBCASE("Listen and accept pattern") {
        netpipe::ShmStream listener;
        netpipe::ShmEndpoint endpoint{"netpipe_test_shm_1", 8192};

        // Server listens
        auto listen_res = listener.listen_shm(endpoint);
        REQUIRE(listen_res.is_ok());
        CHECK(listener.is_listening());

        std::unique_ptr<netpipe::Stream> server_conn;

        std::thread accept_thread([&]() {
            auto accept_res = listener.accept();
            REQUIRE(accept_res.is_ok());
            server_conn = std::move(accept_res.value());
            CHECK(server_conn->is_connected());
        });

        // Client connects
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        netpipe::ShmStream client;
        auto connect_res = client.connect_shm(endpoint);
        REQUIRE(connect_res.is_ok());
        CHECK(client.is_connected());

        accept_thread.join();

        client.close();
        server_conn->close();
        listener.close();
    }

    SUBCASE("Connect to non-existent shared memory fails") {
        netpipe::ShmStream connector;
        netpipe::ShmEndpoint endpoint{"nonexistent_shm", 4096};

        auto connect_res = connector.connect_shm(endpoint);
        CHECK(connect_res.is_err());
        CHECK_FALSE(connector.is_connected());
    }

    SUBCASE("Accept without listen fails") {
        netpipe::ShmStream stream;
        auto accept_res = stream.accept();
        CHECK(accept_res.is_err());
    }
}

TEST_CASE("ShmStream - Message exchange") {
    SUBCASE("Send and receive small message") {
        netpipe::ShmStream listener;
        netpipe::ShmEndpoint endpoint{"netpipe_test_shm_2", 8192};

        auto listen_res = listener.listen_shm(endpoint);
        REQUIRE(listen_res.is_ok());

        std::unique_ptr<netpipe::Stream> server_conn;

        std::thread server_thread([&]() {
            auto accept_res = listener.accept();
            REQUIRE(accept_res.is_ok());
            server_conn = std::move(accept_res.value());

            // Server sends message
            netpipe::Message msg = {0x01, 0x02, 0x03, 0x04, 0x05};
            auto send_res = server_conn->send(msg);
            CHECK(send_res.is_ok());
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        netpipe::ShmStream client;
        auto connect_res = client.connect_shm(endpoint);
        REQUIRE(connect_res.is_ok());

        server_thread.join();

        // Client receives message
        auto recv_res = client.recv();
        REQUIRE(recv_res.is_ok());
        auto msg = std::move(recv_res.value());
        CHECK(msg.size() == 5);
        CHECK(msg[0] == 0x01);
        CHECK(msg[4] == 0x05);

        client.close();
        server_conn->close();
        listener.close();
    }

    SUBCASE("Send and receive empty message") {
        netpipe::ShmStream listener;
        netpipe::ShmEndpoint endpoint{"netpipe_test_shm_3", 8192};

        auto listen_res = listener.listen_shm(endpoint);
        REQUIRE(listen_res.is_ok());

        std::unique_ptr<netpipe::Stream> server_conn;

        std::thread server_thread([&]() {
            auto accept_res = listener.accept();
            REQUIRE(accept_res.is_ok());
            server_conn = std::move(accept_res.value());

            netpipe::Message msg; // Empty
            auto send_res = server_conn->send(msg);
            CHECK(send_res.is_ok());
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        netpipe::ShmStream client;
        auto connect_res = client.connect_shm(endpoint);
        REQUIRE(connect_res.is_ok());

        server_thread.join();

        auto recv_res = client.recv();
        REQUIRE(recv_res.is_ok());
        auto msg = std::move(recv_res.value());
        CHECK(msg.empty());

        client.close();
        server_conn->close();
        listener.close();
    }

    SUBCASE("Send message larger than buffer fails") {
        netpipe::ShmStream listener;
        netpipe::ShmEndpoint endpoint{"netpipe_test_shm_4", 1024}; // Small buffer

        auto listen_res = listener.listen_shm(endpoint);
        REQUIRE(listen_res.is_ok());

        std::unique_ptr<netpipe::Stream> server_conn;

        std::thread server_thread([&]() {
            auto accept_res = listener.accept();
            REQUIRE(accept_res.is_ok());
            server_conn = std::move(accept_res.value());

            // Try to send message larger than buffer
            netpipe::Message msg(2000);
            auto send_res = server_conn->send(msg);
            CHECK(send_res.is_err());
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        netpipe::ShmStream client;
        auto connect_res = client.connect_shm(endpoint);
        REQUIRE(connect_res.is_ok());

        server_thread.join();

        client.close();
        server_conn->close();
        listener.close();
    }
}

TEST_CASE("ShmStream - Multiple messages") {
    SUBCASE("Send and receive single message sequence") {
        netpipe::ShmStream listener;
        netpipe::ShmEndpoint endpoint{"netpipe_test_shm_5", 16384};

        auto listen_res = listener.listen_shm(endpoint);
        REQUIRE(listen_res.is_ok());

        std::unique_ptr<netpipe::Stream> server_conn;

        std::thread server_thread([&]() {
            auto accept_res = listener.accept();
            REQUIRE(accept_res.is_ok());
            server_conn = std::move(accept_res.value());

            // Send multiple messages
            for (int i = 0; i < 3; i++) {
                netpipe::Message msg = {static_cast<dp::u8>(i), static_cast<dp::u8>(i + 1)};
                auto send_res = server_conn->send(msg);
                CHECK(send_res.is_ok());
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        netpipe::ShmStream client;
        auto connect_res = client.connect_shm(endpoint);
        REQUIRE(connect_res.is_ok());

        server_thread.join();

        // Receive messages
        for (int i = 0; i < 3; i++) {
            auto recv_res = client.recv();
            if (recv_res.is_ok()) {
                auto received = std::move(recv_res.value());
                CHECK(received.size() == 2);
                CHECK(received[0] == static_cast<dp::u8>(i));
                CHECK(received[1] == static_cast<dp::u8>(i + 1));
            }
        }

        client.close();
        server_conn->close();
        listener.close();
    }
}

TEST_CASE("ShmStream - Error conditions") {
    SUBCASE("Send without connection fails") {
        netpipe::ShmStream stream;
        netpipe::Message msg = {0x01, 0x02};
        auto send_res = stream.send(msg);
        CHECK(send_res.is_err());
    }

    SUBCASE("Recv without connection fails") {
        netpipe::ShmStream stream;
        auto recv_res = stream.recv();
        CHECK(recv_res.is_err());
    }

    SUBCASE("Recv when no data available times out") {
        netpipe::ShmStream listener;
        netpipe::ShmEndpoint endpoint{"netpipe_test_shm_6", 8192};

        auto listen_res = listener.listen_shm(endpoint);
        REQUIRE(listen_res.is_ok());

        std::unique_ptr<netpipe::Stream> server_conn;

        std::thread server_thread([&]() {
            auto accept_res = listener.accept();
            REQUIRE(accept_res.is_ok());
            server_conn = std::move(accept_res.value());
            // Don't send anything
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        netpipe::ShmStream client;
        auto connect_res = client.connect_shm(endpoint);
        REQUIRE(connect_res.is_ok());

        // Set a short timeout so recv fails quickly when no data available
        client.set_recv_timeout(100);

        // Try to receive when nothing was sent
        auto recv_res = client.recv();
        CHECK(recv_res.is_err());

        server_thread.join();

        client.close();
        server_conn->close();
        listener.close();
    }

    SUBCASE("Close and check connection state") {
        netpipe::ShmStream listener;
        netpipe::ShmEndpoint endpoint{"netpipe_test_shm_7", 8192};

        auto listen_res = listener.listen_shm(endpoint);
        REQUIRE(listen_res.is_ok());
        CHECK(listener.is_listening());

        listener.close();
        CHECK_FALSE(listener.is_listening());
    }
}

TEST_CASE("ShmStream - Different buffer sizes") {
    SUBCASE("Small buffer (1KB)") {
        netpipe::ShmStream listener;
        netpipe::ShmEndpoint endpoint{"netpipe_test_shm_small", 1024};

        auto listen_res = listener.listen_shm(endpoint);
        CHECK(listen_res.is_ok());

        listener.close();
    }

    SUBCASE("Medium buffer (64KB)") {
        netpipe::ShmStream listener;
        netpipe::ShmEndpoint endpoint{"netpipe_test_shm_medium", 65536};

        auto listen_res = listener.listen_shm(endpoint);
        CHECK(listen_res.is_ok());

        listener.close();
    }

    SUBCASE("Large buffer (1MB)") {
        netpipe::ShmStream listener;
        netpipe::ShmEndpoint endpoint{"netpipe_test_shm_large", 1048576};

        auto listen_res = listener.listen_shm(endpoint);
        CHECK(listen_res.is_ok());

        listener.close();
    }
}

TEST_CASE("ShmStream - Cleanup") {
    SUBCASE("Shared memory is cleaned up on close") {
        netpipe::ShmStream listener;
        netpipe::ShmEndpoint endpoint{"netpipe_test_shm_cleanup", 8192};

        auto listen_res = listener.listen_shm(endpoint);
        REQUIRE(listen_res.is_ok());

        listener.close();

        // Should be able to create again after cleanup
        netpipe::ShmStream listener2;
        auto listen_res2 = listener2.listen_shm(endpoint);
        CHECK(listen_res2.is_ok());

        listener2.close();
    }

    SUBCASE("Reuse shared memory name after listener closes") {
        {
            netpipe::ShmStream listener;
            netpipe::ShmEndpoint endpoint{"netpipe_test_shm_reuse", 8192};
            auto listen_res = listener.listen_shm(endpoint);
            REQUIRE(listen_res.is_ok());
            // listener goes out of scope and closes
        }

        // Should be able to create new shared memory with same name
        netpipe::ShmStream listener2;
        netpipe::ShmEndpoint endpoint{"netpipe_test_shm_reuse", 8192};
        auto listen_res2 = listener2.listen_shm(endpoint);
        CHECK(listen_res2.is_ok());

        listener2.close();
    }
}

TEST_CASE("ShmStream - Bidirectional communication") {
    SUBCASE("Both sides send and receive") {
        netpipe::ShmStream listener;
        netpipe::ShmEndpoint endpoint{"netpipe_test_shm_bidir", 16384};

        auto listen_res = listener.listen_shm(endpoint);
        REQUIRE(listen_res.is_ok());

        std::unique_ptr<netpipe::Stream> server_conn;

        std::thread server_thread([&]() {
            auto accept_res = listener.accept();
            REQUIRE(accept_res.is_ok());
            server_conn = std::move(accept_res.value());

            // Server sends
            netpipe::Message msg1 = {0xAA, 0xAA};
            server_conn->send(msg1);

            // Server receives
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            auto recv_res = server_conn->recv();
            if (recv_res.is_ok()) {
                auto msg = std::move(recv_res.value());
                CHECK(msg.size() == 2);
                CHECK(msg[0] == 0xBB);
            }
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        netpipe::ShmStream client;
        auto connect_res = client.connect_shm(endpoint);
        REQUIRE(connect_res.is_ok());

        // Client receives
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto recv_res = client.recv();
        if (recv_res.is_ok()) {
            auto msg = std::move(recv_res.value());
            CHECK(msg.size() == 2);
            CHECK(msg[0] == 0xAA);
        }

        // Client sends
        netpipe::Message msg2 = {0xBB, 0xBB};
        client.send(msg2);

        server_thread.join();

        client.close();
        server_conn->close();
        listener.close();
    }
}

TEST_CASE("ShmStream - Multiple connections") {
    SUBCASE("Accept multiple clients") {
        netpipe::ShmStream listener;
        netpipe::ShmEndpoint endpoint{"netpipe_test_shm_multi", 8192};

        auto listen_res = listener.listen_shm(endpoint);
        REQUIRE(listen_res.is_ok());

        std::vector<std::unique_ptr<netpipe::Stream>> server_conns;
        std::atomic<int> accepted{0};

        std::thread accept_thread([&]() {
            for (int i = 0; i < 3; i++) {
                auto accept_res = listener.accept();
                if (accept_res.is_ok()) {
                    server_conns.push_back(std::move(accept_res.value()));
                    accepted++;
                }
            }
        });

        // Connect 3 clients
        std::vector<netpipe::ShmStream> clients(3);
        for (int i = 0; i < 3; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            auto connect_res = clients[i].connect_shm(endpoint);
            CHECK(connect_res.is_ok());
        }

        accept_thread.join();
        CHECK(accepted == 3);
        CHECK(server_conns.size() == 3);

        for (auto& client : clients) {
            client.close();
        }
        for (auto& conn : server_conns) {
            conn->close();
        }
        listener.close();
    }
}

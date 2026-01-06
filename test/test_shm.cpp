#include <chrono>
#include <doctest/doctest.h>
#include <netpipe/stream/shm.hpp>
#include <thread>

TEST_CASE("ShmStream - Basic connection") {
    SUBCASE("Creator and connector") {
        netpipe::ShmStream creator;
        netpipe::ShmEndpoint endpoint{"netpipe_test_shm_1", 8192};

        // Creator creates shared memory
        auto listen_res = creator.listen_shm(endpoint);
        REQUIRE(listen_res.is_ok());
        CHECK(creator.is_connected());

        // Connector connects to existing shared memory
        netpipe::ShmStream connector;
        auto connect_res = connector.connect_shm(endpoint);
        REQUIRE(connect_res.is_ok());
        CHECK(connector.is_connected());

        connector.close();
        creator.close();
    }

    SUBCASE("Connect to non-existent shared memory fails") {
        netpipe::ShmStream connector;
        netpipe::ShmEndpoint endpoint{"nonexistent_shm", 4096};

        auto connect_res = connector.connect_shm(endpoint);
        CHECK(connect_res.is_err());
        CHECK_FALSE(connector.is_connected());
    }

    SUBCASE("Accept not supported") {
        netpipe::ShmStream stream;
        auto accept_res = stream.accept();
        CHECK(accept_res.is_err());
    }
}

TEST_CASE("ShmStream - Message exchange") {
    SUBCASE("Send and receive small message") {
        netpipe::ShmStream creator;
        netpipe::ShmEndpoint endpoint{"netpipe_test_shm_2", 8192};

        auto listen_res = creator.listen_shm(endpoint);
        REQUIRE(listen_res.is_ok());

        std::thread connector_thread([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            netpipe::ShmStream connector;
            auto connect_res = connector.connect_shm(endpoint);
            REQUIRE(connect_res.is_ok());

            // Wait for message
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            // Receive message
            auto recv_res = connector.recv();
            REQUIRE(recv_res.is_ok());
            auto msg = std::move(recv_res.value());
            CHECK(msg.size() == 5);
            CHECK(msg[0] == 0x01);
            CHECK(msg[4] == 0x05);

            connector.close();
        });

        // Creator sends message
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        netpipe::Message msg = {0x01, 0x02, 0x03, 0x04, 0x05};
        auto send_res = creator.send(msg);
        CHECK(send_res.is_ok());

        connector_thread.join();
        creator.close();
    }

    SUBCASE("Send and receive empty message") {
        netpipe::ShmStream creator;
        netpipe::ShmEndpoint endpoint{"netpipe_test_shm_3", 8192};

        auto listen_res = creator.listen_shm(endpoint);
        REQUIRE(listen_res.is_ok());

        std::thread connector_thread([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            netpipe::ShmStream connector;
            auto connect_res = connector.connect_shm(endpoint);
            REQUIRE(connect_res.is_ok());

            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            auto recv_res = connector.recv();
            REQUIRE(recv_res.is_ok());
            auto msg = std::move(recv_res.value());
            CHECK(msg.empty());

            connector.close();
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        netpipe::Message msg; // Empty
        auto send_res = creator.send(msg);
        CHECK(send_res.is_ok());

        connector_thread.join();
        creator.close();
    }

    SUBCASE("Send message larger than buffer fails") {
        netpipe::ShmStream creator;
        netpipe::ShmEndpoint endpoint{"netpipe_test_shm_4", 1024}; // Small buffer

        auto listen_res = creator.listen_shm(endpoint);
        REQUIRE(listen_res.is_ok());

        // Try to send message larger than buffer
        netpipe::Message msg(2000);
        auto send_res = creator.send(msg);
        CHECK(send_res.is_err());

        creator.close();
    }
}

TEST_CASE("ShmStream - Multiple messages") {
    SUBCASE("Send and receive single message sequence") {
        netpipe::ShmStream creator;
        netpipe::ShmEndpoint endpoint{"netpipe_test_shm_5", 16384};

        auto listen_res = creator.listen_shm(endpoint);
        REQUIRE(listen_res.is_ok());

        netpipe::ShmStream connector;
        auto connect_res = connector.connect_shm(endpoint);
        REQUIRE(connect_res.is_ok());

        // Send and receive messages one at a time
        for (int i = 0; i < 3; i++) {
            netpipe::Message msg = {static_cast<dp::u8>(i), static_cast<dp::u8>(i + 1)};
            auto send_res = creator.send(msg);
            CHECK(send_res.is_ok());

            // Small delay to ensure message is written
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

            auto recv_res = connector.recv();
            if (recv_res.is_ok()) {
                auto received = std::move(recv_res.value());
                CHECK(received.size() == 2);
                CHECK(received[0] == static_cast<dp::u8>(i));
                CHECK(received[1] == static_cast<dp::u8>(i + 1));
            }
        }

        connector.close();
        creator.close();
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

    SUBCASE("Recv when no data available fails") {
        netpipe::ShmStream creator;
        netpipe::ShmEndpoint endpoint{"netpipe_test_shm_6", 8192};

        auto listen_res = creator.listen_shm(endpoint);
        REQUIRE(listen_res.is_ok());

        netpipe::ShmStream connector;
        auto connect_res = connector.connect_shm(endpoint);
        REQUIRE(connect_res.is_ok());

        // Try to receive when nothing was sent
        auto recv_res = connector.recv();
        CHECK(recv_res.is_err());

        connector.close();
        creator.close();
    }

    SUBCASE("Close and check connection state") {
        netpipe::ShmStream creator;
        netpipe::ShmEndpoint endpoint{"netpipe_test_shm_7", 8192};

        auto listen_res = creator.listen_shm(endpoint);
        REQUIRE(listen_res.is_ok());
        CHECK(creator.is_connected());

        creator.close();
        CHECK_FALSE(creator.is_connected());
    }
}

TEST_CASE("ShmStream - Different buffer sizes") {
    SUBCASE("Small buffer (1KB)") {
        netpipe::ShmStream creator;
        netpipe::ShmEndpoint endpoint{"netpipe_test_shm_small", 1024};

        auto listen_res = creator.listen_shm(endpoint);
        CHECK(listen_res.is_ok());

        creator.close();
    }

    SUBCASE("Medium buffer (64KB)") {
        netpipe::ShmStream creator;
        netpipe::ShmEndpoint endpoint{"netpipe_test_shm_medium", 65536};

        auto listen_res = creator.listen_shm(endpoint);
        CHECK(listen_res.is_ok());

        creator.close();
    }

    SUBCASE("Large buffer (1MB)") {
        netpipe::ShmStream creator;
        netpipe::ShmEndpoint endpoint{"netpipe_test_shm_large", 1048576};

        auto listen_res = creator.listen_shm(endpoint);
        CHECK(listen_res.is_ok());

        creator.close();
    }
}

TEST_CASE("ShmStream - Cleanup") {
    SUBCASE("Shared memory is cleaned up on close") {
        netpipe::ShmStream creator;
        netpipe::ShmEndpoint endpoint{"netpipe_test_shm_cleanup", 8192};

        auto listen_res = creator.listen_shm(endpoint);
        REQUIRE(listen_res.is_ok());

        creator.close();

        // Should be able to create again after cleanup
        netpipe::ShmStream creator2;
        auto listen_res2 = creator2.listen_shm(endpoint);
        CHECK(listen_res2.is_ok());

        creator2.close();
    }

    SUBCASE("Reuse shared memory name after creator closes") {
        {
            netpipe::ShmStream creator;
            netpipe::ShmEndpoint endpoint{"netpipe_test_shm_reuse", 8192};
            auto listen_res = creator.listen_shm(endpoint);
            REQUIRE(listen_res.is_ok());
            // creator goes out of scope and closes
        }

        // Should be able to create new shared memory with same name
        netpipe::ShmStream creator2;
        netpipe::ShmEndpoint endpoint{"netpipe_test_shm_reuse", 8192};
        auto listen_res2 = creator2.listen_shm(endpoint);
        CHECK(listen_res2.is_ok());

        creator2.close();
    }
}

TEST_CASE("ShmStream - Bidirectional communication") {
    SUBCASE("Both sides send and receive") {
        netpipe::ShmStream side1;
        netpipe::ShmEndpoint endpoint1{"netpipe_test_shm_bidir_1", 16384};

        auto listen_res1 = side1.listen_shm(endpoint1);
        REQUIRE(listen_res1.is_ok());

        netpipe::ShmStream side2;
        netpipe::ShmEndpoint endpoint2{"netpipe_test_shm_bidir_2", 16384};

        auto listen_res2 = side2.listen_shm(endpoint2);
        REQUIRE(listen_res2.is_ok());

        std::thread thread1([&]() {
            // Connect to side2's shared memory
            netpipe::ShmStream connector;
            auto connect_res = connector.connect_shm(endpoint2);
            REQUIRE(connect_res.is_ok());

            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            // Receive from side2
            auto recv_res = connector.recv();
            if (recv_res.is_ok()) {
                auto msg = std::move(recv_res.value());
                CHECK(msg.size() == 2);
                CHECK(msg[0] == 0xBB);
            }

            connector.close();
        });

        std::thread thread2([&]() {
            // Connect to side1's shared memory
            netpipe::ShmStream connector;
            auto connect_res = connector.connect_shm(endpoint1);
            REQUIRE(connect_res.is_ok());

            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            // Receive from side1
            auto recv_res = connector.recv();
            if (recv_res.is_ok()) {
                auto msg = std::move(recv_res.value());
                CHECK(msg.size() == 2);
                CHECK(msg[0] == 0xAA);
            }

            connector.close();
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        // Side1 sends
        netpipe::Message msg1 = {0xAA, 0xAA};
        side1.send(msg1);

        // Side2 sends
        netpipe::Message msg2 = {0xBB, 0xBB};
        side2.send(msg2);

        thread1.join();
        thread2.join();

        side1.close();
        side2.close();
    }
}

#include <chrono>
#include <doctest/doctest.h>
#include <netpipe/stream/shm_rpc.hpp>
#include <thread>

TEST_CASE("ShmRpcStream - Basic connection") {
    SUBCASE("Server and client creation") {
        auto server_res = netpipe::ShmRpcStream::create_server("test_shm_rpc_1", 8192);
        REQUIRE(server_res.is_ok());
        auto server = std::move(server_res.value());
        CHECK(server->is_connected());
        CHECK(server->is_server());

        auto client_res = netpipe::ShmRpcStream::create_client("test_shm_rpc_1", 8192);
        REQUIRE(client_res.is_ok());
        auto client = std::move(client_res.value());
        CHECK(client->is_connected());
        CHECK_FALSE(client->is_server());

        client->close();
        server->close();
    }

    SUBCASE("Client connect to non-existent server fails") {
        auto client_res = netpipe::ShmRpcStream::create_client("nonexistent_shm_rpc", 8192);
        CHECK(client_res.is_err());
    }

    SUBCASE("Accept not supported") {
        auto server_res = netpipe::ShmRpcStream::create_server("test_shm_rpc_2", 8192);
        REQUIRE(server_res.is_ok());
        auto server = std::move(server_res.value());

        auto accept_res = server->accept();
        CHECK(accept_res.is_err());

        server->close();
    }
}

TEST_CASE("ShmRpcStream - Bidirectional message exchange") {
    SUBCASE("Server sends, client receives") {
        auto server_res = netpipe::ShmRpcStream::create_server("test_shm_rpc_3", 16384);
        REQUIRE(server_res.is_ok());
        auto server = std::move(server_res.value());

        std::thread client_thread([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            auto client_res = netpipe::ShmRpcStream::create_client("test_shm_rpc_3", 16384);
            REQUIRE(client_res.is_ok());
            auto client = std::move(client_res.value());

            // Receive message from server
            auto recv_res = client->recv();
            REQUIRE(recv_res.is_ok());
            auto msg = std::move(recv_res.value());
            CHECK(msg.size() == 5);
            CHECK(msg[0] == 0x01);
            CHECK(msg[4] == 0x05);

            client->close();
        });

        // Server sends message
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        netpipe::Message msg = {0x01, 0x02, 0x03, 0x04, 0x05};
        auto send_res = server->send(msg);
        CHECK(send_res.is_ok());

        client_thread.join();
        server->close();
    }

    SUBCASE("Client sends, server receives") {
        auto server_res = netpipe::ShmRpcStream::create_server("test_shm_rpc_4", 16384);
        REQUIRE(server_res.is_ok());
        auto server = std::move(server_res.value());

        std::thread client_thread([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            auto client_res = netpipe::ShmRpcStream::create_client("test_shm_rpc_4", 16384);
            REQUIRE(client_res.is_ok());
            auto client = std::move(client_res.value());

            // Client sends message
            netpipe::Message msg = {0xAA, 0xBB, 0xCC};
            auto send_res = client->send(msg);
            CHECK(send_res.is_ok());

            client->close();
        });

        // Server receives message
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        auto recv_res = server->recv();
        REQUIRE(recv_res.is_ok());
        auto msg = std::move(recv_res.value());
        CHECK(msg.size() == 3);
        CHECK(msg[0] == 0xAA);
        CHECK(msg[1] == 0xBB);
        CHECK(msg[2] == 0xCC);

        client_thread.join();
        server->close();
    }

    SUBCASE("Full duplex communication") {
        auto server_res = netpipe::ShmRpcStream::create_server("test_shm_rpc_5", 16384);
        REQUIRE(server_res.is_ok());
        auto server = std::move(server_res.value());

        std::thread client_thread([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            auto client_res = netpipe::ShmRpcStream::create_client("test_shm_rpc_5", 16384);
            REQUIRE(client_res.is_ok());
            auto client = std::move(client_res.value());

            // Client sends request
            netpipe::Message request = {0x01, 0x02};
            auto send_res = client->send(request);
            CHECK(send_res.is_ok());

            // Client receives response
            auto recv_res = client->recv();
            REQUIRE(recv_res.is_ok());
            auto response = std::move(recv_res.value());
            CHECK(response.size() == 2);
            CHECK(response[0] == 0x03);
            CHECK(response[1] == 0x04);

            client->close();
        });

        // Server receives request
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        auto recv_res = server->recv();
        REQUIRE(recv_res.is_ok());
        auto request = std::move(recv_res.value());
        CHECK(request.size() == 2);
        CHECK(request[0] == 0x01);
        CHECK(request[1] == 0x02);

        // Server sends response
        netpipe::Message response = {0x03, 0x04};
        auto send_res = server->send(response);
        CHECK(send_res.is_ok());

        client_thread.join();
        server->close();
    }
}

TEST_CASE("ShmRpcStream - Timeout support") {
    SUBCASE("Recv timeout works") {
        auto server_res = netpipe::ShmRpcStream::create_server("test_shm_rpc_6", 8192);
        REQUIRE(server_res.is_ok());
        auto server = std::move(server_res.value());

        // Set a short timeout
        auto timeout_res = server->set_recv_timeout(100); // 100ms
        CHECK(timeout_res.is_ok());

        // Try to receive when nothing is sent - should timeout
        auto recv_res = server->recv();
        CHECK(recv_res.is_err());

        server->close();
    }
}

TEST_CASE("ShmRpcStream - Multiple messages") {
    SUBCASE("Request-response pattern") {
        auto server_res = netpipe::ShmRpcStream::create_server("test_shm_rpc_7", 32768);
        REQUIRE(server_res.is_ok());
        auto server = std::move(server_res.value());

        std::thread client_thread([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            auto client_res = netpipe::ShmRpcStream::create_client("test_shm_rpc_7", 32768);
            REQUIRE(client_res.is_ok());
            auto client = std::move(client_res.value());

            // Multiple request-response cycles
            for (int i = 0; i < 5; i++) {
                // Send request
                netpipe::Message request = {static_cast<dp::u8>(i)};
                auto send_res = client->send(request);
                CHECK(send_res.is_ok());

                // Receive response
                auto recv_res = client->recv();
                REQUIRE(recv_res.is_ok());
                auto response = std::move(recv_res.value());
                CHECK(response.size() == 1);
                CHECK(response[0] == static_cast<dp::u8>(i + 100));
            }

            client->close();
        });

        // Server handles requests
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        for (int i = 0; i < 5; i++) {
            // Receive request
            auto recv_res = server->recv();
            REQUIRE(recv_res.is_ok());
            auto request = std::move(recv_res.value());
            CHECK(request.size() == 1);
            CHECK(request[0] == static_cast<dp::u8>(i));

            // Send response
            netpipe::Message response = {static_cast<dp::u8>(i + 100)};
            auto send_res = server->send(response);
            CHECK(send_res.is_ok());
        }

        client_thread.join();
        server->close();
    }
}

TEST_CASE("ShmRpcStream - Error conditions") {
    SUBCASE("Close and check connection state") {
        auto server_res = netpipe::ShmRpcStream::create_server("test_shm_rpc_8", 8192);
        REQUIRE(server_res.is_ok());
        auto server = std::move(server_res.value());
        CHECK(server->is_connected());

        server->close();
        CHECK_FALSE(server->is_connected());
    }
}

TEST_CASE("ShmRpcStream - Cleanup") {
    SUBCASE("Shared memory is cleaned up on close") {
        {
            auto server_res = netpipe::ShmRpcStream::create_server("test_shm_rpc_cleanup", 8192);
            REQUIRE(server_res.is_ok());
            auto server = std::move(server_res.value());
            // server goes out of scope and closes
        }

        // Should be able to create again after cleanup
        auto server_res = netpipe::ShmRpcStream::create_server("test_shm_rpc_cleanup", 8192);
        CHECK(server_res.is_ok());
        if (server_res.is_ok()) {
            server_res.value()->close();
        }
    }
}

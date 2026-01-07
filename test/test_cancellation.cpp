#include <atomic>
#include <chrono>
#include <doctest/doctest.h>
#include <netpipe/netpipe.hpp>
#include <thread>

TEST_CASE("Request Cancellation") {
    SUBCASE("RemoteAsync - Cancel non-existent request") {
        netpipe::TcpStream server;
        netpipe::TcpEndpoint endpoint{"127.0.0.1", 20001};

        auto listen_res = server.listen(endpoint);
        REQUIRE(listen_res.is_ok());

        std::atomic<bool> done{false};

        std::thread server_thread([&]() {
            auto accept_res = server.accept();
            if (accept_res.is_ok()) {
                auto stream = std::move(accept_res.value());
                netpipe::RemotePeer peer(*stream);
                peer.register_method(1, [](const netpipe::Message &req) { return dp::result::ok(req); });

                while (!done.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        netpipe::TcpStream client;
        auto connect_res = client.connect(endpoint);
        REQUIRE(connect_res.is_ok());

        netpipe::RemoteAsync remote(client);

        // Try to cancel a request that doesn't exist
        bool cancelled = remote.cancel(999);
        CHECK(cancelled == false);

        done = true;
        server_thread.join();
        client.close();
        server.close();
    }

    SUBCASE("RemoteAsync - Cancel after completion") {
        netpipe::TcpStream server;
        netpipe::TcpEndpoint endpoint{"127.0.0.1", 20002};

        auto listen_res = server.listen(endpoint);
        REQUIRE(listen_res.is_ok());

        std::atomic<bool> done{false};

        std::thread server_thread([&]() {
            auto accept_res = server.accept();
            if (accept_res.is_ok()) {
                auto stream = std::move(accept_res.value());
                netpipe::RemotePeer peer(*stream);

                // Fast handler
                peer.register_method(
                    1, [](const netpipe::Message &req) -> dp::Res<netpipe::Message> { return dp::result::ok(req); });

                while (!done.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        netpipe::TcpStream client;
        auto connect_res = client.connect(endpoint);
        REQUIRE(connect_res.is_ok());

        netpipe::RemoteAsync remote(client);

        // Make a fast call
        netpipe::Message req = {1, 2, 3};
        auto resp = remote.call(1, req, 1000);
        REQUIRE(resp.is_ok());

        // Try to cancel after completion
        bool cancelled = remote.cancel(0);
        CHECK(cancelled == false); // Should fail because already completed

        done = true;
        server_thread.join();
        client.close();
        server.close();
    }

    SUBCASE("RemotePeer - Cancel method exists") {
        netpipe::TcpStream server;
        netpipe::TcpEndpoint endpoint{"127.0.0.1", 20003};

        auto listen_res = server.listen(endpoint);
        REQUIRE(listen_res.is_ok());

        std::atomic<bool> done{false};

        std::thread server_thread([&]() {
            auto accept_res = server.accept();
            if (accept_res.is_ok()) {
                auto stream = std::move(accept_res.value());
                netpipe::RemotePeer peer(*stream);

                peer.register_method(
                    1, [](const netpipe::Message &req) -> dp::Res<netpipe::Message> { return dp::result::ok(req); });

                while (!done.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        netpipe::TcpStream client;
        auto connect_res = client.connect(endpoint);
        REQUIRE(connect_res.is_ok());

        netpipe::RemotePeer peer(client);

        // Try to cancel non-existent request
        bool cancelled = peer.cancel(999);
        CHECK(cancelled == false);

        done = true;
        server_thread.join();
        client.close();
        server.close();
    }

    SUBCASE("Cancel message type exists in protocol") {
        // Just verify the Cancel message type is defined
        netpipe::remote::MessageType cancel_type = netpipe::remote::MessageType::Cancel;
        CHECK(static_cast<dp::u8>(cancel_type) == 7);
    }

    SUBCASE("PendingRequest has cancelled flag") {
        // Verify PendingRequest structure has cancelled field
        netpipe::remote::PendingRequest req(123);
        CHECK(req.cancelled == false);
        CHECK(req.completed == false);
        CHECK(req.request_id == 123);
    }
}

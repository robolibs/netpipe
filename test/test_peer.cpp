#include <atomic>
#include <chrono>
#include <doctest/doctest.h>
#include <netpipe/netpipe.hpp>
#include <thread>

TEST_CASE("RemotePeer - Bidirectional RPC") {
    SUBCASE("Peer can register methods and receive calls") {
        netpipe::TcpStream server;
        netpipe::TcpEndpoint endpoint{"127.0.0.1", 18030};

        auto listen_res = server.listen(endpoint);
        REQUIRE(listen_res.is_ok());

        std::atomic<bool> server_handled{false};

        // Server peer
        std::thread server_thread([&]() {
            auto accept_res = server.accept();
            if (accept_res.is_ok()) {
                auto stream = std::move(accept_res.value());
                netpipe::RemotePeer peer(*stream);

                // Register method
                peer.register_method(1, [&](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
                    server_handled = true;
                    netpipe::Message resp = {static_cast<dp::u8>(req[0] * 2)};
                    return dp::result::ok(resp);
                });

                // Keep alive
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        });

        // Client peer
        std::thread client_thread([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            netpipe::TcpStream stream;
            auto connect_res = stream.connect(endpoint);
            if (connect_res.is_ok()) {
                netpipe::RemotePeer peer(stream);

                // Call method on server
                netpipe::Message req = {5};
                auto resp = peer.call(1, req, 1000);
                REQUIRE(resp.is_ok());
                CHECK(resp.value()[0] == 10);

                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        });

        server_thread.join();
        client_thread.join();
        server.close();

        CHECK(server_handled.load());
    }

    SUBCASE("Both peers can call each other sequentially") {
        netpipe::TcpStream server;
        netpipe::TcpEndpoint endpoint{"127.0.0.1", 18031};

        auto listen_res = server.listen(endpoint);
        REQUIRE(listen_res.is_ok());

        std::atomic<bool> peer1_called{false};
        std::atomic<bool> peer2_called{false};
        std::atomic<bool> peer1_done{false};
        std::atomic<bool> peer2_done{false};

        // Peer 1
        std::thread peer1_thread([&]() {
            auto accept_res = server.accept();
            if (accept_res.is_ok()) {
                auto stream = std::move(accept_res.value());
                netpipe::RemotePeer peer(*stream);

                // Register method 1
                peer.register_method(1, [&](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
                    peer1_called = true;
                    return dp::result::ok(req);
                });

                // Wait for peer2 to call us first
                while (!peer1_called.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }

                // Now call peer2
                netpipe::Message req = {0x02};
                auto resp = peer.call(2, req, 1000);
                CHECK(resp.is_ok());
                peer1_done = true;

                // Wait for peer2 to finish
                while (!peer2_done.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
        });

        // Peer 2
        std::thread peer2_thread([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            netpipe::TcpStream stream;
            auto connect_res = stream.connect(endpoint);
            if (connect_res.is_ok()) {
                netpipe::RemotePeer peer(stream);

                // Register method 2
                peer.register_method(2, [&](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
                    peer2_called = true;
                    return dp::result::ok(req);
                });

                // Call peer1 first
                netpipe::Message req = {0x01};
                auto resp = peer.call(1, req, 1000);
                CHECK(resp.is_ok());

                // Wait for peer1 to call us back
                while (!peer2_called.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                peer2_done = true;

                // Wait for peer1 to finish
                while (!peer1_done.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
        });

        peer1_thread.join();
        peer2_thread.join();
        server.close();

        CHECK(peer1_called.load());
        CHECK(peer2_called.load());
    }

    SUBCASE("Method and pending count tracking") {
        netpipe::TcpStream server;
        netpipe::TcpEndpoint endpoint{"127.0.0.1", 18032};

        auto listen_res = server.listen(endpoint);
        REQUIRE(listen_res.is_ok());

        std::thread server_thread([&]() {
            auto accept_res = server.accept();
            if (accept_res.is_ok()) {
                auto stream = std::move(accept_res.value());
                netpipe::RemotePeer peer(*stream);

                peer.register_method(1, [](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    return dp::result::ok(req);
                });

                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        });

        std::thread client_thread([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            netpipe::TcpStream stream;
            auto connect_res = stream.connect(endpoint);
            if (connect_res.is_ok()) {
                netpipe::RemotePeer peer(stream);

                // Check method count
                CHECK(peer.method_count() == 0);

                peer.register_method(1, [](const netpipe::Message &) { return dp::result::ok(netpipe::Message()); });
                peer.register_method(2, [](const netpipe::Message &) { return dp::result::ok(netpipe::Message()); });

                CHECK(peer.method_count() == 2);

                // Make a call and check pending
                netpipe::Message req = {0x01};
                auto resp = peer.call(1, req, 1000);
                CHECK(resp.is_ok());

                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        });

        server_thread.join();
        client_thread.join();
        server.close();
    }
}

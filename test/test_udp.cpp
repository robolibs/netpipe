#include <chrono>
#include <doctest/doctest.h>
#include <netpipe/datagram/udp.hpp>
#include <thread>

TEST_CASE("UdpDatagram - Basic send and receive") {
    SUBCASE("Send to and receive from") {
        netpipe::UdpDatagram receiver;
        netpipe::UdpEndpoint recv_endpoint{"127.0.0.1", 19101};

        // Bind receiver
        auto bind_res = receiver.bind(recv_endpoint);
        REQUIRE(bind_res.is_ok());

        std::thread sender_thread([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            netpipe::UdpDatagram sender;

            // Send message
            netpipe::Message msg = {0x01, 0x02, 0x03, 0x04, 0x05};
            auto send_res = sender.send_to(msg, recv_endpoint);
            CHECK(send_res.is_ok());

            sender.close();
        });

        // Receive message
        auto recv_res = receiver.recv_from();
        REQUIRE(recv_res.is_ok());

        auto [msg, src] = std::move(recv_res.value());
        CHECK(msg.size() == 5);
        CHECK(msg[0] == 0x01);
        CHECK(msg[4] == 0x05);
        CHECK(src.host == "127.0.0.1");

        sender_thread.join();
        receiver.close();
    }

    SUBCASE("Send without bind") {
        netpipe::UdpDatagram sender;
        netpipe::UdpEndpoint dest{"127.0.0.1", 19102};

        // Should be able to send without binding
        netpipe::Message msg = {0xAA, 0xBB};
        auto send_res = sender.send_to(msg, dest);
        // May succeed or fail depending on if receiver is listening
        // Just check it doesn't crash
        sender.close();
    }
}

TEST_CASE("UdpDatagram - Message size limits") {
    SUBCASE("Send small message") {
        netpipe::UdpDatagram receiver;
        netpipe::UdpEndpoint recv_endpoint{"127.0.0.1", 19103};

        auto bind_res = receiver.bind(recv_endpoint);
        REQUIRE(bind_res.is_ok());

        std::thread sender_thread([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            netpipe::UdpDatagram sender;

            netpipe::Message msg = {0x42};
            auto send_res = sender.send_to(msg, recv_endpoint);
            CHECK(send_res.is_ok());

            sender.close();
        });

        auto recv_res = receiver.recv_from();
        REQUIRE(recv_res.is_ok());
        auto [msg, src] = std::move(recv_res.value());
        CHECK(msg.size() == 1);
        CHECK(msg[0] == 0x42);

        sender_thread.join();
        receiver.close();
    }

    SUBCASE("Send maximum safe size message") {
        netpipe::UdpDatagram receiver;
        netpipe::UdpEndpoint recv_endpoint{"127.0.0.1", 19104};

        auto bind_res = receiver.bind(recv_endpoint);
        REQUIRE(bind_res.is_ok());

        std::thread sender_thread([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            netpipe::UdpDatagram sender;

            // 1400 bytes is the safe size
            netpipe::Message msg(1400);
            for (size_t i = 0; i < msg.size(); i++) {
                msg[i] = static_cast<dp::u8>(i % 256);
            }
            auto send_res = sender.send_to(msg, recv_endpoint);
            CHECK(send_res.is_ok());

            sender.close();
        });

        auto recv_res = receiver.recv_from();
        REQUIRE(recv_res.is_ok());
        auto [msg, src] = std::move(recv_res.value());
        CHECK(msg.size() == 1400);

        // Verify pattern
        for (size_t i = 0; i < msg.size(); i++) {
            CHECK(msg[i] == static_cast<dp::u8>(i % 256));
        }

        sender_thread.join();
        receiver.close();
    }

    SUBCASE("Send too large message fails") {
        netpipe::UdpDatagram sender;
        netpipe::UdpEndpoint dest{"127.0.0.1", 19105};

        // Message larger than MAX_UDP_SIZE (1400)
        netpipe::Message msg(2000);
        auto send_res = sender.send_to(msg, dest);
        CHECK(send_res.is_err());

        sender.close();
    }
}

TEST_CASE("UdpDatagram - Multiple messages") {
    SUBCASE("Send and receive multiple messages") {
        netpipe::UdpDatagram receiver;
        netpipe::UdpEndpoint recv_endpoint{"127.0.0.1", 19106};

        auto bind_res = receiver.bind(recv_endpoint);
        REQUIRE(bind_res.is_ok());

        std::thread sender_thread([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            netpipe::UdpDatagram sender;

            // Send multiple messages
            for (int i = 0; i < 5; i++) {
                netpipe::Message msg = {static_cast<dp::u8>(i)};
                auto send_res = sender.send_to(msg, recv_endpoint);
                CHECK(send_res.is_ok());
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            sender.close();
        });

        // Receive all messages
        for (int i = 0; i < 5; i++) {
            auto recv_res = receiver.recv_from();
            REQUIRE(recv_res.is_ok());
            auto [msg, src] = std::move(recv_res.value());
            CHECK(msg.size() == 1);
            CHECK(msg[0] == static_cast<dp::u8>(i));
        }

        sender_thread.join();
        receiver.close();
    }
}

TEST_CASE("UdpDatagram - Broadcast") {
    SUBCASE("Broadcast message") {
        netpipe::UdpDatagram sender;

        // Enable broadcast and send
        netpipe::Message msg = {0xFF, 0xFF, 0xFF};
        auto broadcast_res = sender.broadcast(msg);
        // Broadcast may succeed or fail depending on network configuration
        // Just check it doesn't crash

        sender.close();
    }

    SUBCASE("Broadcast with bound socket") {
        netpipe::UdpDatagram sender;
        netpipe::UdpEndpoint endpoint{"0.0.0.0", 19107};

        auto bind_res = sender.bind(endpoint);
        REQUIRE(bind_res.is_ok());

        netpipe::Message msg = {0xBB, 0xBB};
        auto broadcast_res = sender.broadcast(msg);
        // May succeed or fail, just check it doesn't crash

        sender.close();
    }

    SUBCASE("Broadcast too large message fails") {
        netpipe::UdpDatagram sender;

        netpipe::Message msg(2000);
        auto broadcast_res = sender.broadcast(msg);
        CHECK(broadcast_res.is_err());

        sender.close();
    }
}

TEST_CASE("UdpDatagram - Error conditions") {
    SUBCASE("Recv without bind fails") {
        netpipe::UdpDatagram receiver;
        auto recv_res = receiver.recv_from();
        CHECK(recv_res.is_err());
    }

    SUBCASE("Bind to invalid address fails") {
        netpipe::UdpDatagram receiver;
        netpipe::UdpEndpoint endpoint{"999.999.999.999", 19108};

        auto bind_res = receiver.bind(endpoint);
        CHECK(bind_res.is_err());
    }

    SUBCASE("Close and reuse") {
        netpipe::UdpDatagram datagram;
        netpipe::UdpEndpoint endpoint{"127.0.0.1", 19109};

        auto bind_res = datagram.bind(endpoint);
        REQUIRE(bind_res.is_ok());

        datagram.close();

        // After close, should be able to bind again
        netpipe::UdpDatagram datagram2;
        auto bind_res2 = datagram2.bind(endpoint);
        CHECK(bind_res2.is_ok());

        datagram2.close();
    }
}

TEST_CASE("UdpDatagram - Different endpoints") {
    SUBCASE("Bind to 0.0.0.0") {
        netpipe::UdpDatagram receiver;
        netpipe::UdpEndpoint endpoint{"0.0.0.0", 19110};

        auto bind_res = receiver.bind(endpoint);
        CHECK(bind_res.is_ok());

        receiver.close();
    }

    SUBCASE("Send to 127.0.0.1") {
        netpipe::UdpDatagram receiver;
        netpipe::UdpEndpoint recv_endpoint{"127.0.0.1", 19111};

        auto bind_res = receiver.bind(recv_endpoint);
        REQUIRE(bind_res.is_ok());

        std::thread sender_thread([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            netpipe::UdpDatagram sender;

            netpipe::Message msg = {0x99};
            auto send_res = sender.send_to(msg, recv_endpoint);
            CHECK(send_res.is_ok());

            sender.close();
        });

        auto recv_res = receiver.recv_from();
        REQUIRE(recv_res.is_ok());

        sender_thread.join();
        receiver.close();
    }
}

TEST_CASE("UdpDatagram - Empty message") {
    SUBCASE("Send and receive empty message") {
        netpipe::UdpDatagram receiver;
        netpipe::UdpEndpoint recv_endpoint{"127.0.0.1", 19112};

        auto bind_res = receiver.bind(recv_endpoint);
        REQUIRE(bind_res.is_ok());

        std::thread sender_thread([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            netpipe::UdpDatagram sender;

            netpipe::Message msg; // Empty
            auto send_res = sender.send_to(msg, recv_endpoint);
            CHECK(send_res.is_ok());

            sender.close();
        });

        auto recv_res = receiver.recv_from();
        REQUIRE(recv_res.is_ok());
        auto [msg, src] = std::move(recv_res.value());
        CHECK(msg.empty());

        sender_thread.join();
        receiver.close();
    }
}

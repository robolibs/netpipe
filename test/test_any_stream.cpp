#include <chrono>
#include <doctest/doctest.h>
#include <netpipe/netpipe.hpp>
#include <thread>

TEST_CASE("AnyStream - TCP basic exchange") {
    netpipe::AnyEndpoint endpoint = netpipe::AnyEndpoint::tcp_endpoint("127.0.0.1", 19501);

    netpipe::AnyStream server;
    auto listen_res = server.listen(endpoint);
    REQUIRE(listen_res.is_ok());

    std::thread client_thread([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        netpipe::AnyStream client;
        auto connect_res = client.connect(endpoint);
        REQUIRE(connect_res.is_ok());

        netpipe::Message msg = {0x01, 0x02, 0x03};
        CHECK(client.send(msg).is_ok());
        client.close();
    });

    auto accept_res = server.accept();
    REQUIRE(accept_res.is_ok());
    auto conn = std::move(accept_res.value());

    auto recv_res = conn.recv();
    REQUIRE(recv_res.is_ok());
    auto msg = std::move(recv_res.value());
    CHECK(msg.size() == 3);
    CHECK(msg[0] == 0x01);
    CHECK(msg[2] == 0x03);

    client_thread.join();
    conn.close();
    server.close();
}

TEST_CASE("AnyStream - IPC basic exchange") {
    netpipe::AnyEndpoint endpoint = netpipe::AnyEndpoint::ipc_endpoint("/tmp/netpipe_test_any_ipc.sock");

    netpipe::AnyStream server;
    auto listen_res = server.listen(endpoint);
    REQUIRE(listen_res.is_ok());

    std::thread client_thread([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        netpipe::AnyStream client;
        auto connect_res = client.connect(endpoint);
        REQUIRE(connect_res.is_ok());

        netpipe::Message msg = {0x0A, 0x0B};
        CHECK(client.send(msg).is_ok());
        client.close();
    });

    auto accept_res = server.accept();
    REQUIRE(accept_res.is_ok());
    auto conn = std::move(accept_res.value());

    auto recv_res = conn.recv();
    REQUIRE(recv_res.is_ok());
    auto msg = std::move(recv_res.value());
    CHECK(msg.size() == 2);
    CHECK(msg[0] == 0x0A);
    CHECK(msg[1] == 0x0B);

    client_thread.join();
    conn.close();
    server.close();
}

TEST_CASE("AnyStream - SHM basic exchange") {
    netpipe::AnyEndpoint endpoint = netpipe::AnyEndpoint::shm_endpoint("netpipe_test_any_shm", 8192);

    netpipe::AnyStream server;
    auto listen_res = server.listen(endpoint);
    REQUIRE(listen_res.is_ok());

    std::thread client_thread([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        netpipe::AnyStream client;
        auto connect_res = client.connect(endpoint);
        REQUIRE(connect_res.is_ok());

        netpipe::Message msg = {0xEE, 0xFF};
        CHECK(client.send(msg).is_ok());
        client.close();
    });

    auto accept_res = server.accept();
    REQUIRE(accept_res.is_ok());
    auto conn = std::move(accept_res.value());

    auto recv_res = conn.recv();
    REQUIRE(recv_res.is_ok());
    auto msg = std::move(recv_res.value());
    CHECK(msg.size() == 2);
    CHECK(msg[0] == 0xEE);
    CHECK(msg[1] == 0xFF);

    client_thread.join();
    conn.close();
    server.close();
}

TEST_CASE("AnyStream - Error conditions") {
    SUBCASE("send/recv without initialization fails") {
        netpipe::AnyStream s;
        netpipe::Message m = {0x01};
        CHECK(s.send(m).is_err());
        CHECK(s.recv().is_err());
    }

    SUBCASE("accept without listen/connect fails") {
        netpipe::AnyStream s;
        CHECK(s.accept().is_err());
    }
}

#include <atomic>
#include <chrono>
#include <doctest/doctest.h>
#include <memory>
#include <netpipe/netpipe.hpp>
#include <thread>

static netpipe::Message create_payload(size_t size) {
    netpipe::Message msg(size);
    for (size_t i = 0; i < size; i++) {
        msg[i] = static_cast<dp::u8>(i % 256);
    }
    return msg;
}

static bool verify_payload(const netpipe::Message &msg, size_t expected_size) {
    if (msg.size() != expected_size)
        return false;
    for (size_t i = 0; i < msg.size(); i++) {
        if (msg[i] != static_cast<dp::u8>(i % 256))
            return false;
    }
    return true;
}

TEST_CASE("IpcStream + Remote<Bidirect> - 1MB payload") {
    const size_t PAYLOAD_SIZE = 1024 * 1024;
    const dp::u32 METHOD_PING = 1;
    const dp::u32 METHOD_PONG = 2;

    netpipe::IpcStream server_stream;
    netpipe::IpcEndpoint endpoint{"/tmp/netpipe_test_ipc_bidirect_1mb.sock"};
    REQUIRE(server_stream.listen_ipc(endpoint).is_ok());

    std::atomic<bool> peer1_called{false}, peer2_called{false};
    std::atomic<bool> peer1_ok{false}, peer2_ok{false};

    std::unique_ptr<netpipe::Stream> server_conn;

    std::thread t1([&]() {
        auto accept_res = server_stream.accept();
        REQUIRE(accept_res.is_ok());
        server_conn = std::move(accept_res.value());

        netpipe::Remote<netpipe::Bidirect> r(*server_conn);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        r.register_method(METHOD_PONG, [&](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            peer1_called = true;
            if (!verify_payload(req, PAYLOAD_SIZE))
                return dp::result::err(dp::Error::invalid_argument("bad"));
            return dp::result::ok(create_payload(PAYLOAD_SIZE));
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        auto resp = r.call(METHOD_PING, create_payload(PAYLOAD_SIZE), 10000);
        if (resp.is_ok() && verify_payload(resp.value(), PAYLOAD_SIZE))
            peer1_ok = true;

        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    });

    std::thread t2([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        netpipe::IpcStream client;
        REQUIRE(client.connect_ipc(endpoint).is_ok());

        netpipe::Remote<netpipe::Bidirect> r(client);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        r.register_method(METHOD_PING, [&](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            peer2_called = true;
            if (!verify_payload(req, PAYLOAD_SIZE))
                return dp::result::err(dp::Error::invalid_argument("bad"));
            return dp::result::ok(create_payload(PAYLOAD_SIZE));
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        auto resp = r.call(METHOD_PONG, create_payload(PAYLOAD_SIZE), 10000);
        if (resp.is_ok() && verify_payload(resp.value(), PAYLOAD_SIZE))
            peer2_ok = true;

        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    });

    t1.join();
    t2.join();

    CHECK(peer1_called);
    CHECK(peer2_called);
    CHECK(peer1_ok);
    CHECK(peer2_ok);

    server_stream.close();
}

TEST_CASE("IpcStream + Remote<Bidirect> - 10MB payload") {
    const size_t PAYLOAD_SIZE = 10 * 1024 * 1024;
    const dp::u32 METHOD_PING = 1;
    const dp::u32 METHOD_PONG = 2;

    netpipe::IpcStream server_stream;
    netpipe::IpcEndpoint endpoint{"/tmp/netpipe_test_ipc_bidirect_10mb.sock"};
    REQUIRE(server_stream.listen_ipc(endpoint).is_ok());

    std::atomic<bool> peer1_called{false}, peer2_called{false};
    std::atomic<bool> peer1_ok{false}, peer2_ok{false};

    std::unique_ptr<netpipe::Stream> server_conn;

    std::thread t1([&]() {
        auto accept_res = server_stream.accept();
        REQUIRE(accept_res.is_ok());
        server_conn = std::move(accept_res.value());

        netpipe::Remote<netpipe::Bidirect> r(*server_conn);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        r.register_method(METHOD_PONG, [&](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            peer1_called = true;
            if (!verify_payload(req, PAYLOAD_SIZE))
                return dp::result::err(dp::Error::invalid_argument("bad"));
            return dp::result::ok(create_payload(PAYLOAD_SIZE));
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        auto resp = r.call(METHOD_PING, create_payload(PAYLOAD_SIZE), 30000);
        if (resp.is_ok() && verify_payload(resp.value(), PAYLOAD_SIZE))
            peer1_ok = true;

        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    });

    std::thread t2([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        netpipe::IpcStream client;
        REQUIRE(client.connect_ipc(endpoint).is_ok());

        netpipe::Remote<netpipe::Bidirect> r(client);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        r.register_method(METHOD_PING, [&](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            peer2_called = true;
            if (!verify_payload(req, PAYLOAD_SIZE))
                return dp::result::err(dp::Error::invalid_argument("bad"));
            return dp::result::ok(create_payload(PAYLOAD_SIZE));
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        auto resp = r.call(METHOD_PONG, create_payload(PAYLOAD_SIZE), 30000);
        if (resp.is_ok() && verify_payload(resp.value(), PAYLOAD_SIZE))
            peer2_ok = true;

        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    });

    t1.join();
    t2.join();

    CHECK(peer1_called);
    CHECK(peer2_called);
    CHECK(peer1_ok);
    CHECK(peer2_ok);

    server_stream.close();
}

#ifdef BIG_TRANSFER
TEST_CASE("IpcStream + Remote<Bidirect> - 100MB payload") {
    const size_t PAYLOAD_SIZE = 100 * 1024 * 1024;
    const dp::u32 METHOD_PING = 1;
    const dp::u32 METHOD_PONG = 2;

    netpipe::IpcStream server_stream;
    netpipe::IpcEndpoint endpoint{"/tmp/netpipe_test_ipc_bidirect_100mb.sock"};
    REQUIRE(server_stream.listen_ipc(endpoint).is_ok());

    std::atomic<bool> peer1_called{false}, peer2_called{false};
    std::atomic<bool> peer1_ok{false}, peer2_ok{false};

    std::unique_ptr<netpipe::Stream> server_conn;

    std::thread t1([&]() {
        auto accept_res = server_stream.accept();
        REQUIRE(accept_res.is_ok());
        server_conn = std::move(accept_res.value());

        netpipe::Remote<netpipe::Bidirect> r(*server_conn);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        r.register_method(METHOD_PONG, [&](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            peer1_called = true;
            if (!verify_payload(req, PAYLOAD_SIZE))
                return dp::result::err(dp::Error::invalid_argument("bad"));
            return dp::result::ok(create_payload(PAYLOAD_SIZE));
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        auto resp = r.call(METHOD_PING, create_payload(PAYLOAD_SIZE), 60000);
        if (resp.is_ok() && verify_payload(resp.value(), PAYLOAD_SIZE))
            peer1_ok = true;

        std::this_thread::sleep_for(std::chrono::milliseconds(3000));
    });

    std::thread t2([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        netpipe::IpcStream client;
        REQUIRE(client.connect_ipc(endpoint).is_ok());

        netpipe::Remote<netpipe::Bidirect> r(client);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        r.register_method(METHOD_PING, [&](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            peer2_called = true;
            if (!verify_payload(req, PAYLOAD_SIZE))
                return dp::result::err(dp::Error::invalid_argument("bad"));
            return dp::result::ok(create_payload(PAYLOAD_SIZE));
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        auto resp = r.call(METHOD_PONG, create_payload(PAYLOAD_SIZE), 60000);
        if (resp.is_ok() && verify_payload(resp.value(), PAYLOAD_SIZE))
            peer2_ok = true;

        std::this_thread::sleep_for(std::chrono::milliseconds(3000));
    });

    t1.join();
    t2.join();

    CHECK(peer1_called);
    CHECK(peer2_called);
    CHECK(peer1_ok);
    CHECK(peer2_ok);

    server_stream.close();
}

TEST_CASE("IpcStream + Remote<Bidirect> - 1GB payload") {
    const size_t PAYLOAD_SIZE = 1024 * 1024 * 1024;
    const dp::u32 METHOD_PING = 1;
    const dp::u32 METHOD_PONG = 2;

    netpipe::IpcStream server_stream;
    netpipe::IpcEndpoint endpoint{"/tmp/netpipe_test_ipc_bidirect_1gb.sock"};
    REQUIRE(server_stream.listen_ipc(endpoint).is_ok());

    std::atomic<bool> peer1_called{false}, peer2_called{false};
    std::atomic<bool> peer1_ok{false}, peer2_ok{false};

    std::unique_ptr<netpipe::Stream> server_conn;

    std::thread t1([&]() {
        auto accept_res = server_stream.accept();
        REQUIRE(accept_res.is_ok());
        server_conn = std::move(accept_res.value());

        netpipe::Remote<netpipe::Bidirect> r(*server_conn);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        r.register_method(METHOD_PONG, [&](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            peer1_called = true;
            if (!verify_payload(req, PAYLOAD_SIZE))
                return dp::result::err(dp::Error::invalid_argument("bad"));
            return dp::result::ok(create_payload(PAYLOAD_SIZE));
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        auto resp = r.call(METHOD_PING, create_payload(PAYLOAD_SIZE), 120000);
        if (resp.is_ok() && verify_payload(resp.value(), PAYLOAD_SIZE))
            peer1_ok = true;

        std::this_thread::sleep_for(std::chrono::milliseconds(5000));
    });

    std::thread t2([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        netpipe::IpcStream client;
        REQUIRE(client.connect_ipc(endpoint).is_ok());

        netpipe::Remote<netpipe::Bidirect> r(client);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        r.register_method(METHOD_PING, [&](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            peer2_called = true;
            if (!verify_payload(req, PAYLOAD_SIZE))
                return dp::result::err(dp::Error::invalid_argument("bad"));
            return dp::result::ok(create_payload(PAYLOAD_SIZE));
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        auto resp = r.call(METHOD_PONG, create_payload(PAYLOAD_SIZE), 120000);
        if (resp.is_ok() && verify_payload(resp.value(), PAYLOAD_SIZE))
            peer2_ok = true;

        std::this_thread::sleep_for(std::chrono::milliseconds(5000));
    });

    t1.join();
    t2.join();

    CHECK(peer1_called);
    CHECK(peer2_called);
    CHECK(peer1_ok);
    CHECK(peer2_ok);

    server_stream.close();
}
#endif // BIG_TRANSFER

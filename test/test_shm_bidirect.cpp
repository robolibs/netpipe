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

TEST_CASE("ShmStream + Remote<Bidirect> - 1MB payload") {
    const size_t PAYLOAD_SIZE = 1024 * 1024;
    const dp::u32 METHOD_PING = 1;
    const dp::u32 METHOD_PONG = 2;

    netpipe::ShmStream listener;
    netpipe::ShmEndpoint endpoint{"shm_bidirect_1mb", 8 * 1024 * 1024};
    REQUIRE(listener.listen_shm(endpoint).is_ok());

    std::atomic<bool> peer1_called{false}, peer2_called{false};
    std::atomic<bool> peer1_ok{false}, peer2_ok{false};
    std::atomic<bool> client_connected{false};

    std::unique_ptr<netpipe::Stream> server_conn;

    std::thread t1([&]() {
        auto accept_res = listener.accept();
        REQUIRE(accept_res.is_ok());
        server_conn = std::move(accept_res.value());

        // Signal and wait for client to be ready
        while (!client_connected) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        // Give time for both sides to fully initialize buffers
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        netpipe::Remote<netpipe::Bidirect> r(*server_conn);

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

        netpipe::ShmStream client;
        REQUIRE(client.connect_shm(endpoint).is_ok());
        client_connected = true;

        // Give time for both sides to fully initialize buffers
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        netpipe::Remote<netpipe::Bidirect> r(client);

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

    listener.close();
}

TEST_CASE("ShmStream + Remote<Bidirect> - 10MB payload") {
    const size_t PAYLOAD_SIZE = 10 * 1024 * 1024;
    const dp::u32 METHOD_PING = 1;
    const dp::u32 METHOD_PONG = 2;

    netpipe::ShmStream listener;
    netpipe::ShmEndpoint endpoint{"shm_bidirect_10mb", 80 * 1024 * 1024};
    REQUIRE(listener.listen_shm(endpoint).is_ok());

    std::atomic<bool> peer1_called{false}, peer2_called{false};
    std::atomic<bool> peer1_ok{false}, peer2_ok{false};
    std::atomic<bool> client_connected{false};

    std::unique_ptr<netpipe::Stream> server_conn;

    std::thread t1([&]() {
        auto accept_res = listener.accept();
        REQUIRE(accept_res.is_ok());
        server_conn = std::move(accept_res.value());

        while (!client_connected) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        // Give time for both sides to fully initialize buffers
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        netpipe::Remote<netpipe::Bidirect> r(*server_conn);

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

        netpipe::ShmStream client;
        REQUIRE(client.connect_shm(endpoint).is_ok());
        client_connected = true;

        // Give time for both sides to fully initialize buffers
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        netpipe::Remote<netpipe::Bidirect> r(client);

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

    listener.close();
}

#ifdef BIG_TRANSFER
TEST_CASE("ShmStream + Remote<Bidirect> - 100MB payload") {
    const size_t PAYLOAD_SIZE = 100 * 1024 * 1024;
    const dp::u32 METHOD_PING = 1;
    const dp::u32 METHOD_PONG = 2;

    netpipe::ShmStream listener;
    netpipe::ShmEndpoint endpoint{"shm_bidirect_100mb", 1024ULL * 1024 * 1024};
    REQUIRE(listener.listen_shm(endpoint).is_ok());

    std::atomic<bool> peer1_called{false}, peer2_called{false};
    std::atomic<bool> peer1_ok{false}, peer2_ok{false};
    std::atomic<bool> client_connected{false};

    std::unique_ptr<netpipe::Stream> server_conn;

    std::thread t1([&]() {
        auto accept_res = listener.accept();
        REQUIRE(accept_res.is_ok());
        server_conn = std::move(accept_res.value());

        while (!client_connected) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        // Give time for both sides to fully initialize buffers
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        netpipe::Remote<netpipe::Bidirect> r(*server_conn);

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

        netpipe::ShmStream client;
        REQUIRE(client.connect_shm(endpoint).is_ok());
        client_connected = true;

        // Give time for both sides to fully initialize buffers
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        netpipe::Remote<netpipe::Bidirect> r(client);

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

    listener.close();
}

TEST_CASE("ShmStream + Remote<Bidirect> - 1GB payload") {
    const size_t PAYLOAD_SIZE = 1024 * 1024 * 1024;
    const dp::u32 METHOD_PING = 1;
    const dp::u32 METHOD_PONG = 2;

    netpipe::ShmStream listener;
    netpipe::ShmEndpoint endpoint{"shm_bidirect_1gb", 3ULL * 1024 * 1024 * 1024};
    REQUIRE(listener.listen_shm(endpoint).is_ok());

    std::atomic<bool> peer1_called{false}, peer2_called{false};
    std::atomic<bool> peer1_ok{false}, peer2_ok{false};
    std::atomic<bool> client_connected{false};

    std::unique_ptr<netpipe::Stream> server_conn;

    std::thread t1([&]() {
        auto accept_res = listener.accept();
        REQUIRE(accept_res.is_ok());
        server_conn = std::move(accept_res.value());

        while (!client_connected) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        // Give time for both sides to fully initialize buffers
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        netpipe::Remote<netpipe::Bidirect> r(*server_conn);

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

        netpipe::ShmStream client;
        REQUIRE(client.connect_shm(endpoint).is_ok());
        client_connected = true;

        // Give time for both sides to fully initialize buffers
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        netpipe::Remote<netpipe::Bidirect> r(client);

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

    listener.close();
}
#endif // BIG_TRANSFER

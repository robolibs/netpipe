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

TEST_CASE("TcpStream + Remote<Bidirect> - 1MB payload") {
    const size_t PAYLOAD_SIZE = 1024 * 1024;
    const dp::u32 METHOD_PING = 1;
    const dp::u32 METHOD_PONG = 2;

    netpipe::TcpStream server_stream;
    netpipe::TcpEndpoint endpoint{"127.0.0.1", 20001};
    REQUIRE(server_stream.listen(endpoint).is_ok());

    std::atomic<bool> peer1_called{false}, peer2_called{false};
    std::atomic<bool> peer1_ok{false}, peer2_ok{false};

    // Use shared_ptr to keep streams alive until both threads done
    std::shared_ptr<std::unique_ptr<netpipe::Stream>> shared_server_stream;
    std::shared_ptr<netpipe::TcpStream> shared_client_stream = std::make_shared<netpipe::TcpStream>();

    std::thread t1([&, shared_server_stream]() mutable {
        auto s = std::move(server_stream.accept().value());
        shared_server_stream = std::make_shared<std::unique_ptr<netpipe::Stream>>(std::move(s));

        netpipe::Remote<netpipe::Bidirect> r(**shared_server_stream);
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

    std::thread t2([&, shared_client_stream]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        REQUIRE(shared_client_stream->connect(endpoint).is_ok());

        netpipe::Remote<netpipe::Bidirect> r(*shared_client_stream);
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

TEST_CASE("TcpStream + Remote<Bidirect> - 10MB payload") {
    const size_t PAYLOAD_SIZE = 10 * 1024 * 1024;
    const dp::u32 METHOD_PING = 1;
    const dp::u32 METHOD_PONG = 2;

    netpipe::TcpStream server_stream;
    netpipe::TcpEndpoint endpoint{"127.0.0.1", 20002};
    REQUIRE(server_stream.listen(endpoint).is_ok());

    std::atomic<bool> peer1_called{false}, peer2_called{false};
    std::atomic<bool> peer1_ok{false}, peer2_ok{false};

    std::shared_ptr<std::unique_ptr<netpipe::Stream>> shared_server_stream;
    std::shared_ptr<netpipe::TcpStream> shared_client_stream = std::make_shared<netpipe::TcpStream>();

    std::thread t1([&, shared_server_stream]() mutable {
        auto s = std::move(server_stream.accept().value());
        shared_server_stream = std::make_shared<std::unique_ptr<netpipe::Stream>>(std::move(s));

        netpipe::Remote<netpipe::Bidirect> r(**shared_server_stream);
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

    std::thread t2([&, shared_client_stream]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        REQUIRE(shared_client_stream->connect(endpoint).is_ok());

        netpipe::Remote<netpipe::Bidirect> r(*shared_client_stream);
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

TEST_CASE("TcpStream + Remote<Bidirect> - server-side handler cancellation") {
    const dp::u32 METHOD_SLOW = 10;

    netpipe::TcpStream server_stream;
    netpipe::TcpEndpoint endpoint{"127.0.0.1", 20010};
    REQUIRE(server_stream.listen(endpoint).is_ok());

    std::atomic<bool> handler_started{false};
    std::atomic<bool> handler_completed{false};
    std::atomic<bool> cancel_result{false};
    std::atomic<bool> call_returned_error{false};

    std::shared_ptr<std::unique_ptr<netpipe::Stream>> shared_server_stream;
    std::shared_ptr<netpipe::TcpStream> shared_client_stream = std::make_shared<netpipe::TcpStream>();

    // Server thread - has a slow handler
    std::thread t1([&, shared_server_stream]() mutable {
        auto s = std::move(server_stream.accept().value());
        shared_server_stream = std::make_shared<std::unique_ptr<netpipe::Stream>>(std::move(s));

        netpipe::Remote<netpipe::Bidirect> r(**shared_server_stream);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Register a slow handler that takes 5 seconds
        r.register_method(METHOD_SLOW, [&](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            handler_started = true;
            // Simulate slow work - sleep in small increments to allow cancellation
            for (int i = 0; i < 50 && !handler_completed; i++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            handler_completed = true;
            return dp::result::ok(netpipe::Message{'O', 'K'});
        });

        // Keep server alive long enough for test
        std::this_thread::sleep_for(std::chrono::milliseconds(3000));
    });

    // Client thread - calls slow method then cancels it
    std::thread t2([&, shared_client_stream]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        REQUIRE(shared_client_stream->connect(endpoint).is_ok());

        netpipe::Remote<netpipe::Bidirect> r(*shared_client_stream);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Start the call in a separate thread so we can cancel it
        std::atomic<dp::u32> call_request_id{0};
        std::thread call_thread([&]() {
            // We need to track the request ID - use a small payload
            auto resp = r.call(METHOD_SLOW, netpipe::Message{'H', 'I'}, 10000);
            if (resp.is_err()) {
                call_returned_error = true;
            }
        });

        // Wait for handler to start on server
        while (!handler_started) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        // Give it a moment to be in progress
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // Cancel request ID 0 (first request)
        cancel_result = r.cancel(0);

        call_thread.join();

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    });

    t1.join();
    t2.join();

    CHECK(handler_started);
    CHECK(cancel_result);
    CHECK(call_returned_error);

    server_stream.close();
}

#ifdef BIG_TRANSFER
TEST_CASE("TcpStream + Remote<Bidirect> - 100MB payload") {
    const size_t PAYLOAD_SIZE = 100 * 1024 * 1024;
    const dp::u32 METHOD_PING = 1;
    const dp::u32 METHOD_PONG = 2;

    netpipe::TcpStream server_stream;
    netpipe::TcpEndpoint endpoint{"127.0.0.1", 20003};
    REQUIRE(server_stream.listen(endpoint).is_ok());

    std::atomic<bool> peer1_called{false}, peer2_called{false};
    std::atomic<bool> peer1_ok{false}, peer2_ok{false};

    std::shared_ptr<std::unique_ptr<netpipe::Stream>> shared_server_stream;
    std::shared_ptr<netpipe::TcpStream> shared_client_stream = std::make_shared<netpipe::TcpStream>();

    std::thread t1([&, shared_server_stream]() mutable {
        auto s = std::move(server_stream.accept().value());
        shared_server_stream = std::make_shared<std::unique_ptr<netpipe::Stream>>(std::move(s));

        netpipe::Remote<netpipe::Bidirect> r(**shared_server_stream);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        r.register_method(METHOD_PONG, [&](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            peer1_called = true;
            if (!verify_payload(req, PAYLOAD_SIZE))
                return dp::result::err(dp::Error::invalid_argument("bad"));
            return dp::result::ok(create_payload(PAYLOAD_SIZE));
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        auto resp = r.call(METHOD_PING, create_payload(PAYLOAD_SIZE), 20000);
        if (resp.is_ok() && verify_payload(resp.value(), PAYLOAD_SIZE))
            peer1_ok = true;

        std::this_thread::sleep_for(std::chrono::milliseconds(3000));
    });

    std::thread t2([&, shared_client_stream]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        REQUIRE(shared_client_stream->connect(endpoint).is_ok());

        netpipe::Remote<netpipe::Bidirect> r(*shared_client_stream);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        r.register_method(METHOD_PING, [&](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            peer2_called = true;
            if (!verify_payload(req, PAYLOAD_SIZE))
                return dp::result::err(dp::Error::invalid_argument("bad"));
            return dp::result::ok(create_payload(PAYLOAD_SIZE));
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        auto resp = r.call(METHOD_PONG, create_payload(PAYLOAD_SIZE), 20000);
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

TEST_CASE("TcpStream + Remote<Bidirect> - 1GB payload") {
    const size_t PAYLOAD_SIZE = 1024 * 1024 * 1024;
    const dp::u32 METHOD_PING = 1;
    const dp::u32 METHOD_PONG = 2;

    netpipe::TcpStream server_stream;
    netpipe::TcpEndpoint endpoint{"127.0.0.1", 20004};
    REQUIRE(server_stream.listen(endpoint).is_ok());

    std::atomic<bool> peer1_called{false}, peer2_called{false};
    std::atomic<bool> peer1_ok{false}, peer2_ok{false};

    std::shared_ptr<std::unique_ptr<netpipe::Stream>> shared_server_stream;
    std::shared_ptr<netpipe::TcpStream> shared_client_stream = std::make_shared<netpipe::TcpStream>();

    std::thread t1([&, shared_server_stream]() mutable {
        auto s = std::move(server_stream.accept().value());
        shared_server_stream = std::make_shared<std::unique_ptr<netpipe::Stream>>(std::move(s));

        netpipe::Remote<netpipe::Bidirect> r(**shared_server_stream);
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

    std::thread t2([&, shared_client_stream]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        REQUIRE(shared_client_stream->connect(endpoint).is_ok());

        netpipe::Remote<netpipe::Bidirect> r(*shared_client_stream);
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

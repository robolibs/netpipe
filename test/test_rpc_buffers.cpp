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

TEST_CASE("RPC Buffers - Large message handling (TCP)") {
    const size_t LARGE_SIZE = 10 * 1024 * 1024;
    const dp::u32 METHOD_ECHO = 1;

    netpipe::TcpStream server_stream;
    netpipe::TcpEndpoint endpoint{"127.0.0.1", 24001};
    REQUIRE(server_stream.listen(endpoint).is_ok());

    std::atomic<bool> handler_called{false};
    std::atomic<bool> done{false};

    std::thread server_thread([&]() {
        auto accept_res = server_stream.accept();
        if (accept_res.is_ok()) {
            auto client_stream = std::move(accept_res.value());
            netpipe::Remote<netpipe::Bidirect> remote(*client_stream);

            remote.register_method(METHOD_ECHO, [&](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
                handler_called = true;
                return dp::result::ok(req);
            });

            while (!done) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    netpipe::TcpStream client_stream;
    REQUIRE(client_stream.connect(endpoint).is_ok());
    netpipe::Remote<netpipe::Bidirect> client_remote(client_stream);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto payload = create_payload(LARGE_SIZE);
    auto resp = client_remote.call(METHOD_ECHO, payload, 30000);

    CHECK(resp.is_ok());
    CHECK(handler_called);

    if (resp.is_ok()) {
        CHECK(resp.value().size() == LARGE_SIZE);
    }

    client_stream.close();
    done = true;
    server_thread.join();
    server_stream.close();
}

TEST_CASE("RPC Buffers - Multiple large messages in sequence (TCP)") {
    const size_t MESSAGE_SIZE = 5 * 1024 * 1024;
    const int MESSAGE_COUNT = 5;
    const dp::u32 METHOD_ECHO = 1;

    netpipe::TcpStream server_stream;
    netpipe::TcpEndpoint endpoint{"127.0.0.1", 24002};
    REQUIRE(server_stream.listen(endpoint).is_ok());

    std::atomic<int> handler_count{0};
    std::atomic<bool> done{false};

    std::thread server_thread([&]() {
        auto accept_res = server_stream.accept();
        if (accept_res.is_ok()) {
            auto client_stream = std::move(accept_res.value());
            netpipe::Remote<netpipe::Bidirect> remote(*client_stream);

            remote.register_method(METHOD_ECHO, [&](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
                handler_count++;
                return dp::result::ok(req);
            });

            while (!done) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    netpipe::TcpStream client_stream;
    REQUIRE(client_stream.connect(endpoint).is_ok());
    netpipe::Remote<netpipe::Bidirect> client_remote(client_stream);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    int success_count = 0;
    for (int i = 0; i < MESSAGE_COUNT; i++) {
        auto payload = create_payload(MESSAGE_SIZE);
        auto resp = client_remote.call(METHOD_ECHO, payload, 30000);

        if (resp.is_ok()) {
            success_count++;
        }
    }

    CHECK(success_count == MESSAGE_COUNT);
    CHECK(handler_count == MESSAGE_COUNT);

    client_stream.close();
    done = true;
    server_thread.join();
    server_stream.close();
}

TEST_CASE("RPC Buffers - Message size validation (TCP)") {
    const dp::u32 METHOD_ECHO = 1;

    netpipe::TcpStream server_stream;
    netpipe::TcpEndpoint endpoint{"127.0.0.1", 24003};
    REQUIRE(server_stream.listen(endpoint).is_ok());

    std::atomic<bool> done{false};

    std::thread server_thread([&]() {
        auto accept_res = server_stream.accept();
        if (accept_res.is_ok()) {
            auto client_stream = std::move(accept_res.value());
            netpipe::Remote<netpipe::Bidirect> remote(*client_stream);

            remote.register_method(METHOD_ECHO, [&](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
                return dp::result::ok(req);
            });

            while (!done) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    netpipe::TcpStream client_stream;
    REQUIRE(client_stream.connect(endpoint).is_ok());
    netpipe::Remote<netpipe::Bidirect> client_remote(client_stream);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::vector<size_t> sizes = {100, 1024, 1024 * 1024, 10 * 1024 * 1024};

    for (auto size : sizes) {
        auto payload = create_payload(size);
        auto resp = client_remote.call(METHOD_ECHO, payload, 30000);

        CHECK(resp.is_ok());
        if (resp.is_ok()) {
            CHECK(resp.value().size() == size);
        }
    }

    client_stream.close();
    done = true;
    server_thread.join();
    server_stream.close();
}

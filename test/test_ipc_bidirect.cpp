#include <chrono>
#include <doctest/doctest.h>
#include <netpipe/netpipe.hpp>
#include <thread>

// Helper function to create test payload of specific size
static netpipe::Message create_payload(dp::usize size, dp::u8 pattern = 0x00) {
    netpipe::Message payload(size);
    for (dp::usize i = 0; i < size; i++) {
        payload[i] = static_cast<dp::u8>((pattern + i) % 256);
    }
    return payload;
}

// Helper function to verify payload pattern
static bool verify_payload(const netpipe::Message &payload, dp::usize expected_size, dp::u8 pattern = 0x00) {
    if (payload.size() != expected_size) {
        return false;
    }
    for (dp::usize i = 0; i < payload.size(); i++) {
        if (payload[i] != static_cast<dp::u8>((pattern + i) % 256)) {
            return false;
        }
    }
    return true;
}

TEST_CASE("IPC + Remote<Bidirect> - 1MB payload") {
    constexpr dp::usize PAYLOAD_SIZE = 1 * 1024 * 1024; // 1 MB
    constexpr dp::u32 METHOD_ECHO = 1;
    constexpr dp::u32 METHOD_REVERSE = 2;

    netpipe::IpcStream server_stream;
    netpipe::IpcEndpoint endpoint{"/tmp/netpipe_test_ipc_bidirect_1mb.sock"};

    auto listen_res = server_stream.listen_ipc(endpoint);
    REQUIRE(listen_res.is_ok());

    std::atomic<int> finished_count{0};

    std::thread server_thread([&]() {
        // Accept connection
        auto accept_res = server_stream.accept();
        if (!accept_res.is_ok()) {
            return;
        }
        auto client_stream = std::move(accept_res.value());

        // Create Remote<Bidirect> for server
        netpipe::Remote<netpipe::Bidirect> server_remote(*client_stream, 10, false);
        // Wait for receiver thread to start
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Register echo handler
        server_remote.register_method(
            METHOD_ECHO, [](const netpipe::Message &req) -> dp::Res<netpipe::Message> { return dp::result::ok(req); });

        // Register reverse handler
        server_remote.register_method(METHOD_REVERSE, [](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            netpipe::Message response = req;
            std::reverse(response.begin(), response.end());
            return dp::result::ok(response);
        });

        // Wait for client to complete
        std::this_thread::sleep_for(std::chrono::seconds(5));

        // Signal we're done
        finished_count++;

        // Wait for BOTH peers to finish
        while (finished_count < 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // Now both are done, safe to exit and destroy streams
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    });

    // Give server time to set up
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Client connects
    netpipe::IpcStream client_stream;
    auto connect_res = client_stream.connect_ipc(endpoint);
    REQUIRE(connect_res.is_ok());

    // Create Remote<Bidirect> for client
    netpipe::Remote<netpipe::Bidirect> client_remote(client_stream, 10, false);
    // Wait for receiver thread to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    SUBCASE("Echo 1MB payload") {
        auto payload = create_payload(PAYLOAD_SIZE, 0xAA);
        auto resp = client_remote.call(METHOD_ECHO, payload, 10000);
        REQUIRE(resp.is_ok());
        CHECK(verify_payload(resp.value(), PAYLOAD_SIZE, 0xAA));
    }

    SUBCASE("Reverse 1MB payload") {
        auto payload = create_payload(PAYLOAD_SIZE, 0xBB);
        auto resp = client_remote.call(METHOD_REVERSE, payload, 10000);
        REQUIRE(resp.is_ok());
        CHECK(resp.value().size() == PAYLOAD_SIZE);

        // Verify first and last bytes are swapped
        CHECK(resp.value()[0] == static_cast<dp::u8>((0xBB + PAYLOAD_SIZE - 1) % 256));
        CHECK(resp.value()[PAYLOAD_SIZE - 1] == 0xBB);
    }

    SUBCASE("Multiple 1MB calls") {
        for (int i = 0; i < 3; i++) {
            auto payload = create_payload(PAYLOAD_SIZE, static_cast<dp::u8>(i));
            auto resp = client_remote.call(METHOD_ECHO, payload, 10000);
            REQUIRE(resp.is_ok());
            CHECK(verify_payload(resp.value(), PAYLOAD_SIZE, static_cast<dp::u8>(i)));
        }
    }

    // Signal we're done
    finished_count++;

    // Wait for BOTH peers to finish
    while (finished_count < 2) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Now both are done, safe to exit and destroy streams
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    server_thread.join();
    server_stream.close();
}

TEST_CASE("IPC + Remote<Bidirect> - 10MB payload") {
    constexpr dp::usize PAYLOAD_SIZE = 10 * 1024 * 1024; // 10 MB
    constexpr dp::u32 METHOD_ECHO = 1;
    constexpr dp::u32 METHOD_SIZE = 2;

    netpipe::IpcStream server_stream;
    netpipe::IpcEndpoint endpoint{"/tmp/netpipe_test_ipc_bidirect_10mb.sock"};

    auto listen_res = server_stream.listen_ipc(endpoint);
    REQUIRE(listen_res.is_ok());

    std::atomic<int> finished_count{0};

    std::thread server_thread([&]() {
        auto accept_res = server_stream.accept();
        if (!accept_res.is_ok()) {
            return;
        }
        auto client_stream = std::move(accept_res.value());

        netpipe::Remote<netpipe::Bidirect> server_remote(*client_stream, 10, false);
        // Wait for receiver thread to start
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Register echo handler
        server_remote.register_method(
            METHOD_ECHO, [](const netpipe::Message &req) -> dp::Res<netpipe::Message> { return dp::result::ok(req); });

        // Register size handler - returns size of received payload
        server_remote.register_method(METHOD_SIZE, [](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            dp::u32 size = static_cast<dp::u32>(req.size());
            netpipe::Message response(4);
            response[0] = static_cast<dp::u8>((size >> 24) & 0xFF);
            response[1] = static_cast<dp::u8>((size >> 16) & 0xFF);
            response[2] = static_cast<dp::u8>((size >> 8) & 0xFF);
            response[3] = static_cast<dp::u8>(size & 0xFF);
            return dp::result::ok(response);
        });

        std::this_thread::sleep_for(std::chrono::seconds(10));

        // Signal we're done
        finished_count++;

        // Wait for BOTH peers to finish
        while (finished_count < 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // Now both are done, safe to exit and destroy streams
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    netpipe::IpcStream client_stream;
    auto connect_res = client_stream.connect_ipc(endpoint);
    REQUIRE(connect_res.is_ok());

    netpipe::Remote<netpipe::Bidirect> client_remote(client_stream, 10, false);
    // Wait for receiver thread to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    SUBCASE("Echo 10MB payload") {
        auto payload = create_payload(PAYLOAD_SIZE, 0xCC);
        auto resp = client_remote.call(METHOD_ECHO, payload, 15000);
        REQUIRE(resp.is_ok());
        CHECK(verify_payload(resp.value(), PAYLOAD_SIZE, 0xCC));
    }

    SUBCASE("Get size of 10MB payload") {
        auto payload = create_payload(PAYLOAD_SIZE, 0xDD);
        auto resp = client_remote.call(METHOD_SIZE, payload, 15000);
        REQUIRE(resp.is_ok());
        CHECK(resp.value().size() == 4);

        dp::u32 returned_size = (static_cast<dp::u32>(resp.value()[0]) << 24) |
                                (static_cast<dp::u32>(resp.value()[1]) << 16) |
                                (static_cast<dp::u32>(resp.value()[2]) << 8) | static_cast<dp::u32>(resp.value()[3]);
        CHECK(returned_size == PAYLOAD_SIZE);
    }

    SUBCASE("Sequential 10MB calls") {
        for (int i = 0; i < 2; i++) {
            auto payload = create_payload(PAYLOAD_SIZE, static_cast<dp::u8>(0x10 + i));
            auto resp = client_remote.call(METHOD_ECHO, payload, 15000);
            REQUIRE(resp.is_ok());
            CHECK(verify_payload(resp.value(), PAYLOAD_SIZE, static_cast<dp::u8>(0x10 + i)));
        }
    }

    // Signal we're done
    finished_count++;

    // Wait for BOTH peers to finish
    while (finished_count < 2) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Now both are done, safe to exit and destroy streams
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    server_thread.join();
    server_stream.close();
}

#ifdef BIG_TRANSFER
TEST_CASE("IPC + Remote<Bidirect> - 100MB payload") {
    constexpr dp::usize PAYLOAD_SIZE = 100 * 1024 * 1024; // 100 MB
    constexpr dp::u32 METHOD_ECHO = 1;
    constexpr dp::u32 METHOD_CHECKSUM = 2;

    netpipe::IpcStream server_stream;
    netpipe::IpcEndpoint endpoint{"/tmp/netpipe_test_ipc_bidirect_100mb.sock"};

    auto listen_res = server_stream.listen_ipc(endpoint);
    REQUIRE(listen_res.is_ok());

    std::atomic<int> finished_count{0};

    std::thread server_thread([&]() {
        auto accept_res = server_stream.accept();
        if (!accept_res.is_ok()) {
            return;
        }
        auto client_stream = std::move(accept_res.value());

        netpipe::Remote<netpipe::Bidirect> server_remote(*client_stream, 5, false);
        // Wait for receiver thread to start
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Register echo handler
        server_remote.register_method(
            METHOD_ECHO, [](const netpipe::Message &req) -> dp::Res<netpipe::Message> { return dp::result::ok(req); });

        // Register checksum handler - returns simple sum of all bytes
        server_remote.register_method(METHOD_CHECKSUM, [](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            dp::u64 checksum = 0;
            for (dp::u8 byte : req) {
                checksum += byte;
            }
            netpipe::Message response(8);
            for (int i = 0; i < 8; i++) {
                response[7 - i] = static_cast<dp::u8>((checksum >> (i * 8)) & 0xFF);
            }
            return dp::result::ok(response);
        });

        std::this_thread::sleep_for(std::chrono::seconds(20));

        // Signal we're done
        finished_count++;

        // Wait for BOTH peers to finish
        while (finished_count < 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // Now both are done, safe to exit and destroy streams
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    netpipe::IpcStream client_stream;
    auto connect_res = client_stream.connect_ipc(endpoint);
    REQUIRE(connect_res.is_ok());

    netpipe::Remote<netpipe::Bidirect> client_remote(client_stream, 5, false);
    // Wait for receiver thread to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    SUBCASE("Echo 100MB payload") {
        auto payload = create_payload(PAYLOAD_SIZE, 0xEE);
        auto resp = client_remote.call(METHOD_ECHO, payload, 30000);
        REQUIRE(resp.is_ok());
        CHECK(verify_payload(resp.value(), PAYLOAD_SIZE, 0xEE));
    }

    SUBCASE("Checksum 100MB payload") {
        auto payload = create_payload(PAYLOAD_SIZE, 0xFF);
        auto resp = client_remote.call(METHOD_CHECKSUM, payload, 30000);
        REQUIRE(resp.is_ok());
        CHECK(resp.value().size() == 8);

        // Verify checksum
        dp::u64 checksum = 0;
        for (int i = 0; i < 8; i++) {
            checksum |= static_cast<dp::u64>(resp.value()[7 - i]) << (i * 8);
        }
        CHECK(checksum > 0); // Basic sanity check
    }

    // Signal we're done
    finished_count++;

    // Wait for BOTH peers to finish
    while (finished_count < 2) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Now both are done, safe to exit and destroy streams
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    server_thread.join();
    server_stream.close();
}

TEST_CASE("IPC + Remote<Bidirect> - 1GB payload") {
    constexpr dp::usize PAYLOAD_SIZE = 1024 * 1024 * 1024; // 1 GB
    constexpr dp::u32 METHOD_ECHO = 1;
    constexpr dp::u32 METHOD_VERIFY = 2;

    netpipe::IpcStream server_stream;
    netpipe::IpcEndpoint endpoint{"/tmp/netpipe_test_ipc_bidirect_1gb.sock"};

    auto listen_res = server_stream.listen_ipc(endpoint);
    REQUIRE(listen_res.is_ok());

    std::atomic<int> finished_count{0};

    std::thread server_thread([&]() {
        auto accept_res = server_stream.accept();
        if (!accept_res.is_ok()) {
            return;
        }
        auto client_stream = std::move(accept_res.value());

        netpipe::Remote<netpipe::Bidirect> server_remote(*client_stream, 2, false);
        // Wait for receiver thread to start
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Register echo handler
        server_remote.register_method(
            METHOD_ECHO, [](const netpipe::Message &req) -> dp::Res<netpipe::Message> { return dp::result::ok(req); });

        // Register verify handler - checks pattern and returns OK/FAIL
        server_remote.register_method(METHOD_VERIFY, [](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            // Sample check - verify first 1000 and last 1000 bytes
            bool valid = true;
            dp::usize check_size = (req.size() > 2000) ? 1000 : req.size() / 2;

            for (dp::usize i = 0; i < check_size && valid; i++) {
                if (req[i] != static_cast<dp::u8>(i % 256)) {
                    valid = false;
                }
            }

            for (dp::usize i = req.size() - check_size; i < req.size() && valid; i++) {
                if (req[i] != static_cast<dp::u8>(i % 256)) {
                    valid = false;
                }
            }

            netpipe::Message response(1);
            response[0] = valid ? 0x01 : 0x00;
            return dp::result::ok(response);
        });

        std::this_thread::sleep_for(std::chrono::seconds(60));

        // Signal we're done
        finished_count++;

        // Wait for BOTH peers to finish
        while (finished_count < 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // Now both are done, safe to exit and destroy streams
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    netpipe::IpcStream client_stream;
    auto connect_res = client_stream.connect_ipc(endpoint);
    REQUIRE(connect_res.is_ok());

    netpipe::Remote<netpipe::Bidirect> client_remote(client_stream, 2, false);
    // Wait for receiver thread to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    SUBCASE("Echo 1GB payload") {
        auto payload = create_payload(PAYLOAD_SIZE, 0x00);
        auto resp = client_remote.call(METHOD_ECHO, payload, 60000);
        REQUIRE(resp.is_ok());
        CHECK(resp.value().size() == PAYLOAD_SIZE);

        // Verify sampling of payload
        bool valid = true;
        for (dp::usize i = 0; i < 1000 && valid; i++) {
            if (resp.value()[i] != static_cast<dp::u8>(i % 256)) {
                valid = false;
            }
        }
        CHECK(valid);
    }

    SUBCASE("Verify 1GB payload pattern") {
        auto payload = create_payload(PAYLOAD_SIZE, 0x00);
        auto resp = client_remote.call(METHOD_VERIFY, payload, 60000);
        REQUIRE(resp.is_ok());
        CHECK(resp.value().size() == 1);
        CHECK(resp.value()[0] == 0x01); // Valid pattern
    }

    // Signal we're done
    finished_count++;

    // Wait for BOTH peers to finish
    while (finished_count < 2) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Now both are done, safe to exit and destroy streams
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    server_thread.join();
    server_stream.close();
}
#endif // BIG_TRANSFER

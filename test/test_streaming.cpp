#include <atomic>
#include <chrono>
#include <doctest/doctest.h>
#include <netpipe/netpipe.hpp>
#include <thread>

TEST_CASE("Streaming RPC") {
    SUBCASE("StreamingRemote - Basic construction") {
        netpipe::TcpStream server;
        netpipe::TcpEndpoint endpoint{"127.0.0.1", 21000};

        auto listen_res = server.listen(endpoint);
        REQUIRE(listen_res.is_ok());

        std::atomic<bool> done{false};

        std::thread server_thread([&]() {
            auto accept_res = server.accept();
            if (accept_res.is_ok()) {
                auto stream = std::move(accept_res.value());
                netpipe::StreamingRemote streaming(*stream);

                CHECK(streaming.active_stream_count() == 0);

                while (!done.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        netpipe::TcpStream client;
        auto connect_res = client.connect(endpoint);
        REQUIRE(connect_res.is_ok());

        netpipe::StreamingRemote streaming(client);
        CHECK(streaming.active_stream_count() == 0);

        done = true;
        server_thread.join();
        client.close();
        server.close();
    }

    SUBCASE("Protocol supports streaming message types") {
        // Verify streaming message types exist
        netpipe::remote::MessageType stream_data = netpipe::remote::MessageType::StreamData;
        netpipe::remote::MessageType stream_end = netpipe::remote::MessageType::StreamEnd;
        netpipe::remote::MessageType stream_error = netpipe::remote::MessageType::StreamError;

        CHECK(static_cast<dp::u8>(stream_data) == 4);
        CHECK(static_cast<dp::u8>(stream_end) == 5);
        CHECK(static_cast<dp::u8>(stream_error) == 6);
    }

    SUBCASE("Protocol supports streaming flags") {
        // Verify streaming flags exist
        dp::u16 streaming_flag = netpipe::remote::MessageFlags::Streaming;
        dp::u16 final_flag = netpipe::remote::MessageFlags::Final;

        CHECK(streaming_flag == 0x0002);
        CHECK(final_flag == 0x0008);

        // Can combine flags
        dp::u16 combined = streaming_flag | final_flag;
        CHECK(combined == 0x000A);
    }

    SUBCASE("Version info is accessible") {
        // Check version constants
        CHECK(netpipe::remote::PROTOCOL_VERSION_1 == 1);
        CHECK(netpipe::remote::PROTOCOL_VERSION_2 == 2);
        CHECK(netpipe::remote::PROTOCOL_VERSION_CURRENT == 2);

        // Check library version
        auto version = netpipe::remote::LIBRARY_VERSION;
        CHECK(version.major == 0);
        CHECK(version.minor == 1);
        CHECK(version.patch == 0);

        auto version_str = netpipe::remote::get_version_string();
        CHECK(version_str == "0.1.0");

        // Check protocol support
        CHECK(netpipe::remote::is_protocol_supported(1) == true);
        CHECK(netpipe::remote::is_protocol_supported(2) == true);
        CHECK(netpipe::remote::is_protocol_supported(99) == false);

        // Check protocol names
        CHECK(dp::String(netpipe::remote::get_protocol_name(1)) == "V1 (Basic)");
        CHECK(dp::String(netpipe::remote::get_protocol_name(2)) == "V2 (Streaming)");
        CHECK(dp::String(netpipe::remote::get_protocol_name(99)) == "Unknown");
    }

    SUBCASE("StreamState structure") {
        netpipe::remote::StreamState state(123);
        CHECK(state.stream_id == 123);
        CHECK(state.completed == false);
        CHECK(state.error == false);
        CHECK(state.chunks.empty());
    }
}

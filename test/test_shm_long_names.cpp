#include <doctest/doctest.h>
#include <netpipe/stream/shm.hpp>
#include <thread>

TEST_CASE("ShmStream - Long name handling") {
    SUBCASE("Name with 200 characters (safe)") {
        std::string name_200(200, 'a');
        netpipe::ShmEndpoint endpoint{name_200.c_str(), 8192};
        netpipe::ShmStream stream;

        auto result = stream.listen_shm(endpoint);
        CHECK(result.is_ok());
        if (result.is_ok()) {
            stream.close();
        }
    }

    SUBCASE("Name with 230 characters (safe)") {
        std::string name_230(230, 'b');
        netpipe::ShmEndpoint endpoint{name_230.c_str(), 8192};
        netpipe::ShmStream stream;

        auto result = stream.listen_shm(endpoint);
        CHECK(result.is_ok());
        if (result.is_ok()) {
            stream.close();
        }
    }

    SUBCASE("Name with 240 characters (at limit)") {
        // With "/_connq" suffix (7 chars), max base name is 240 chars
        std::string name_240(240, 'c');
        netpipe::ShmEndpoint endpoint{name_240.c_str(), 8192};
        netpipe::ShmStream stream;

        auto result = stream.listen_shm(endpoint);
        CHECK(result.is_ok());
        if (result.is_ok()) {
            stream.close();
        }
    }

    SUBCASE("Name with 241 characters (exceeds limit - should fail)") {
        std::string name_241(241, 'd');
        netpipe::ShmEndpoint endpoint{name_241.c_str(), 8192};
        netpipe::ShmStream stream;

        auto result = stream.listen_shm(endpoint);
        CHECK(result.is_err());
    }

    SUBCASE("Name with 255 characters (exceeds limit - should fail gracefully)") {
        std::string name_255(255, 'e');
        netpipe::ShmEndpoint endpoint{name_255.c_str(), 8192};
        netpipe::ShmStream stream;

        auto result = stream.listen_shm(endpoint);
        CHECK(result.is_err());
    }

    SUBCASE("Name with 300 characters (way too long - should fail gracefully)") {
        std::string name_300(300, 'f');
        netpipe::ShmEndpoint endpoint{name_300.c_str(), 8192};
        netpipe::ShmStream stream;

        auto result = stream.listen_shm(endpoint);
        CHECK(result.is_err());
    }

    SUBCASE("Connect to long name with accept") {
        std::string name_200(200, 'g');
        netpipe::ShmEndpoint endpoint{name_200.c_str(), 8192};

        netpipe::ShmStream listener;
        auto listen_res = listener.listen_shm(endpoint);
        REQUIRE(listen_res.is_ok());

        std::unique_ptr<netpipe::Stream> server_conn;

        std::thread accept_thread([&]() {
            auto accept_res = listener.accept();
            REQUIRE(accept_res.is_ok());
            server_conn = std::move(accept_res.value());
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        netpipe::ShmStream connector;
        auto connect_res = connector.connect_shm(endpoint);
        CHECK(connect_res.is_ok());

        accept_thread.join();

        connector.close();
        if (server_conn) server_conn->close();
        listener.close();
    }
}

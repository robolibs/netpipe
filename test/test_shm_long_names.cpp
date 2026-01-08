#include <doctest/doctest.h>
#include <netpipe/stream/shm.hpp>

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

    SUBCASE("Name with 250 characters (safe)") {
        std::string name_250(250, 'b');
        netpipe::ShmEndpoint endpoint{name_250.c_str(), 8192};
        netpipe::ShmStream stream;

        auto result = stream.listen_shm(endpoint);
        CHECK(result.is_ok());
        if (result.is_ok()) {
            stream.close();
        }
    }

    SUBCASE("Name with 254 characters (at POSIX limit with '/')") {
        std::string name_254(254, 'c');
        netpipe::ShmEndpoint endpoint{name_254.c_str(), 8192};
        netpipe::ShmStream stream;

        auto result = stream.listen_shm(endpoint);
        CHECK(result.is_ok());
        if (result.is_ok()) {
            stream.close();
        }
    }

    SUBCASE("Name with 255 characters (exceeds limit - should fail gracefully)") {
        std::string name_255(255, 'd');
        netpipe::ShmEndpoint endpoint{name_255.c_str(), 8192};
        netpipe::ShmStream stream;

        // This should fail because with '/' prefix it becomes 256 chars
        auto result = stream.listen_shm(endpoint);
        CHECK(result.is_err());
    }

    SUBCASE("Name with 300 characters (way too long - should fail gracefully)") {
        std::string name_300(300, 'e');
        netpipe::ShmEndpoint endpoint{name_300.c_str(), 8192};
        netpipe::ShmStream stream;

        auto result = stream.listen_shm(endpoint);
        CHECK(result.is_err());
    }

    SUBCASE("Connect to long name") {
        std::string name_250(250, 'f');
        netpipe::ShmEndpoint endpoint{name_250.c_str(), 8192};

        netpipe::ShmStream creator;
        auto listen_res = creator.listen_shm(endpoint);
        REQUIRE(listen_res.is_ok());

        netpipe::ShmStream connector;
        auto connect_res = connector.connect_shm(endpoint);
        CHECK(connect_res.is_ok());

        connector.close();
        creator.close();
    }
}

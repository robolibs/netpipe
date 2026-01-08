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

    SUBCASE("Name with 250 characters (at limit with suffix)") {
        // With "/" prefix and "_c2s" suffix, max base name is 250 chars (1+250+4=255)
        std::string name_250_limit(250, 'c');
        netpipe::ShmEndpoint endpoint{name_250_limit.c_str(), 8192};
        netpipe::ShmStream stream;

        auto result = stream.listen_shm(endpoint);
        CHECK(result.is_ok());
        if (result.is_ok()) {
            stream.close();
        }
    }

    SUBCASE("Name with 251 characters (exceeds limit - should fail)") {
        std::string name_251(251, 'c');
        netpipe::ShmEndpoint endpoint{name_251.c_str(), 8192};
        netpipe::ShmStream stream;

        auto result = stream.listen_shm(endpoint);
        CHECK(result.is_err());
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

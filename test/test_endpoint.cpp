#include <doctest/doctest.h>
#include <netpipe/endpoint.hpp>

TEST_CASE("TcpEndpoint") {
    SUBCASE("Create and convert to string") {
        netpipe::TcpEndpoint endpoint{"127.0.0.1", 8080};
        CHECK(endpoint.host == "127.0.0.1");
        CHECK(endpoint.port == 8080);
        CHECK(endpoint.to_string() == "127.0.0.1:8080");
    }

    SUBCASE("Different hosts and ports") {
        netpipe::TcpEndpoint ep1{"localhost", 3000};
        CHECK(ep1.to_string() == "localhost:3000");

        netpipe::TcpEndpoint ep2{"0.0.0.0", 65535};
        CHECK(ep2.to_string() == "0.0.0.0:65535");

        netpipe::TcpEndpoint ep3{"192.168.1.100", 1};
        CHECK(ep3.to_string() == "192.168.1.100:1");
    }
}

TEST_CASE("UdpEndpoint") {
    SUBCASE("Create and convert to string") {
        netpipe::UdpEndpoint endpoint{"127.0.0.1", 9090};
        CHECK(endpoint.host == "127.0.0.1");
        CHECK(endpoint.port == 9090);
        CHECK(endpoint.to_string() == "127.0.0.1:9090");
    }

    SUBCASE("Broadcast address") {
        netpipe::UdpEndpoint endpoint{"255.255.255.255", 7447};
        CHECK(endpoint.to_string() == "255.255.255.255:7447");
    }
}

TEST_CASE("IpcEndpoint") {
    SUBCASE("Create and convert to string") {
        netpipe::IpcEndpoint endpoint{"/tmp/test.sock"};
        CHECK(endpoint.path == "/tmp/test.sock");
        CHECK(endpoint.to_string() == "/tmp/test.sock");
    }

    SUBCASE("Different paths") {
        netpipe::IpcEndpoint ep1{"/var/run/myapp.sock"};
        CHECK(ep1.to_string() == "/var/run/myapp.sock");

        netpipe::IpcEndpoint ep2{"./local.sock"};
        CHECK(ep2.to_string() == "./local.sock");
    }
}

TEST_CASE("ShmEndpoint") {
    SUBCASE("Create and convert to string") {
        netpipe::ShmEndpoint endpoint{"my_shm", 4096};
        CHECK(endpoint.name == "my_shm");
        CHECK(endpoint.size == 4096);
        CHECK(endpoint.to_string() == "my_shm (size=4096)");
    }

    SUBCASE("Different sizes") {
        netpipe::ShmEndpoint ep1{"buffer1", 1024};
        CHECK(ep1.to_string() == "buffer1 (size=1024)");

        netpipe::ShmEndpoint ep2{"large_buffer", 1048576};
        CHECK(ep2.to_string() == "large_buffer (size=1048576)");
    }
}

TEST_CASE("LoraEndpoint") {
    SUBCASE("Create and convert to string") {
        netpipe::LoraEndpoint endpoint{"2001:db8::42"};
        CHECK(endpoint.ipv6 == "2001:db8::42");
        CHECK(endpoint.to_string() == "2001:db8::42");
    }

    SUBCASE("Different IPv6 addresses") {
        netpipe::LoraEndpoint ep1{"fe80::1"};
        CHECK(ep1.to_string() == "fe80::1");

        netpipe::LoraEndpoint ep2{"::1"};
        CHECK(ep2.to_string() == "::1");
    }
}

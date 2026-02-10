#include <doctest/doctest.h>

#include <netpipe/protocol/http1.hpp>
#include <netpipe/protocol/http2.hpp>

TEST_CASE("HTTP/1.1 conformance smoke") {
    auto req = netpipe::http1::parse_request_head("GET /index.html HTTP/1.1\r\n"
                                                   "Host: example.com\r\n"
                                                   "\r\n");
    CHECK(req.is_ok());

    auto bad_version = netpipe::http1::parse_request_head("GET / HTTP/1.0\r\n"
                                                           "Host: example.com\r\n"
                                                           "\r\n");
    CHECK(bad_version.is_err());

    auto bad_obs_fold = netpipe::http1::parse_request_head("GET / HTTP/1.1\r\n"
                                                            "X-Test: a\r\n"
                                                            " b\r\n"
                                                            "\r\n");
    CHECK(bad_obs_fold.is_err());
}

TEST_CASE("HTTP/2 conformance smoke") {
    netpipe::http2::FrameHeader settings;
    settings.type = netpipe::http2::FrameType::Settings;
    settings.stream_id = 0;
    CHECK(netpipe::http2::validate_frame_header(settings).is_ok());

    netpipe::http2::FrameHeader bad_settings = settings;
    bad_settings.stream_id = 1;
    CHECK(netpipe::http2::validate_frame_header(bad_settings).is_err());

    netpipe::http2::HpackContext hpack;
    netpipe::http::HeaderList headers = {
        {":method", "GET"},
        {":scheme", "https"},
        {":path", "/"},
    };
    auto encoded = hpack.encode(headers);
    REQUIRE(encoded.is_ok());
    auto decoded = hpack.decode(encoded.value());
    CHECK(decoded.is_ok());
}

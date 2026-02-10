#include <doctest/doctest.h>

#include <netpipe/protocol/http1/body.hpp>

TEST_CASE("HTTP/1.1 content-length body decoding") {
    netpipe::http::HeaderList headers;
    netpipe::http1::set_content_length(headers, 5);

    dp::Vector<dp::u8> payload{'h', 'e', 'l', 'l', 'o', '!'};
    auto decoded = netpipe::http1::decode_content_length_body(headers, payload);
    CHECK(decoded.is_ok());
    CHECK(decoded.value().size() == 5);
}

TEST_CASE("HTTP/1.1 chunked body encode/decode roundtrip") {
    dp::Vector<dp::u8> body{'n', 'e', 't', 'p', 'i', 'p', 'e'};
    netpipe::http::HeaderList trailers = {{"x-checksum", "7"}};

    auto encoded = netpipe::http1::encode_chunked_body(body, trailers);
    CHECK(encoded.is_ok());

    auto decoded = netpipe::http1::decode_chunked_body_ex(encoded.value());
    CHECK(decoded.is_ok());
    CHECK(decoded.value().body == body);
    REQUIRE(decoded.value().trailers.size() == 1);
    CHECK(decoded.value().trailers[0].name == "x-checksum");
    CHECK(decoded.value().trailers[0].value == "7");
}

TEST_CASE("HTTP/1.1 empty-body response rules") {
    CHECK(netpipe::http1::response_must_not_have_body(101));
    CHECK(netpipe::http1::response_must_not_have_body(204));
    CHECK(netpipe::http1::response_must_not_have_body(304));
    CHECK(netpipe::http1::response_must_not_have_body(200, netpipe::http::Method::Head));
    CHECK(netpipe::http1::response_must_not_have_body(200, netpipe::http::Method::Get) == false);
}

TEST_CASE("HTTP/1.1 keep-alive model") {
    netpipe::http::HeaderList headers;
    CHECK(netpipe::http1::should_keep_alive(headers));

    netpipe::http1::set_header(headers, "Connection", "close");
    CHECK(netpipe::http1::should_keep_alive(headers) == false);

    netpipe::http1::set_header(headers, "Connection", "keep-alive");
    CHECK(netpipe::http1::should_keep_alive(headers));
}

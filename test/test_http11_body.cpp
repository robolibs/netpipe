#include <doctest/doctest.h>

#include <netpipe/protocol/http11/body.hpp>

TEST_CASE("HTTP/1.1 content-length body decoding") {
    netpipe::http::HeaderList headers;
    netpipe::http11::set_content_length(headers, 5);

    dp::Vector<dp::u8> payload{'h', 'e', 'l', 'l', 'o', '!'};
    auto decoded = netpipe::http11::decode_content_length_body(headers, payload);
    CHECK(decoded.is_ok());
    CHECK(decoded.value().size() == 5);
}

TEST_CASE("HTTP/1.1 chunked body encode/decode roundtrip") {
    dp::Vector<dp::u8> body{'n', 'e', 't', 'p', 'i', 'p', 'e'};

    auto encoded = netpipe::http11::encode_chunked_body(body);
    CHECK(encoded.is_ok());

    auto decoded = netpipe::http11::decode_chunked_body(encoded.value());
    CHECK(decoded.is_ok());
    CHECK(decoded.value() == body);
}

TEST_CASE("HTTP/1.1 empty-body response rules") {
    CHECK(netpipe::http11::response_must_not_have_body(101));
    CHECK(netpipe::http11::response_must_not_have_body(204));
    CHECK(netpipe::http11::response_must_not_have_body(304));
    CHECK(netpipe::http11::response_must_not_have_body(200, netpipe::http::Method::Head));
    CHECK(netpipe::http11::response_must_not_have_body(200, netpipe::http::Method::Get) == false);
}

TEST_CASE("HTTP/1.1 keep-alive model") {
    netpipe::http::HeaderList headers;
    CHECK(netpipe::http11::should_keep_alive(headers));

    netpipe::http11::set_header(headers, "Connection", "close");
    CHECK(netpipe::http11::should_keep_alive(headers) == false);

    netpipe::http11::set_header(headers, "Connection", "keep-alive");
    CHECK(netpipe::http11::should_keep_alive(headers));
}

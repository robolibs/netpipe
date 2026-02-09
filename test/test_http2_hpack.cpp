#include <doctest/doctest.h>

#include <netpipe/protocol/http2/hpack.hpp>

TEST_CASE("HTTP/2 HPACK decodes indexed static header") {
    netpipe::http2::HpackContext ctx;

    // Indexed header field with index 2 => :method: GET
    dp::Vector<dp::u8> block{0x82};
    auto decoded = ctx.decode(block);
    CHECK(decoded.is_ok());
    REQUIRE(decoded.value().size() == 1);
    CHECK(decoded.value()[0].name == ":method");
    CHECK(decoded.value()[0].value == "GET");
}

TEST_CASE("HTTP/2 HPACK encode/decode roundtrip") {
    netpipe::http2::HpackContext encoder;
    netpipe::http2::HpackContext decoder;

    netpipe::http::HeaderList headers;
    headers.push_back(netpipe::http::HeaderField(":method", "GET"));
    headers.push_back(netpipe::http::HeaderField(":scheme", "https"));
    headers.push_back(netpipe::http::HeaderField(":path", "/"));
    headers.push_back(netpipe::http::HeaderField("content-type", "application/json"));

    auto encoded = encoder.encode(headers);
    CHECK(encoded.is_ok());

    auto decoded = decoder.decode(encoded.value());
    CHECK(decoded.is_ok());
    REQUIRE(decoded.value().size() == headers.size());
    CHECK(decoded.value()[0].name == ":method");
    CHECK(decoded.value()[3].name == "content-type");
    CHECK(decoded.value()[3].value == "application/json");
}

TEST_CASE("HTTP/2 HPACK dynamic table helps second block") {
    netpipe::http2::HpackContext encoder;
    netpipe::http2::HpackContext decoder;

    netpipe::http::HeaderList headers;
    headers.push_back(netpipe::http::HeaderField("x-request-id", "abc-123"));

    auto first = encoder.encode(headers);
    REQUIRE(first.is_ok());
    auto decoded_first = decoder.decode(first.value());
    REQUIRE(decoded_first.is_ok());

    auto second = encoder.encode(headers);
    REQUIRE(second.is_ok());
    auto decoded_second = decoder.decode(second.value());
    REQUIRE(decoded_second.is_ok());

    CHECK(second.value().size() < first.value().size());
    CHECK(decoded_second.value()[0].name == "x-request-id");
    CHECK(decoded_second.value()[0].value == "abc-123");
}

TEST_CASE("HTTP/2 HPACK rejects huffman string for now") {
    netpipe::http2::HpackContext ctx;

    // Literal with indexing and new name, but name length uses huffman flag.
    // 0x40 => literal with indexing and name index 0
    // 0x81 => huffman=1, len=1
    dp::Vector<dp::u8> block{0x40, 0x81, 0x61, 0x01, 0x62};
    auto decoded = ctx.decode(block);
    CHECK(decoded.is_err());
}

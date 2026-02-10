#include <doctest/doctest.h>

#include <netpipe/protocol/http2/hpack.hpp>
#include <netpipe/protocol/http3/qpack.hpp>

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

TEST_CASE("HTTP/2 HPACK decodes huffman-encoded literal header") {
    netpipe::http2::HpackContext ctx;

    auto name_h = netpipe::http3::huffman_encoder().encode("x-name");
    auto value_h = netpipe::http3::huffman_encoder().encode("x-value");

    // Literal Header Field with Incremental Indexing (new name)
    // name-len byte has H bit set, same for value-len.
    dp::Vector<dp::u8> block;
    block.push_back(0x40);
    block.push_back(static_cast<dp::u8>(0x80 | name_h.size()));
    block.insert(block.end(), name_h.begin(), name_h.end());
    block.push_back(static_cast<dp::u8>(0x80 | value_h.size()));
    block.insert(block.end(), value_h.begin(), value_h.end());

    auto decoded = ctx.decode(block);
    REQUIRE(decoded.is_ok());
    REQUIRE(decoded.value().size() == 1);
    CHECK(decoded.value()[0].name == "x-name");
    CHECK(decoded.value()[0].value == "x-value");
}

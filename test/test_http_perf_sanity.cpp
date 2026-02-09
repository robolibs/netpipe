#include <chrono>

#include <doctest/doctest.h>

#include <netpipe/protocol/http11.hpp>
#include <netpipe/protocol/http2.hpp>

TEST_CASE("HTTP performance sanity: http11 serialize/parse loop") {
    netpipe::http11::Request req;
    req.method = netpipe::http::Method::Post;
    req.target = "/perf";
    netpipe::http11::set_header(req.headers, "Host", "bench.local");
    netpipe::http11::set_header(req.headers, "Content-Type", "application/json");
    req.body = dp::Vector<dp::u8>{'{', '}', '\n'};

    auto start = std::chrono::steady_clock::now();
    dp::usize bytes_processed = 0;
    for (int i = 0; i < 2000; ++i) {
        auto wire = netpipe::http11::ClientConnection{}.encode_request(req);
        REQUIRE(wire.is_ok());
        bytes_processed += wire.value().size();

        auto parsed = netpipe::http11::ServerConnection{}.decode_request(wire.value());
        REQUIRE(parsed.is_ok());
    }
    auto end = std::chrono::steady_clock::now();
    auto micros = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    CHECK(bytes_processed > 0);
    CHECK(micros >= 0);
}

TEST_CASE("HTTP performance sanity: http2 frame and hpack loop") {
    netpipe::http2::HpackContext encoder;
    netpipe::http2::HpackContext decoder;

    netpipe::http::HeaderList headers = {
        {":method", "GET"},
        {":scheme", "https"},
        {":path", "/bench"},
        {":authority", "bench.local"},
        {"accept", "application/json"},
    };

    auto start = std::chrono::steady_clock::now();
    dp::usize bytes_processed = 0;
    for (int i = 0; i < 2000; ++i) {
        auto block = encoder.encode(headers);
        REQUIRE(block.is_ok());

        netpipe::http2::Frame frame;
        frame.header.type = netpipe::http2::FrameType::Headers;
        frame.header.stream_id = 1;
        frame.header.flags = 0x4;
        frame.payload = block.value();

        auto wire = netpipe::http2::serialize_frame(frame);
        REQUIRE(wire.is_ok());
        bytes_processed += wire.value().size();

        auto parsed = netpipe::http2::parse_frame(wire.value().data(), wire.value().size());
        REQUIRE(parsed.is_ok());

        auto decoded = decoder.decode(parsed.value().first.payload);
        REQUIRE(decoded.is_ok());
    }
    auto end = std::chrono::steady_clock::now();
    auto micros = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    CHECK(bytes_processed > 0);
    CHECK(micros >= 0);
}

TEST_CASE("HTTP memory sanity: hpack dynamic table bounded") {
    netpipe::http2::HpackContext ctx;
    ctx.set_max_table_size(128);

    for (int i = 0; i < 256; ++i) {
        netpipe::http::HeaderList headers;
        headers.push_back(netpipe::http::HeaderField("x-k", "x-value-that-should-trigger-eviction"));
        auto encoded = ctx.encode(headers);
        REQUIRE(encoded.is_ok());
    }

    CHECK(ctx.dynamic_table_size() <= ctx.max_table_size());
}

#include <doctest/doctest.h>

#include <netpipe/protocol/http2/stream.hpp>

namespace {

    netpipe::http2::Frame make_frame(netpipe::http2::FrameType type, dp::u32 stream_id, dp::u8 flags,
                                     const dp::Vector<dp::u8> &payload) {
        netpipe::http2::Frame frame;
        frame.header.type = type;
        frame.header.stream_id = stream_id;
        frame.header.flags = flags;
        frame.payload = payload;
        return frame;
    }

} // namespace

TEST_CASE("HTTP/2 HEADERS and CONTINUATION reassembly") {
    netpipe::http2::HpackContext encoder;
    netpipe::http2::StreamManager manager;

    netpipe::http::HeaderList headers = {
        {":method", "GET"},
        {":scheme", "https"},
        {":path", "/items"},
        {"accept", "application/json"},
    };

    auto block = encoder.encode(headers);
    REQUIRE(block.is_ok());
    REQUIRE(block.value().size() > 3);

    dp::usize cut = block.value().size() / 2;
    dp::Vector<dp::u8> part1(block.value().begin(), block.value().begin() + cut);
    dp::Vector<dp::u8> part2(block.value().begin() + cut, block.value().end());

    auto res1 = manager.process_incoming_frame(make_frame(netpipe::http2::FrameType::Headers, 1, 0x0, part1));
    REQUIRE(res1.is_ok());
    CHECK(res1.value().has_value() == false);

    auto res2 = manager.process_incoming_frame(make_frame(netpipe::http2::FrameType::Continuation, 1, 0x4, part2));
    REQUIRE(res2.is_ok());
    REQUIRE(res2.value().has_value());
    CHECK(res2.value().value().stream_id == 1);
    CHECK(res2.value().value().headers.size() == headers.size());
    CHECK(manager.state(1) == netpipe::http2::StreamState::Open);
}

TEST_CASE("HTTP/2 END_STREAM transitions to half-closed-remote") {
    netpipe::http2::HpackContext encoder;
    netpipe::http2::StreamManager manager;

    netpipe::http::HeaderList headers = {
        {":method", "GET"},
        {":scheme", "https"},
        {":path", "/done"},
    };

    auto block = encoder.encode(headers);
    REQUIRE(block.is_ok());

    auto head_res = manager.process_incoming_frame(
        make_frame(netpipe::http2::FrameType::Headers, 3, 0x5, block.value())); // END_STREAM | END_HEADERS
    REQUIRE(head_res.is_ok());
    REQUIRE(head_res.value().has_value());
    CHECK(head_res.value().value().end_stream);
    CHECK(manager.state(3) == netpipe::http2::StreamState::HalfClosedRemote);

    auto data_res = manager.process_incoming_frame(make_frame(netpipe::http2::FrameType::Data, 3, 0x0, {'x'}));
    CHECK(data_res.is_err());
}

TEST_CASE("HTTP/2 CONTINUATION stream mismatch is rejected") {
    netpipe::http2::HpackContext encoder;
    netpipe::http2::StreamManager manager;

    netpipe::http::HeaderList headers = {
        {":method", "GET"},
        {":scheme", "https"},
        {":path", "/"},
    };

    auto block = encoder.encode(headers);
    REQUIRE(block.is_ok());
    dp::usize cut = block.value().size() / 2;

    auto res1 = manager.process_incoming_frame(
        make_frame(netpipe::http2::FrameType::Headers, 5, 0x0, {block.value().begin(), block.value().begin() + cut}));
    REQUIRE(res1.is_ok());

    auto res2 = manager.process_incoming_frame(make_frame(netpipe::http2::FrameType::Continuation, 7, 0x4,
                                                          {block.value().begin() + cut, block.value().end()}));
    CHECK(res2.is_err());
}

TEST_CASE("HTTP/2 pseudo-header order is enforced") {
    netpipe::http2::HpackContext encoder;
    netpipe::http2::StreamManager manager;

    netpipe::http::HeaderList invalid = {
        {"accept", "*/*"},
        {":method", "GET"},
        {":scheme", "https"},
        {":path", "/"},
    };

    auto block = encoder.encode(invalid);
    REQUIRE(block.is_ok());

    auto res = manager.process_incoming_frame(make_frame(netpipe::http2::FrameType::Headers, 9, 0x4, block.value()));
    CHECK(res.is_err());
}

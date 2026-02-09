#include <doctest/doctest.h>

#include <netpipe/protocol/http2/frame.hpp>

TEST_CASE("HTTP/2 frame header serialization and parse") {
    netpipe::http2::FrameHeader header;
    header.length = 5;
    header.type = netpipe::http2::FrameType::Data;
    header.flags = 0x1;
    header.stream_id = 3;

    auto bytes = netpipe::http2::serialize_frame_header(header);
    CHECK(bytes.size() == 9);

    auto parsed = netpipe::http2::parse_frame_header(bytes.data(), bytes.size());
    CHECK(parsed.is_ok());
    CHECK(parsed.value().first.length == 5);
    CHECK(parsed.value().first.type == netpipe::http2::FrameType::Data);
    CHECK(parsed.value().first.flags == 0x1);
    CHECK(parsed.value().first.stream_id == 3);
}

TEST_CASE("HTTP/2 frame roundtrip") {
    netpipe::http2::Frame frame;
    frame.header.type = netpipe::http2::FrameType::Headers;
    frame.header.flags = 0x4;
    frame.header.stream_id = 1;
    frame.payload = dp::Vector<dp::u8>{'a', 'b', 'c'};

    auto serialized = netpipe::http2::serialize_frame(frame);
    CHECK(serialized.is_ok());

    auto parsed = netpipe::http2::parse_frame(serialized.value().data(), serialized.value().size());
    CHECK(parsed.is_ok());
    CHECK(parsed.value().first.header.length == 3);
    CHECK(parsed.value().first.header.type == netpipe::http2::FrameType::Headers);
    CHECK(parsed.value().first.payload == frame.payload);
}

TEST_CASE("HTTP/2 frame validation for control stream rules") {
    netpipe::http2::FrameHeader settings;
    settings.type = netpipe::http2::FrameType::Settings;
    settings.stream_id = 1;
    CHECK(netpipe::http2::validate_frame_header(settings).is_err());

    netpipe::http2::FrameHeader data;
    data.type = netpipe::http2::FrameType::Data;
    data.stream_id = 0;
    CHECK(netpipe::http2::validate_frame_header(data).is_err());

    netpipe::http2::FrameHeader ping;
    ping.type = netpipe::http2::FrameType::Ping;
    ping.stream_id = 0;
    CHECK(netpipe::http2::validate_frame_header(ping).is_ok());
}

#include <doctest/doctest.h>

#include <cstring>

#include <netpipe/protocol/http2/connection.hpp>

namespace {

    netpipe::http2::Frame make_frame(netpipe::http2::FrameType type, dp::u32 stream_id, dp::u8 flags,
                                     const dp::Vector<dp::u8> &payload = {}) {
        netpipe::http2::Frame frame;
        frame.header.type = type;
        frame.header.stream_id = stream_id;
        frame.header.flags = flags;
        frame.payload = payload;
        return frame;
    }

} // namespace

TEST_CASE("HTTP/2 connection client startup queues preface and settings") {
    netpipe::http2::Connection client(true);
    CHECK(client.is_client());
    CHECK(client.state() == netpipe::http2::ConnectionState::Idle);

    netpipe::http2::Settings settings;
    settings.set(netpipe::http2::SettingsId::InitialWindowSize, 65535);

    auto start = client.start(settings);
    REQUIRE(start.is_ok());
    CHECK(client.state() == netpipe::http2::ConnectionState::PrefaceSent);
    CHECK(client.has_outbound());

    auto outbound = client.pop_outbound();
    REQUIRE(outbound.is_ok());
    REQUIRE(outbound.value().size() > std::strlen(netpipe::http2::PREFACE));
    CHECK(std::memcmp(outbound.value().data(), netpipe::http2::PREFACE, std::strlen(netpipe::http2::PREFACE)) == 0);
}

TEST_CASE("HTTP/2 connection server accepts preface and sends settings") {
    netpipe::http2::Connection server(false);
    CHECK(server.is_client() == false);

    netpipe::Message preface(netpipe::http2::PREFACE, netpipe::http2::PREFACE + std::strlen(netpipe::http2::PREFACE));
    auto accept = server.accept_client_preface(preface);
    REQUIRE(accept.is_ok());
    CHECK(server.state() == netpipe::http2::ConnectionState::PrefaceReceived);

    netpipe::http2::Settings local;
    local.set(netpipe::http2::SettingsId::MaxFrameSize, 16384);

    auto send = server.send_server_settings(local);
    REQUIRE(send.is_ok());
    CHECK(server.has_outbound());

    auto outbound = server.pop_outbound();
    REQUIRE(outbound.is_ok());

    auto parsed = netpipe::http2::parse_frame(outbound.value().data(), outbound.value().size());
    REQUIRE(parsed.is_ok());
    CHECK(parsed.value().first.header.type == netpipe::http2::FrameType::Settings);
}

TEST_CASE("HTTP/2 connection server can process inbound startup bytes") {
    netpipe::http2::Connection client(true);
    netpipe::http2::Connection server(false);

    REQUIRE(client.start().is_ok());
    auto client_start = client.pop_outbound();
    REQUIRE(client_start.is_ok());

    auto process = server.process_inbound_bytes(client_start.value());
    REQUIRE(process.is_ok());
    CHECK(server.state() == netpipe::http2::ConnectionState::PrefaceReceived);
    CHECK(server.has_outbound());

    auto ack = server.pop_outbound();
    REQUIRE(ack.is_ok());
    auto parsed = netpipe::http2::parse_frame(ack.value().data(), ack.value().size());
    REQUIRE(parsed.is_ok());
    CHECK(parsed.value().first.header.type == netpipe::http2::FrameType::Settings);
}

TEST_CASE("HTTP/2 connection emits structured inbound events") {
    netpipe::http2::Connection conn(false);

    netpipe::Message preface(netpipe::http2::PREFACE, netpipe::http2::PREFACE + std::strlen(netpipe::http2::PREFACE));
    REQUIRE(conn.accept_client_preface(preface).is_ok());

    netpipe::http2::Settings settings;
    settings.set(netpipe::http2::SettingsId::HeaderTableSize, 4096);
    auto settings_frame = netpipe::http2::make_settings_frame(settings);
    REQUIRE(conn.process_inbound_frame(settings_frame).is_ok());
    REQUIRE(conn.has_event());

    auto settings_event = conn.pop_event();
    REQUIRE(settings_event.is_ok());
    CHECK(settings_event.value().type == netpipe::http2::InboundEventType::Settings);
    CHECK(settings_event.value().settings.has_value());

    netpipe::http2::HpackContext encoder;
    netpipe::http::HeaderList headers = {
        {":method", "GET"},
        {":scheme", "https"},
        {":path", "/resource"},
        {":authority", "example.com"},
    };
    auto encoded = encoder.encode(headers);
    REQUIRE(encoded.is_ok());

    REQUIRE(
        conn.process_inbound_frame(make_frame(netpipe::http2::FrameType::Headers, 1, 0x4, encoded.value())).is_ok());
    REQUIRE(conn.has_event());
    auto headers_event = conn.pop_event();
    REQUIRE(headers_event.is_ok());
    CHECK(headers_event.value().type == netpipe::http2::InboundEventType::Headers);
    CHECK(headers_event.value().stream_id == 1);
    CHECK(headers_event.value().headers.size() == headers.size());

    REQUIRE(conn.process_inbound_frame(make_frame(netpipe::http2::FrameType::Data, 1, 0x1, {'x', 'y'})).is_ok());
    REQUIRE(conn.has_event());
    auto data_event = conn.pop_event();
    REQUIRE(data_event.is_ok());
    CHECK(data_event.value().type == netpipe::http2::InboundEventType::Data);
    CHECK(data_event.value().stream_id == 1);
    CHECK(data_event.value().end_stream);
    CHECK(data_event.value().data.size() == 2);

    REQUIRE(
        conn.process_inbound_frame(make_frame(netpipe::http2::FrameType::RstStream, 3, 0x0, {0x00, 0x00, 0x00, 0x08}))
            .is_ok());
    REQUIRE(conn.has_event());
    auto rst_event = conn.pop_event();
    REQUIRE(rst_event.is_ok());
    CHECK(rst_event.value().type == netpipe::http2::InboundEventType::RstStream);
    CHECK(rst_event.value().rst_error.has_value());

    netpipe::http2::GoAway goaway;
    goaway.last_stream_id = 5;
    goaway.error_code = netpipe::http2::ErrorCode::NoError;
    REQUIRE(conn.process_inbound_frame(netpipe::http2::make_goaway(goaway)).is_ok());
    REQUIRE(conn.has_event());
    auto goaway_event = conn.pop_event();
    REQUIRE(goaway_event.is_ok());
    CHECK(goaway_event.value().type == netpipe::http2::InboundEventType::GoAway);
    CHECK(goaway_event.value().goaway.has_value());
    CHECK(goaway_event.value().goaway.value().last_stream_id == 5);
}

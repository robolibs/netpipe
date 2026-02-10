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

    void complete_settings_startup(netpipe::http2::Connection &client, netpipe::http2::Connection &server) {
        REQUIRE(client.start().is_ok());

        auto client_start = client.pop_outbound();
        REQUIRE(client_start.is_ok());
        REQUIRE(server.process_inbound_bytes(client_start.value()).is_ok());

        REQUIRE(server.send_server_settings().is_ok());

        // server has ACK(to client settings) + server SETTINGS
        auto server_msg1 = server.pop_outbound();
        REQUIRE(server_msg1.is_ok());
        REQUIRE(client.process_inbound_bytes(server_msg1.value()).is_ok());

        auto server_msg2 = server.pop_outbound();
        REQUIRE(server_msg2.is_ok());
        REQUIRE(client.process_inbound_bytes(server_msg2.value()).is_ok());

        // client ACK(to server settings)
        auto client_ack = client.pop_outbound();
        REQUIRE(client_ack.is_ok());
        REQUIRE(server.process_inbound_bytes(client_ack.value()).is_ok());

        CHECK(client.startup_complete());
        CHECK(server.startup_complete());

        while (client.has_event()) {
            auto ev = client.pop_event();
            REQUIRE(ev.is_ok());
        }
        while (server.has_event()) {
            auto ev = server.pop_event();
            REQUIRE(ev.is_ok());
        }
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
    netpipe::http2::Connection client(true);
    netpipe::http2::Connection conn(false);
    complete_settings_startup(client, conn);

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

TEST_CASE("HTTP/2 connection request response helper roundtrip") {
    netpipe::http2::Connection client(true);
    netpipe::http2::Connection server(false);

    complete_settings_startup(client, server);

    auto stream_id_result = client.open_request_stream();
    REQUIRE(stream_id_result.is_ok());
    dp::u32 stream_id = stream_id_result.value();
    CHECK((stream_id % 2) == 1);

    netpipe::http2::Request request;
    request.method = netpipe::http::Method::Get;
    request.scheme = "https";
    request.authority = "example.com";
    request.path = "/v1/ping";
    request.headers.push_back({"accept", "application/json"});

    REQUIRE(client.send_request_headers(stream_id, request, false).is_ok());
    REQUIRE(client.send_data(stream_id, {'p', 'i', 'n', 'g'}, true).is_ok());

    auto req_headers_wire = client.pop_outbound();
    REQUIRE(req_headers_wire.is_ok());
    auto req_frame = netpipe::http2::parse_frame(req_headers_wire.value().data(), req_headers_wire.value().size());
    REQUIRE(req_frame.is_ok());
    REQUIRE(server.process_inbound_frame(req_frame.value().first).is_ok());

    auto req_data_wire = client.pop_outbound();
    REQUIRE(req_data_wire.is_ok());
    auto req_data_frame = netpipe::http2::parse_frame(req_data_wire.value().data(), req_data_wire.value().size());
    REQUIRE(req_data_frame.is_ok());
    REQUIRE(server.process_inbound_frame(req_data_frame.value().first).is_ok());

    REQUIRE(server.has_event());
    auto ev1 = server.pop_event();
    REQUIRE(ev1.is_ok());
    CHECK(ev1.value().type == netpipe::http2::InboundEventType::Headers);
    CHECK(ev1.value().stream_id == stream_id);

    REQUIRE(server.has_event());
    auto ev2 = server.pop_event();
    REQUIRE(ev2.is_ok());
    CHECK(ev2.value().type == netpipe::http2::InboundEventType::Data);
    CHECK(ev2.value().end_stream);
    CHECK(ev2.value().data.size() == 4);

    netpipe::http2::Response response;
    response.status_code = 200;
    response.headers.push_back({"content-type", "application/json"});

    REQUIRE(server.send_response_headers(stream_id, response, false).is_ok());
    REQUIRE(server.send_data(stream_id, {'o', 'k'}, true).is_ok());

    auto resp_headers_wire = server.pop_outbound();
    REQUIRE(resp_headers_wire.is_ok());
    auto resp_headers_frame =
        netpipe::http2::parse_frame(resp_headers_wire.value().data(), resp_headers_wire.value().size());
    REQUIRE(resp_headers_frame.is_ok());
    REQUIRE(client.process_inbound_frame(resp_headers_frame.value().first).is_ok());

    auto resp_data_wire = server.pop_outbound();
    REQUIRE(resp_data_wire.is_ok());
    auto resp_data_frame = netpipe::http2::parse_frame(resp_data_wire.value().data(), resp_data_wire.value().size());
    REQUIRE(resp_data_frame.is_ok());
    REQUIRE(client.process_inbound_frame(resp_data_frame.value().first).is_ok());

    REQUIRE(client.has_event());
    auto cev1 = client.pop_event();
    REQUIRE(cev1.is_ok());
    CHECK(cev1.value().type == netpipe::http2::InboundEventType::Headers);

    REQUIRE(client.has_event());
    auto cev2 = client.pop_event();
    REQUIRE(cev2.is_ok());
    CHECK(cev2.value().type == netpipe::http2::InboundEventType::Data);
    CHECK(cev2.value().data.size() == 2);
}

TEST_CASE("HTTP/2 connection startup gating and shutdown orchestration") {
    netpipe::http2::Connection client(true);
    netpipe::http2::Connection server(false);

    CHECK(client.open_request_stream().is_err());

    complete_settings_startup(client, server);

    auto sid = client.open_request_stream();
    REQUIRE(sid.is_ok());

    REQUIRE(client.send_rst_stream(sid.value(), netpipe::http2::ErrorCode::Cancel).is_ok());
    auto rst_wire = client.pop_outbound();
    REQUIRE(rst_wire.is_ok());
    REQUIRE(server.process_inbound_bytes(rst_wire.value()).is_ok());
    REQUIRE(server.has_event());
    auto rst_event = server.pop_event();
    REQUIRE(rst_event.is_ok());
    CHECK(rst_event.value().type == netpipe::http2::InboundEventType::RstStream);

    REQUIRE(server.initiate_shutdown(netpipe::http2::ErrorCode::NoError).is_ok());
    auto goaway_wire = server.pop_outbound();
    REQUIRE(goaway_wire.is_ok());
    REQUIRE(client.process_inbound_bytes(goaway_wire.value()).is_ok());
    REQUIRE(client.has_event());
    auto goaway_event = client.pop_event();
    REQUIRE(goaway_event.is_ok());
    CHECK(goaway_event.value().type == netpipe::http2::InboundEventType::GoAway);
    CHECK(client.draining());

    CHECK(client.open_request_stream().is_err());
}

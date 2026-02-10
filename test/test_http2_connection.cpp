#include <doctest/doctest.h>

#include <cstring>

#include <netpipe/protocol/http2/connection.hpp>

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

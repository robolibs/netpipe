#include <doctest/doctest.h>

#include <cstring>

#include <netpipe/protocol/http2/settings.hpp>

TEST_CASE("HTTP/2 client preface includes SETTINGS frame") {
    netpipe::http2::SettingsStateMachine client(true);
    netpipe::http2::Settings local;
    local.set(netpipe::http2::SettingsId::MaxConcurrentStreams, 100);

    auto preface_and_settings = client.start_client_preface(local);
    REQUIRE(preface_and_settings.is_ok());

    const auto &bytes = preface_and_settings.value();
    REQUIRE(bytes.size() > std::strlen(netpipe::http2::PREFACE));
    CHECK(std::memcmp(bytes.data(), netpipe::http2::PREFACE, std::strlen(netpipe::http2::PREFACE)) == 0);

    auto frame = netpipe::http2::parse_frame(bytes.data() + std::strlen(netpipe::http2::PREFACE),
                                             bytes.size() - std::strlen(netpipe::http2::PREFACE));
    REQUIRE(frame.is_ok());
    CHECK(frame.value().first.header.type == netpipe::http2::FrameType::Settings);
    CHECK(frame.value().first.header.stream_id == 0);
}

TEST_CASE("HTTP/2 server validates preface then sends SETTINGS") {
    netpipe::http2::SettingsStateMachine server(false);

    netpipe::Message preface(netpipe::http2::PREFACE, netpipe::http2::PREFACE + std::strlen(netpipe::http2::PREFACE));
    CHECK(server.process_client_preface(preface).is_ok());
    CHECK(server.state() == netpipe::http2::ConnectionState::PrefaceReceived);

    netpipe::http2::Settings local;
    local.set(netpipe::http2::SettingsId::InitialWindowSize, 65535);
    auto settings_frame = server.create_server_settings(local);
    REQUIRE(settings_frame.is_ok());

    auto parsed = netpipe::http2::parse_frame(settings_frame.value().data(), settings_frame.value().size());
    REQUIRE(parsed.is_ok());
    CHECK(parsed.value().first.header.type == netpipe::http2::FrameType::Settings);
}

TEST_CASE("HTTP/2 SETTINGS exchange reaches SettingsExchanged") {
    netpipe::http2::SettingsStateMachine client(true);
    netpipe::http2::SettingsStateMachine server(false);

    netpipe::http2::Settings client_local;
    client_local.set(netpipe::http2::SettingsId::HeaderTableSize, 4096);
    auto client_start = client.start_client_preface(client_local);
    REQUIRE(client_start.is_ok());

    netpipe::Message preface(client_start.value().begin(),
                             client_start.value().begin() + std::strlen(netpipe::http2::PREFACE));
    CHECK(server.process_client_preface(preface).is_ok());

    auto client_settings =
        netpipe::http2::parse_frame(client_start.value().data() + std::strlen(netpipe::http2::PREFACE),
                                    client_start.value().size() - std::strlen(netpipe::http2::PREFACE));
    REQUIRE(client_settings.is_ok());

    auto server_ack_opt = server.process_incoming_frame(client_settings.value().first);
    REQUIRE(server_ack_opt.is_ok());
    REQUIRE(server_ack_opt.value().has_value());

    netpipe::http2::Settings server_local;
    server_local.set(netpipe::http2::SettingsId::MaxFrameSize, 16384);
    auto server_settings_bytes = server.create_server_settings(server_local);
    REQUIRE(server_settings_bytes.is_ok());

    auto server_settings =
        netpipe::http2::parse_frame(server_settings_bytes.value().data(), server_settings_bytes.value().size());
    REQUIRE(server_settings.is_ok());

    auto client_ack_opt = client.process_incoming_frame(server_settings.value().first);
    REQUIRE(client_ack_opt.is_ok());
    REQUIRE(client_ack_opt.value().has_value());

    auto client_ack_frame =
        netpipe::http2::parse_frame(client_ack_opt.value().value().data(), client_ack_opt.value().value().size());
    REQUIRE(client_ack_frame.is_ok());
    CHECK(server.process_incoming_frame(client_ack_frame.value().first).is_ok());

    auto server_ack_frame =
        netpipe::http2::parse_frame(server_ack_opt.value().value().data(), server_ack_opt.value().value().size());
    REQUIRE(server_ack_frame.is_ok());
    CHECK(client.process_incoming_frame(server_ack_frame.value().first).is_ok());

    CHECK(client.state() == netpipe::http2::ConnectionState::SettingsExchanged);
    CHECK(server.state() == netpipe::http2::ConnectionState::SettingsExchanged);
}

TEST_CASE("HTTP/2 SETTINGS ACK with payload is rejected") {
    netpipe::http2::SettingsStateMachine client(true);
    netpipe::http2::Settings local;
    auto started = client.start_client_preface(local);
    REQUIRE(started.is_ok());

    netpipe::http2::Frame bad_ack;
    bad_ack.header.type = netpipe::http2::FrameType::Settings;
    bad_ack.header.flags = 0x1;
    bad_ack.header.stream_id = 0;
    bad_ack.payload = dp::Vector<dp::u8>{0x00};

    auto result = client.process_incoming_frame(bad_ack);
    CHECK(result.is_err());
}

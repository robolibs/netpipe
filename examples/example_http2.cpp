/// HTTP/2 protocol module example (in-memory)

#include <cstring>

#include <echo/echo.hpp>
#include <netpipe/protocol/http_selector.hpp>

int main() {
    echo::info("=== HTTP/2 Example ===");

    netpipe::http::ProtocolSelector selector;
    netpipe::http::NegotiatedCapabilities caps;
    caps.tls_active = true;
    caps.alpn_protocol = dp::String("h2");
    selector.set_capabilities(caps);

    auto h2_result = selector.create_http2_stream_manager();
    if (h2_result.is_err()) {
        echo::error("Selector failed: ", h2_result.error().message.c_str());
        return 1;
    }

    netpipe::http2::SettingsStateMachine client_settings(true);
    netpipe::http2::SettingsStateMachine server_settings(false);

    netpipe::http2::Settings client_local;
    client_local.set(netpipe::http2::SettingsId::HeaderTableSize, 4096);
    auto start = client_settings.start_client_preface(client_local);
    if (start.is_err()) {
        echo::error("Client preface failed: ", start.error().message.c_str());
        return 1;
    }

    netpipe::Message preface(start.value().begin(), start.value().begin() + std::strlen(netpipe::http2::PREFACE));
    auto preface_ok = server_settings.process_client_preface(preface);
    if (preface_ok.is_err()) {
        echo::error("Server preface parse failed: ", preface_ok.error().message.c_str());
        return 1;
    }

    auto client_settings_frame =
        netpipe::http2::parse_frame(start.value().data() + std::strlen(netpipe::http2::PREFACE),
                                    start.value().size() - std::strlen(netpipe::http2::PREFACE));
    if (client_settings_frame.is_err()) {
        echo::error("Client settings frame parse failed: ", client_settings_frame.error().message.c_str());
        return 1;
    }

    auto server_ack = server_settings.process_incoming_frame(client_settings_frame.value().first);
    if (server_ack.is_err()) {
        echo::error("Server processing settings failed: ", server_ack.error().message.c_str());
        return 1;
    }

    netpipe::http2::Settings server_local;
    server_local.set(netpipe::http2::SettingsId::MaxFrameSize, 16384);
    auto server_settings_bytes = server_settings.create_server_settings(server_local);
    if (server_settings_bytes.is_err()) {
        echo::error("Server settings encode failed: ", server_settings_bytes.error().message.c_str());
        return 1;
    }

    auto parsed_server_settings =
        netpipe::http2::parse_frame(server_settings_bytes.value().data(), server_settings_bytes.value().size());
    if (parsed_server_settings.is_err()) {
        echo::error("Server settings parse failed: ", parsed_server_settings.error().message.c_str());
        return 1;
    }

    auto client_ack = client_settings.process_incoming_frame(parsed_server_settings.value().first);
    if (client_ack.is_err()) {
        echo::error("Client processing settings failed: ", client_ack.error().message.c_str());
        return 1;
    }

    netpipe::http2::HpackContext encoder;
    netpipe::http2::StreamManager stream_mgr;
    netpipe::http::HeaderList headers = {
        {":method", "GET"},
        {":scheme", "https"},
        {":path", "/status"},
        {":authority", "example.com"},
    };
    auto header_block = encoder.encode(headers);
    if (header_block.is_err()) {
        echo::error("HPACK encode failed: ", header_block.error().message.c_str());
        return 1;
    }

    netpipe::http2::Frame frame;
    frame.header.type = netpipe::http2::FrameType::Headers;
    frame.header.stream_id = 1;
    frame.header.flags = 0x4; // END_HEADERS
    frame.payload = header_block.value();

    auto decoded = stream_mgr.process_incoming_frame(frame);
    if (decoded.is_err() || !decoded.value().has_value()) {
        echo::error("Stream manager decode failed");
        return 1;
    }

    echo::info("Decoded HTTP/2 headers on stream ", decoded.value().value().stream_id);
    echo::info("Header count: ", decoded.value().value().headers.size());
    echo::info("=== HTTP/2 Example Complete ===");
    return 0;
}

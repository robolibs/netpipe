/// HTTP/2 protocol module example using Connection session API (in-memory)

#include <echo/echo.hpp>
#include <netpipe/protocol/http_selector.hpp>

int main() {
    echo::info("=== HTTP/2 Example ===");

    netpipe::http::ProtocolSelector selector;
    netpipe::http::NegotiatedCapabilities caps;
    caps.tls_active = true;
    caps.alpn_protocol = dp::String("h2");
    selector.set_capabilities(caps);

    auto client_conn_result = selector.create_http2_connection(true);
    auto server_conn_result = selector.create_http2_connection(false);
    if (client_conn_result.is_err() || server_conn_result.is_err()) {
        echo::error("Selector failed to create HTTP/2 connections");
        return 1;
    }

    auto client = client_conn_result.value();
    auto server = server_conn_result.value();

    if (client.start().is_err()) {
        echo::error("client.start failed");
        return 1;
    }

    auto client_start = client.pop_outbound();
    if (client_start.is_err() || server.process_inbound_bytes(client_start.value()).is_err()) {
        echo::error("startup exchange failed (client->server)");
        return 1;
    }

    if (server.send_server_settings().is_err()) {
        echo::error("server.send_server_settings failed");
        return 1;
    }

    auto s1 = server.pop_outbound();
    auto s2 = server.pop_outbound();
    if (s1.is_err() || s2.is_err() || client.process_inbound_bytes(s1.value()).is_err() ||
        client.process_inbound_bytes(s2.value()).is_err()) {
        echo::error("startup exchange failed (server->client)");
        return 1;
    }

    auto cack = client.pop_outbound();
    if (cack.is_err() || server.process_inbound_bytes(cack.value()).is_err()) {
        echo::error("startup exchange failed (client ack)");
        return 1;
    }

    if (!client.startup_complete() || !server.startup_complete()) {
        echo::error("SETTINGS exchange not complete");
        return 1;
    }

    auto stream_id_result = client.open_request_stream();
    if (stream_id_result.is_err()) {
        echo::error("open_request_stream failed");
        return 1;
    }
    dp::u32 stream_id = stream_id_result.value();

    netpipe::http2::Request request;
    request.method = netpipe::http::Method::Get;
    request.scheme = "https";
    request.authority = "example.com";
    request.path = "/status";
    if (client.send_request_headers(stream_id, request, false).is_err() ||
        client.send_data(stream_id, {'o', 'k'}, true).is_err()) {
        echo::error("failed to send request over HTTP/2 connection");
        return 1;
    }

    auto req_h = client.pop_outbound();
    auto req_d = client.pop_outbound();
    if (req_h.is_err() || req_d.is_err()) {
        echo::error("failed to read queued request bytes");
        return 1;
    }

    auto req_hf = netpipe::http2::parse_frame(req_h.value().data(), req_h.value().size());
    auto req_df = netpipe::http2::parse_frame(req_d.value().data(), req_d.value().size());
    if (req_hf.is_err() || req_df.is_err() || server.process_inbound_frame(req_hf.value().first).is_err() ||
        server.process_inbound_frame(req_df.value().first).is_err()) {
        echo::error("server failed to process request frames");
        return 1;
    }

    if (server.has_event()) {
        auto ev = server.pop_event();
        if (ev.is_ok() && ev.value().type == netpipe::http2::InboundEventType::Headers) {
            echo::info("Server decoded headers on stream ", ev.value().stream_id);
        }
    }

    echo::info("=== HTTP/2 Example Complete ===");
    return 0;
}

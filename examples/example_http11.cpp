/// HTTP/1.1 protocol module example (in-memory)

#include <echo/echo.hpp>
#include <netpipe/protocol/http_selector.hpp>

int main() {
    echo::info("=== HTTP/1.1 Example ===");

    netpipe::http::SelectorConfig config;
    netpipe::http::NegotiatedCapabilities caps;
    caps.alpn_protocol = dp::String("http/1.1");

    netpipe::http::ProtocolSelector selector(config);
    selector.set_capabilities(caps);

    auto client_conn_result = selector.create_http11_client();
    if (client_conn_result.is_err()) {
        echo::error("Selector failed: ", client_conn_result.error().message.c_str());
        return 1;
    }

    auto client = client_conn_result.value();
    netpipe::http11::ServerConnection server;

    netpipe::http11::Request request;
    request.method = netpipe::http::Method::Post;
    request.target = "/api/echo";
    netpipe::http11::set_header(request.headers, "Host", "localhost");
    netpipe::http11::set_header(request.headers, "Content-Type", "text/plain");
    request.body = dp::Vector<dp::u8>{'h', 'e', 'l', 'l', 'o'};

    auto wire_req = client.encode_request(request);
    if (wire_req.is_err()) {
        echo::error("encode_request failed: ", wire_req.error().message.c_str());
        return 1;
    }

    auto decoded_req = server.decode_request(wire_req.value());
    if (decoded_req.is_err()) {
        echo::error("decode_request failed: ", decoded_req.error().message.c_str());
        return 1;
    }

    server.set_last_request_method(decoded_req.value().method);
    echo::info("Server received target: ", decoded_req.value().target.c_str());

    netpipe::http11::Response response;
    response.status_code = 200;
    response.reason = "OK";
    response.body = decoded_req.value().body;

    auto wire_resp = server.encode_response(response);
    if (wire_resp.is_err()) {
        echo::error("encode_response failed: ", wire_resp.error().message.c_str());
        return 1;
    }

    auto decoded_resp = client.decode_response(wire_resp.value());
    if (decoded_resp.is_err()) {
        echo::error("decode_response failed: ", decoded_resp.error().message.c_str());
        return 1;
    }

    echo::info("Client received status: ", decoded_resp.value().status_code);
    echo::info("Client received body bytes: ", decoded_resp.value().body.size());
    echo::info("=== HTTP/1.1 Example Complete ===");
    return 0;
}

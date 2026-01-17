/// HTTP/3 Example
///
/// This example demonstrates how to use HTTP/3 over QUIC for making
/// HTTP requests with header compression and multiplexed streams.
///
/// Usage:
///   ./example_http3 server   # Start HTTP/3 server
///   ./example_http3 client   # Connect and send requests

#include <chrono>
#include <echo/echo.hpp>
#include <netpipe/http3.hpp>
#include <netpipe/quic.hpp>
#include <thread>

using namespace netpipe;
using namespace netpipe::quic;
using namespace netpipe::http3;

// Simulated HTTP/3 server handler
void handle_request(http3::Connection &conn, dp::u64 stream_id) {
    auto request_opt = conn.get_request(stream_id);
    if (!request_opt.has_value()) {
        echo::error("No request found for stream ", stream_id);
        return;
    }

    auto &req = request_opt.value();
    echo::info("Received request: ", req.method.c_str(), " ", req.path.c_str());

    // Create response
    http3::Response resp;

    if (req.path == "/" || req.path == "/index.html") {
        resp.status = 200;
        resp.headers.push_back(HeaderField("content-type", "text/html"));
        const char *body = "<html><body><h1>Hello from HTTP/3!</h1></body></html>";
        resp.body = dp::Vector<dp::u8>(body, body + strlen(body));
    } else if (req.path == "/api/status") {
        resp.status = 200;
        resp.headers.push_back(HeaderField("content-type", "application/json"));
        const char *body = R"({"status":"ok","protocol":"HTTP/3"})";
        resp.body = dp::Vector<dp::u8>(body, body + strlen(body));
    } else {
        resp.status = 404;
        resp.headers.push_back(HeaderField("content-type", "text/plain"));
        const char *body = "Not Found";
        resp.body = dp::Vector<dp::u8>(body, body + strlen(body));
    }

    // Encode response (in real implementation, send over QUIC stream)
    auto headers_data = conn.encode_response(stream_id, resp);
    if (headers_data.is_ok()) {
        echo::info("Response encoded: ", headers_data.value().size(), " bytes headers");
    }

    if (!resp.body.empty()) {
        auto body_data = conn.encode_data(stream_id, resp.body);
        if (body_data.is_ok()) {
            echo::info("Response body: ", body_data.value().size(), " bytes");
        }
    }

    conn.close_stream(stream_id);
}

void server_demo() {
    echo::info("=== HTTP/3 Server Demo ===");

    // Create HTTP/3 connection (server side)
    http3::Connection server_conn(false);

    // Configure settings
    http3::Settings settings;
    settings.max_field_section_size = 16384;
    server_conn.set_local_settings(settings);

    // Initialize and get SETTINGS frame to send on control stream
    auto init_result = server_conn.initialize();
    if (init_result.is_err()) {
        echo::error("Failed to initialize: ", init_result.error().message.c_str());
        return;
    }

    echo::info("Server initialized, SETTINGS frame ready (", init_result.value().size(), " bytes)");
    echo::info("Waiting for client...");

    // Simulate receiving client SETTINGS
    http3::Connection client_sim(true);
    auto client_init = client_sim.initialize().value();
    auto server_init = init_result.value();

    // Exchange settings both ways
    server_conn.process_control_data(client_init);
    client_sim.process_control_data(server_init);

    echo::info("Client connected, settings exchanged");

    // Simulate receiving a request
    auto stream_result = client_sim.create_request_stream();
    if (stream_result.is_err()) {
        echo::error("Failed to create stream: ", stream_result.error().message.c_str());
        return;
    }
    auto stream_id = stream_result.value();

    http3::Request req;
    req.method = "GET";
    req.scheme = "https";
    req.authority = "localhost:4433";
    req.path = "/api/status";
    req.headers.push_back(HeaderField("user-agent", "netpipe-http3/1.0"));
    req.headers.push_back(HeaderField("accept", "application/json"));

    auto req_data = client_sim.encode_request(stream_id, req).value();
    echo::info("Client sent request (", req_data.size(), " bytes)");

    // Server processes request
    server_conn.process_request_stream(stream_id, req_data);
    server_conn.stream_finished(stream_id);

    // Handle the request
    handle_request(server_conn, stream_id);

    echo::info("Server demo complete");
}

void client_demo() {
    echo::info("=== HTTP/3 Client Demo ===");

    // Create HTTP/3 connection (client side)
    http3::Connection client_conn(true);

    // Initialize
    auto init_result = client_conn.initialize();
    if (init_result.is_err()) {
        echo::error("Failed to initialize: ", init_result.error().message.c_str());
        return;
    }

    echo::info("Client initialized, SETTINGS frame ready");

    // Simulate server connection
    http3::Connection server_sim(false);
    auto client_init = init_result.value();
    auto server_init = server_sim.initialize().value();

    // Exchange settings both ways
    client_conn.process_control_data(server_init);
    server_sim.process_control_data(client_init);

    echo::info("Connected to server, settings exchanged");

    // Make multiple requests (demonstrating multiplexing)
    dp::Vector<dp::u64> stream_ids;

    // Request 1: GET /
    {
        auto stream_id = client_conn.create_request_stream().value();
        stream_ids.push_back(stream_id);

        http3::Request req;
        req.method = "GET";
        req.scheme = "https";
        req.authority = "example.com";
        req.path = "/";
        req.headers.push_back(HeaderField("user-agent", "netpipe-http3/1.0"));

        auto req_data = client_conn.encode_request(stream_id, req).value();
        echo::info("Request 1 (stream ", stream_id, "): GET / - ", req_data.size(), " bytes");
    }

    // Request 2: GET /api/status
    {
        auto stream_id = client_conn.create_request_stream().value();
        stream_ids.push_back(stream_id);

        http3::Request req;
        req.method = "GET";
        req.scheme = "https";
        req.authority = "example.com";
        req.path = "/api/status";
        req.headers.push_back(HeaderField("accept", "application/json"));

        auto req_data = client_conn.encode_request(stream_id, req).value();
        echo::info("Request 2 (stream ", stream_id, "): GET /api/status - ", req_data.size(), " bytes");
    }

    // Request 3: POST /api/data
    {
        auto stream_id = client_conn.create_request_stream().value();
        stream_ids.push_back(stream_id);

        http3::Request req;
        req.method = "POST";
        req.scheme = "https";
        req.authority = "example.com";
        req.path = "/api/data";
        req.headers.push_back(HeaderField("content-type", "application/json"));

        auto req_data = client_conn.encode_request(stream_id, req).value();

        const char *body = R"({"key":"value"})";
        dp::Vector<dp::u8> body_bytes(body, body + strlen(body));
        auto body_data = client_conn.encode_data(stream_id, body_bytes).value();

        echo::info("Request 3 (stream ", stream_id, "): POST /api/data - ", req_data.size(), " + ",
                   body_data.size(), " bytes");
    }

    echo::info("Sent ", stream_ids.size(), " concurrent requests");

    // Simulate receiving responses by encoding directly (bypass server_sim stream tracking)
    QpackEncoder resp_encoder;
    for (auto stream_id : stream_ids) {
        // Encode response headers directly
        http3::Response resp;
        resp.status = 200;
        resp.headers.push_back(HeaderField("content-type", "application/json"));

        auto all_headers = resp.get_all_headers();
        auto encoded_headers = resp_encoder.encode(all_headers);

        HeadersFrame headers_frame;
        headers_frame.encoded_field_section = encoded_headers;
        auto resp_data = headers_frame.serialize();

        // Client processes response
        client_conn.process_request_stream(stream_id, resp_data);

        auto response_opt = client_conn.get_response(stream_id);
        if (response_opt.has_value()) {
            echo::info("Response for stream ", stream_id, ": status=", response_opt.value().status);
        }
    }

    echo::info("Client demo complete");
}

void qpack_demo() {
    echo::info("=== QPACK Compression Demo ===");

    QpackEncoder encoder;
    QpackDecoder decoder;

    // Show compression efficiency
    http3::HeaderList headers;
    headers.push_back(HeaderField(":method", "GET"));
    headers.push_back(HeaderField(":scheme", "https"));
    headers.push_back(HeaderField(":authority", "example.com"));
    headers.push_back(HeaderField(":path", "/api/v1/users"));
    headers.push_back(HeaderField("accept", "*/*"));
    headers.push_back(HeaderField("accept-encoding", "gzip, deflate, br"));
    headers.push_back(HeaderField("user-agent", "netpipe-http3/1.0"));
    headers.push_back(HeaderField("x-request-id", "abc123-def456"));

    // Calculate uncompressed size
    dp::usize uncompressed = 0;
    for (const auto &h : headers) {
        uncompressed += h.name.size() + h.value.size() + 4; // name: value\r\n
    }

    auto encoded = encoder.encode(headers);

    echo::info("Headers:");
    for (const auto &h : headers) {
        echo::info("  ", h.name.c_str(), ": ", h.value.c_str());
    }

    echo::info("Uncompressed size: ", uncompressed, " bytes");
    echo::info("QPACK compressed:  ", encoded.size(), " bytes");
    echo::info("Compression ratio: ", (100 * encoded.size() / uncompressed), "%");

    // Decode and verify
    auto decoded_result = decoder.decode(encoded);
    if (decoded_result.is_ok()) {
        auto &decoded = decoded_result.value();
        echo::info("Decoded ", decoded.size(), " headers successfully");
    }
}

void frame_demo() {
    echo::info("=== HTTP/3 Frame Demo ===");

    // DATA frame
    {
        DataFrame frame;
        const char *data = "Hello, HTTP/3!";
        frame.data = dp::Vector<dp::u8>(data, data + strlen(data));

        auto serialized = frame.serialize();
        echo::info("DATA frame: ", strlen(data), " bytes payload -> ", serialized.size(),
                   " bytes on wire");
    }

    // HEADERS frame
    {
        QpackEncoder encoder;
        http3::HeaderList headers;
        headers.push_back(HeaderField(":status", "200"));
        headers.push_back(HeaderField("content-type", "text/html"));

        HeadersFrame frame;
        frame.encoded_field_section = encoder.encode(headers);

        auto serialized = frame.serialize();
        echo::info("HEADERS frame: ", frame.encoded_field_section.size(), " bytes encoded -> ",
                   serialized.size(), " bytes on wire");
    }

    // SETTINGS frame
    {
        SettingsFrame frame;
        frame.settings.qpack_max_table_capacity = 4096;
        frame.settings.max_field_section_size = 16384;
        frame.settings.qpack_blocked_streams = 100;

        auto serialized = frame.serialize();
        echo::info("SETTINGS frame: ", serialized.size(), " bytes");
    }

    // GOAWAY frame
    {
        GoAwayFrame frame;
        frame.stream_id = 100;

        auto serialized = frame.serialize();
        echo::info("GOAWAY frame: ", serialized.size(), " bytes");
    }
}

int main(int argc, char **argv) {
    echo::set_level(echo::Level::Info);

    if (argc < 2) {
        echo::info("HTTP/3 over QUIC Example");
        echo::info("Usage: ", argv[0], " [server|client|qpack|frames|all]");
        echo::info("");
        echo::info("Demos:");
        echo::info("  server - HTTP/3 server handling requests");
        echo::info("  client - HTTP/3 client making requests");
        echo::info("  qpack  - QPACK header compression");
        echo::info("  frames - HTTP/3 frame encoding");
        echo::info("  all    - Run all demos");
        return 1;
    }

    dp::String mode = argv[1];

    if (mode == "server") {
        server_demo();
    } else if (mode == "client") {
        client_demo();
    } else if (mode == "qpack") {
        qpack_demo();
    } else if (mode == "frames") {
        frame_demo();
    } else if (mode == "all") {
        qpack_demo();
        echo::info("");
        frame_demo();
        echo::info("");
        server_demo();
        echo::info("");
        client_demo();
    } else {
        echo::error("Unknown mode: ", mode.c_str());
        return 1;
    }

    return 0;
}

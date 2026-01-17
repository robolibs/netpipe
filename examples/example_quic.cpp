/// Example: QUIC Transport
/// Demonstrates encrypted client-server communication using QUIC (RFC 9000)
///
/// This example:
/// 1. Server listens on UDP port 4433
/// 2. Client connects and performs QUIC handshake
/// 3. Encrypted messages are exchanged over QUIC streams
/// 4. Shows stream multiplexing capability

#include <chrono>
#include <netpipe/quic.hpp>
#include <thread>

void run_server() {
    echo::info("[Server] Starting QUIC server...");

    netpipe::quic::QuicConfig config;
    // In production, you would set:
    // config.certificate = load_certificate();
    // config.private_key = load_private_key();
    config.transport_params.initial_max_streams_bidi = 100;
    config.transport_params.initial_max_data = 1024 * 1024;

    netpipe::quic::QuicStream server(config);

    auto listen_result = server.listen({"0.0.0.0", 4433});
    if (listen_result.is_err()) {
        echo::error("[Server] Failed to listen: ", listen_result.error().message.c_str());
        return;
    }
    echo::info("[Server] Listening on port 4433");

    // Accept connection
    auto accept_result = server.accept();
    if (accept_result.is_err()) {
        echo::error("[Server] Failed to accept: ", accept_result.error().message.c_str());
        return;
    }
    auto client = std::move(accept_result.value());
    echo::info("[Server] Client connected");

    // Receive message
    auto recv_result = client->recv();
    if (recv_result.is_err()) {
        echo::error("[Server] Failed to receive: ", recv_result.error().message.c_str());
        return;
    }

    auto message = recv_result.value();
    echo::info("[Server] Received: ", std::string(message.begin(), message.end()).c_str());

    // Send response
    netpipe::Message response;
    std::string response_str = "Hello from QUIC server!";
    response.insert(response.end(), response_str.begin(), response_str.end());

    auto send_result = client->send(response);
    if (send_result.is_err()) {
        echo::error("[Server] Failed to send: ", send_result.error().message.c_str());
        return;
    }
    echo::info("[Server] Response sent");

    // Keep alive for a bit
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    server.close();
    echo::info("[Server] Closed");
}

void run_client() {
    // Wait for server to start
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    echo::info("[Client] Starting QUIC client...");

    netpipe::quic::QuicConfig config;
    config.server_name = "localhost";
    config.skip_cert_verification = true; // For testing only!
    config.transport_params.initial_max_streams_bidi = 100;

    netpipe::quic::QuicStream client(config);

    auto connect_result = client.connect({"127.0.0.1", 4433});
    if (connect_result.is_err()) {
        echo::error("[Client] Failed to connect: ", connect_result.error().message.c_str());
        return;
    }
    echo::info("[Client] Connected to server");

    // Send message
    netpipe::Message message;
    std::string message_str = "Hello from QUIC client!";
    message.insert(message.end(), message_str.begin(), message_str.end());

    auto send_result = client.send(message);
    if (send_result.is_err()) {
        echo::error("[Client] Failed to send: ", send_result.error().message.c_str());
        return;
    }
    echo::info("[Client] Message sent");

    // Receive response
    auto recv_result = client.recv();
    if (recv_result.is_err()) {
        echo::error("[Client] Failed to receive: ", recv_result.error().message.c_str());
        return;
    }

    auto response = recv_result.value();
    echo::info("[Client] Received: ", std::string(response.begin(), response.end()).c_str());

    client.close();
    echo::info("[Client] Closed");
}

void demo_stream_multiplexing() {
    echo::info("");
    echo::info("=== Stream Multiplexing Demo ===");
    echo::info("");

    // This demonstrates QUIC's unique capability: multiple streams on one connection

    netpipe::quic::QuicConfig config;
    config.transport_params.initial_max_streams_bidi = 100;

    netpipe::quic::QuicStream quic(config);

    // In a real scenario, you would connect first
    // For this demo, we just show the API

    echo::info("QUIC allows multiple streams over a single connection:");
    echo::info("");
    echo::info("  // Open additional streams");
    echo::info("  auto stream2 = quic.open_stream();");
    echo::info("  auto stream3 = quic.open_stream();");
    echo::info("");
    echo::info("  // Each stream is independent");
    echo::info("  stream2->send(data_for_stream2);");
    echo::info("  stream3->send(data_for_stream3);");
    echo::info("");
    echo::info("  // Head-of-line blocking is avoided!");
    echo::info("  // If stream2 has packet loss, stream3 continues unaffected");
    echo::info("");
}

int main() {
    echo::info("=== QUIC Transport Example ===");
    echo::info("QUIC provides encrypted, multiplexed streams over UDP");
    echo::info("");

    // Start server and client
    std::thread server_thread(run_server);
    std::thread client_thread(run_client);

    server_thread.join();
    client_thread.join();

    // Show multiplexing demo
    demo_stream_multiplexing();

    echo::info("=== QUIC Example Complete ===");
    return 0;
}

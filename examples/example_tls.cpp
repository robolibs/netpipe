/// Example: TLS 1.3 over TCP
/// Demonstrates encrypted client-server communication using TLS 1.3
///
/// This example:
/// 1. Generates a self-signed Ed25519 certificate
/// 2. Server listens with TLS
/// 3. Client connects and performs TLS handshake
/// 4. Encrypted messages are exchanged

#include <chrono>
#include <netpipe/stream/tcp.hpp>
#include <netpipe/tls.hpp>
#include <thread>

#include <keylock/cert/builder.hpp>
#include <keylock/crypto/context.hpp>

// Generate a self-signed certificate for testing
std::pair<dp::Vector<dp::u8>, dp::Vector<dp::u8>> generate_test_certificate() {
    echo::info("Generating self-signed certificate...");

    // Generate Ed25519 keypair
    keylock::crypto::Context crypto(keylock::crypto::Context::Algorithm::Ed25519);
    auto keypair = crypto.generate_keypair();

    // Build certificate
    keylock::cert::CertificateBuilder builder;

    auto now = std::chrono::system_clock::now();
    auto one_year = std::chrono::hours(24 * 365);

    builder.set_serial(1)
        .set_subject_from_string("CN=localhost,O=NetPipe Test,C=US")
        .set_issuer_from_string("CN=localhost,O=NetPipe Test,C=US")
        .set_validity(now, now + one_year)
        .set_subject_public_key_ed25519(keypair.public_key)
        .set_basic_constraints(false, std::nullopt)
        .set_key_usage(0x80); // digitalSignature

    auto cert_result = builder.build_ed25519(keypair, true);

    if (!cert_result.success) {
        echo::error("Failed to build certificate: ", cert_result.error.c_str());
        return {{}, {}};
    }

    auto cert_der = cert_result.value.to_der();
    dp::Vector<dp::u8> cert(cert_der.begin(), cert_der.end());
    dp::Vector<dp::u8> key(keypair.private_key.begin(), keypair.private_key.end());

    echo::info("Certificate generated (", cert.size(), " bytes)");

    return {cert, key};
}

void run_server(const dp::Vector<dp::u8> &cert, const dp::Vector<dp::u8> &key) {
    echo::info("[Server] Starting...");

    netpipe::TcpStream tcp;
    netpipe::TcpEndpoint endpoint{"127.0.0.1", 8443};

    auto listen_result = tcp.listen(endpoint);
    if (listen_result.is_err()) {
        echo::error("[Server] Failed to listen: ", listen_result.error().message.c_str());
        return;
    }
    echo::info("[Server] Listening on ", endpoint.to_string());

    // Accept connection
    auto accept_result = tcp.accept();
    if (accept_result.is_err()) {
        echo::error("[Server] Failed to accept: ", accept_result.error().message.c_str());
        return;
    }
    auto client_stream = std::move(accept_result.value());
    echo::info("[Server] Client connected");

    // Configure TLS session
    netpipe::tls::SessionConfig config;
    config.certificate = cert;
    config.private_key = key;
    config.skip_cert_verification = true; // For testing

    netpipe::tls::Session session(config);

    // Perform TLS handshake
    echo::info("[Server] Starting TLS handshake...");
    auto handshake_result = session.handshake_server(*client_stream);
    if (handshake_result.is_err()) {
        echo::error("[Server] TLS handshake failed: ", handshake_result.error().message.c_str());
        return;
    }
    echo::info("[Server] TLS handshake complete!");

    // Receive encrypted message
    auto recv_result = session.recv(*client_stream);
    if (recv_result.is_err()) {
        echo::error("[Server] Failed to receive: ", recv_result.error().message.c_str());
        return;
    }

    auto message = recv_result.value();
    echo::info("[Server] Received encrypted message: ", message.size(), " bytes");
    echo::info("[Server] Content: ", std::string(message.begin(), message.end()).c_str());

    // Send encrypted response
    dp::Vector<dp::u8> response;
    std::string response_str = "Hello from TLS server!";
    response.insert(response.end(), response_str.begin(), response_str.end());

    auto send_result = session.send(*client_stream, response);
    if (send_result.is_err()) {
        echo::error("[Server] Failed to send: ", send_result.error().message.c_str());
        return;
    }
    echo::info("[Server] Sent encrypted response");

    // Close TLS session
    session.close(*client_stream);
    echo::info("[Server] Session closed");
}

void run_client() {
    // Wait for server to start
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    echo::info("[Client] Starting...");

    netpipe::TcpStream tcp;
    netpipe::TcpEndpoint endpoint{"127.0.0.1", 8443};

    auto connect_result = tcp.connect(endpoint);
    if (connect_result.is_err()) {
        echo::error("[Client] Failed to connect: ", connect_result.error().message.c_str());
        return;
    }
    echo::info("[Client] Connected to ", endpoint.to_string());

    // Configure TLS session
    netpipe::tls::SessionConfig config;
    config.server_name = "localhost";
    config.skip_cert_verification = true; // For testing

    netpipe::tls::Session session(config);

    // Perform TLS handshake
    echo::info("[Client] Starting TLS handshake...");
    auto handshake_result = session.handshake_client(tcp);
    if (handshake_result.is_err()) {
        echo::error("[Client] TLS handshake failed: ", handshake_result.error().message.c_str());
        return;
    }
    echo::info("[Client] TLS handshake complete!");

    // Send encrypted message
    dp::Vector<dp::u8> message;
    std::string message_str = "Hello from TLS client!";
    message.insert(message.end(), message_str.begin(), message_str.end());

    auto send_result = session.send(tcp, message);
    if (send_result.is_err()) {
        echo::error("[Client] Failed to send: ", send_result.error().message.c_str());
        return;
    }
    echo::info("[Client] Sent encrypted message");

    // Receive encrypted response
    auto recv_result = session.recv(tcp);
    if (recv_result.is_err()) {
        echo::error("[Client] Failed to receive: ", recv_result.error().message.c_str());
        return;
    }

    auto response = recv_result.value();
    echo::info("[Client] Received encrypted response: ", response.size(), " bytes");
    echo::info("[Client] Content: ", std::string(response.begin(), response.end()).c_str());

    // Close TLS session
    session.close(tcp);
    echo::info("[Client] Session closed");
}

int main() {
    echo::info("=== TLS 1.3 Example ===");
    echo::info("This example demonstrates encrypted communication using TLS 1.3");
    echo::info("");

    // Generate certificate
    auto [cert, key] = generate_test_certificate();
    if (cert.empty() || key.empty()) {
        echo::error("Failed to generate certificate");
        return 1;
    }

    // Start server and client threads
    std::thread server_thread([&]() { run_server(cert, key); });
    std::thread client_thread(run_client);

    server_thread.join();
    client_thread.join();

    echo::info("");
    echo::info("=== TLS Example Complete ===");

    return 0;
}

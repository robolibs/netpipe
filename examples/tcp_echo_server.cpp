#include <netpipe/stream/tcp.hpp>

int main() {
    echo::info("TCP Echo Server starting...");

    netpipe::TcpStream server;
    netpipe::TcpEndpoint endpoint{"0.0.0.0", 7447};

    // Start listening
    auto res = server.listen(endpoint);
    if (res.is_err()) {
        echo::error("Failed to listen: ", res.error().message.c_str());
        return 1;
    }

    echo::info("Server listening on port 7447");

    // Accept one connection
    auto client_res = server.accept();
    if (client_res.is_err()) {
        echo::error("Failed to accept: ", client_res.error().message.c_str());
        return 1;
    }

    auto client = std::move(client_res.value());
    echo::info("Client connected!");

    // Echo loop
    for (int i = 0; i < 5; i++) {
        auto msg_res = client->recv();
        if (msg_res.is_err()) {
            echo::error("Recv failed: ", msg_res.error().message.c_str());
            break;
        }

        auto msg = msg_res.value();
        echo::info("Received ", msg.size(), " bytes");

        // Echo back
        auto send_res = client->send(msg);
        if (send_res.is_err()) {
            echo::error("Send failed: ", send_res.error().message.c_str());
            break;
        }

        echo::info("Echoed ", msg.size(), " bytes back");
    }

    client->close();
    server.close();

    echo::info("Server shutting down");
    return 0;
}

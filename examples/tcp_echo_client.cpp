#include <netpipe/stream/tcp.hpp>

int main() {
    echo::info("TCP Echo Client starting...");

    netpipe::TcpStream client;
    netpipe::TcpEndpoint endpoint{"127.0.0.1", 7447};

    // Connect to server
    auto res = client.connect(endpoint);
    if (res.is_err()) {
        echo::error("Failed to connect: ", res.error().message.c_str());
        return 1;
    }

    echo::info("Connected to server!");

    // Send and receive messages
    for (int i = 0; i < 5; i++) {
        // Create a test message
        dp::String test_msg = dp::String("Hello from client #") + dp::String(std::to_string(i).c_str());
        netpipe::Message msg(test_msg.begin(), test_msg.end());

        echo::info("Sending: ", test_msg.c_str());

        // Send
        auto send_res = client.send(msg);
        if (send_res.is_err()) {
            echo::error("Send failed: ", send_res.error().message.c_str());
            break;
        }

        // Receive echo
        auto recv_res = client.recv();
        if (recv_res.is_err()) {
            echo::error("Recv failed: ", recv_res.error().message.c_str());
            break;
        }

        auto echo_msg = recv_res.value();
        dp::String echo_str(reinterpret_cast<const char *>(echo_msg.data()));
        echo::info("Received: ", echo_str.c_str());
    }

    client.close();
    echo::info("Client shutting down");
    return 0;
}

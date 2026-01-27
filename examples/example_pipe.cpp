#include <atomic>
#include <chrono>
#include <netpipe/netpipe.hpp>
#include <thread>

int main() {
    // Pipe demo: unified endpoint + stream with blocking and async receive.
    auto endpoint = netpipe::AnyEndpoint::ipc_endpoint("/tmp/netpipe_pipe_example.sock");

    // Server: listen returns already-listening pipe
    auto server_res = netpipe::Pipe::listen(endpoint);
    if (server_res.is_err()) {
        echo::error("Server listen failed: ", server_res.error().message);
        return 1;
    }
    auto server = std::move(server_res.value());

    std::atomic<bool> client_done{false};

    std::thread client_thread([&endpoint, &client_done]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // Client: connect returns already-connected pipe
        auto client_res = netpipe::Pipe::connect(endpoint);
        if (client_res.is_err()) {
            echo::error("Client connect failed: ", client_res.error().message);
            return;
        }
        auto client = std::move(client_res.value());

        // Send a message
        netpipe::Message msg = {0x01, 0x02, 0x03};
        if (client.send(msg).is_err()) {
            echo::error("Client send failed");
            return;
        }
        echo::info("Client sent: ", msg.size(), " bytes");

        // Blocking receive for response
        auto resp = client.recv();
        if (resp.is_ok()) {
            echo::info("Client received response: ", resp.value().size(), " bytes");
        }

        client.close();
        client_done = true;
    });

    // Accept incoming connection
    auto conn_res = server.accept();
    if (conn_res.is_err()) {
        echo::error("Server accept failed");
        client_thread.join();
        return 1;
    }
    auto conn = std::move(conn_res.value());

    // Async receive: callback invoked on background thread
    std::atomic<bool> received{false};
    conn.recv([&conn, &received](dp::Res<netpipe::Message> msg) {
        if (msg.is_ok()) {
            echo::info("Server received (async): ", msg.value().size(), " bytes");

            // Send response
            netpipe::Message resp = {0xAA, 0xBB, 0xCC};
            (void)conn.send(resp);
            echo::info("Server sent response");
        }
        received = true;
    });

    // Wait for async receive to complete
    while (!received) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    client_thread.join();
    conn.close();
    server.close();

    echo::info("Pipe example completed successfully");
    return 0;
}

/// Example: SHM + Remote<Unidirect> - TCP-like connection semantics
/// Simple client-server RPC over shared memory with accept/connect pattern
/// Test cases: 1MB, 10MB, 100MB

#include <chrono>
#include <memory>
#include <netpipe/netpipe.hpp>
#include <thread>

void test_payload(dp::usize size_mb) {
    echo::info("Testing ", size_mb, "MB payload...");

    netpipe::ShmStream listener;
    netpipe::ShmEndpoint endpoint{"netpipe_shm_unidirect", 256 * 1024 * 1024}; // 256MB buffer

    auto listen_res = listener.listen_shm(endpoint);
    if (listen_res.is_err()) {
        echo::error("Server listen failed: ", listen_res.error().message.c_str());
        return;
    }

    std::unique_ptr<netpipe::Stream> server_conn;

    // Server thread
    std::thread server_thread([&]() {
        // Accept a connection
        auto accept_res = listener.accept();
        if (accept_res.is_err()) {
            echo::error("Server accept failed: ", accept_res.error().message.c_str());
            return;
        }
        server_conn = std::move(accept_res.value());
        echo::info("Server accepted connection");

        netpipe::Remote<netpipe::Unidirect> remote(*server_conn);

        remote.register_method(1, [](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            echo::info("Server received: ", req.size() / 1024 / 1024, "MB");
            return dp::result::ok(req); // Echo back
        });

        // Serve one request
        auto recv_res = server_conn->recv();
        if (recv_res.is_ok()) {
            auto decoded = netpipe::remote::decode_remote_message_v2(recv_res.value()).value();
            netpipe::Message resp = decoded.payload;
            auto response = netpipe::remote::encode_remote_message_v2(decoded.request_id, decoded.method_id, resp,
                                                                      netpipe::remote::MessageType::Response);
            server_conn->send(response);
        }
    });

    // Client thread
    std::thread client_thread([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        netpipe::ShmStream stream;
        auto connect_res = stream.connect_shm(endpoint);
        if (connect_res.is_err()) {
            echo::error("Client connect failed: ", connect_res.error().message.c_str());
            return;
        }

        netpipe::Remote<netpipe::Unidirect> remote(stream);

        // Create payload
        dp::usize size = size_mb * 1024 * 1024;
        netpipe::Message request(size);
        for (dp::usize i = 0; i < size; i++) {
            request[i] = static_cast<dp::u8>(i % 256);
        }

        auto start = std::chrono::high_resolution_clock::now();
        auto response = remote.call(1, request, 60000);
        auto end = std::chrono::high_resolution_clock::now();

        if (response.is_ok()) {
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
            echo::info("Client received: ", response.value().size() / 1024 / 1024, "MB in ", duration, "ms");
        } else {
            echo::error("Client call failed: ", response.error().message.c_str());
        }

        stream.close();
    });

    server_thread.join();
    client_thread.join();

    listener.close();
}

int main() {
    echo::info("=== SHM + Unidirect RPC (TCP-like semantics) ===");

    test_payload(1);    // 1MB
    test_payload(10);   // 10MB
    test_payload(100);  // 100MB
    test_payload(1024); // 1GB

    return 0;
}

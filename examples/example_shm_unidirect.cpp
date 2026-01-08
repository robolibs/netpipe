/// Example: SHM + Remote<Unidirect>
/// Simple client-server RPC over shared memory
/// 4 test cases: 1MB, 10MB, 100MB, 1GB

#include <chrono>
#include <netpipe/netpipe.hpp>
#include <thread>

void test_payload(dp::usize size_mb) {
    echo::info("Testing ", size_mb, "MB payload...");

    // Server thread
    std::thread server_thread([size_mb]() {
        netpipe::ShmStream server;
        netpipe::ShmEndpoint endpoint{"netpipe_shm_unidirect", 1536 * 1024 * 1024}; // 1.5GB buffer
        server.listen_shm(endpoint);

        // Wait for client to connect and send data
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        netpipe::Remote<netpipe::Unidirect> remote(server);

        remote.register_method(1, [](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            echo::info("Server received: ", req.size() / 1024 / 1024, "MB");
            return dp::result::ok(req); // Echo back
        });

        // Serve one request
        auto recv_res = server.recv();
        if (recv_res.is_ok()) {
            auto decoded = netpipe::remote::decode_remote_message_v2(recv_res.value()).value();
            netpipe::Message resp = decoded.payload;
            auto response = netpipe::remote::encode_remote_message_v2(decoded.request_id, decoded.method_id, resp,
                                                                      netpipe::remote::MessageType::Response);
            server.send(response);
        }
    });

    // Client thread
    std::thread client_thread([size_mb]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        netpipe::ShmStream stream;
        netpipe::ShmEndpoint endpoint{"netpipe_shm_unidirect", 1536 * 1024 * 1024}; // 1.5GB buffer
        stream.connect_shm(endpoint);

        netpipe::Remote<netpipe::Unidirect> remote(stream);

        // Create payload
        dp::usize size = size_mb * 1024 * 1024;
        netpipe::Message request(size);
        for (dp::usize i = 0; i < size; i++) {
            request[i] = static_cast<dp::u8>(i % 256);
        }

        auto start = std::chrono::high_resolution_clock::now();
        auto response = remote.call(1, request, 30000);
        auto end = std::chrono::high_resolution_clock::now();

        if (response.is_ok()) {
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
            echo::info("Client received: ", response.value().size() / 1024 / 1024, "MB in ", duration, "ms");
        }

        stream.close();
    });

    server_thread.join();
    client_thread.join();
}

int main() {
    echo::info("=== SHM + Unidirect RPC ===");

    test_payload(1);    // 1MB
    test_payload(10);   // 10MB
    test_payload(100);  // 100MB
    test_payload(1024); // 1GB

    return 0;
}

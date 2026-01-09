/// Example: SHM + Remote<Bidirect> - TCP-like connection semantics
/// Demonstrates bidirectional RPC over shared memory with accept/connect pattern
///
/// Architecture:
/// - Server: listen_shm() -> accept() returns new ShmStream per client
/// - Client: connect_shm() establishes dedicated buffer pair
/// - Each connection gets independent s2c/c2s buffers
///
/// Test cases: 1MB, 10MB, 100MB, 1GB

#include <chrono>
#include <memory>
#include <netpipe/netpipe.hpp>
#include <thread>

void test_payload(dp::usize size_mb) {
    echo::info("=== Testing ", size_mb, "MB payload ===");

    netpipe::ShmStream listener;
    netpipe::ShmEndpoint endpoint{"netpipe_shm_bidirect", 256 * 1024 * 1024}; // 256MB buffer per connection

    auto listen_res = listener.listen_shm(endpoint);
    if (listen_res.is_err()) {
        echo::error("Server listen failed: ", listen_res.error().message.c_str());
        return;
    }

    echo::info("Server listening on SHM channel");

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

        // Create bidirectional Remote on accepted connection
        netpipe::Remote<netpipe::Bidirect> remote(*server_conn);

        // Register server's method (method_id = 1)
        remote.register_method(1, [](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            echo::info("Server received request: ", req.size() / 1024 / 1024, "MB");
            return dp::result::ok(req); // Echo back
        });

        echo::info("Server registered method 1, waiting for client...");

        // Wait for client to register its methods
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // Server calls client's method (method_id = 2)
        dp::usize size = size_mb * 1024 * 1024;
        netpipe::Message request(size);
        for (dp::usize i = 0; i < size; i++) {
            request[i] = static_cast<dp::u8>(i % 256);
        }

        echo::info("Server calling client's method 2...");
        auto start = std::chrono::high_resolution_clock::now();
        auto response = remote.call(2, request, 60000);
        auto end = std::chrono::high_resolution_clock::now();

        if (response.is_ok()) {
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
            echo::info("Server got response from client: ", response.value().size() / 1024 / 1024, "MB in ", duration,
                       "ms");
        } else {
            echo::error("Server call to client failed: ", response.error().message.c_str());
        }

        // Keep server alive to handle client's call
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    });

    // Client thread
    std::thread client_thread([&]() {
        // Wait for server to start listening
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        netpipe::ShmStream client;
        auto connect_res = client.connect_shm(endpoint);
        if (connect_res.is_err()) {
            echo::error("Client connect failed: ", connect_res.error().message.c_str());
            return;
        }

        echo::info("Client connected to SHM");

        // Create bidirectional Remote
        netpipe::Remote<netpipe::Bidirect> remote(client);

        // Register client's method (method_id = 2)
        remote.register_method(2, [](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            echo::info("Client received request: ", req.size() / 1024 / 1024, "MB");
            return dp::result::ok(req); // Echo back
        });

        echo::info("Client registered method 2");

        // Wait for server to register its methods
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Client calls server's method (method_id = 1)
        dp::usize size = size_mb * 1024 * 1024;
        netpipe::Message request(size);
        for (dp::usize i = 0; i < size; i++) {
            request[i] = static_cast<dp::u8>(i % 256);
        }

        echo::info("Client calling server's method 1...");
        auto start = std::chrono::high_resolution_clock::now();
        auto response = remote.call(1, request, 60000);
        auto end = std::chrono::high_resolution_clock::now();

        if (response.is_ok()) {
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
            echo::info("Client got response from server: ", response.value().size() / 1024 / 1024, "MB in ", duration,
                       "ms");
        } else {
            echo::error("Client call to server failed: ", response.error().message.c_str());
        }

        // Keep client alive to handle server's call
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    });

    server_thread.join();
    client_thread.join();

    listener.close();
    echo::info("");
}

int main() {
    echo::info("=== SHM + Bidirect RPC (TCP-like semantics) ===");
    echo::info("");

    test_payload(1);    // 1MB
    test_payload(10);   // 10MB
    test_payload(100);  // 100MB
    test_payload(1024); // 1GB

    echo::info("=== All tests complete ===");
    return 0;
}

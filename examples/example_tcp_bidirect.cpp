/// Example: TCP + Remote<Bidirect>
/// Bidirectional peer-to-peer RPC over TCP
/// Both peers can call each other's methods
/// 4 test cases: 1MB, 10MB, 100MB, 1GB

#include <chrono>
#include <netpipe/netpipe.hpp>
#include <thread>

void test_payload(dp::usize size_mb) {
    echo::info("Testing ", size_mb, "MB payload...");

    // Peer 1 thread (listens on port 9002)
    std::thread peer1_thread([size_mb]() {
        netpipe::TcpStream stream;
        netpipe::TcpEndpoint endpoint{"0.0.0.0", 9002};
        stream.listen(endpoint);

        auto peer = std::move(stream.accept().value());
        netpipe::Remote<netpipe::Bidirect> remote(*peer);

        // Register method handler for method 1
        remote.register_method(1, [](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            echo::info("Peer1 received method 1: ", req.size() / 1024 / 1024, "MB");
            return dp::result::ok(req); // Echo back
        });

        // Wait for peer2 to be ready
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // Create payload
        dp::usize size = size_mb * 1024 * 1024;
        netpipe::Message request(size);
        for (dp::usize i = 0; i < size; i++) {
            request[i] = static_cast<dp::u8>(i % 256);
        }

        // Call peer2's method 2
        auto start = std::chrono::high_resolution_clock::now();
        auto response = remote.call(2, request, 30000);
        auto end = std::chrono::high_resolution_clock::now();

        if (response.is_ok()) {
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
            echo::info("Peer1 received response: ", response.value().size() / 1024 / 1024, "MB in ", duration, "ms");
        }

        // Keep alive to handle peer2's call
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    });

    // Peer 2 thread (connects to peer1)
    std::thread peer2_thread([size_mb]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        netpipe::TcpStream stream;
        netpipe::TcpEndpoint endpoint{"127.0.0.1", 9002};
        stream.connect(endpoint);

        netpipe::Remote<netpipe::Bidirect> remote(stream);

        // Register method handler for method 2
        remote.register_method(2, [](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            echo::info("Peer2 received method 2: ", req.size() / 1024 / 1024, "MB");
            return dp::result::ok(req); // Echo back
        });

        // Wait for peer1 to call us first
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        // Create payload
        dp::usize size = size_mb * 1024 * 1024;
        netpipe::Message request(size);
        for (dp::usize i = 0; i < size; i++) {
            request[i] = static_cast<dp::u8>(i % 256);
        }

        // Call peer1's method 1
        auto start = std::chrono::high_resolution_clock::now();
        auto response = remote.call(1, request, 30000);
        auto end = std::chrono::high_resolution_clock::now();

        if (response.is_ok()) {
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
            echo::info("Peer2 received response: ", response.value().size() / 1024 / 1024, "MB in ", duration, "ms");
        }

        stream.close();
    });

    peer1_thread.join();
    peer2_thread.join();
}

int main() {
    echo::info("=== TCP + Bidirect RPC ===");

    test_payload(1);    // 1MB
    test_payload(10);   // 10MB
    test_payload(100);  // 100MB
    test_payload(1024); // 1GB

    return 0;
}

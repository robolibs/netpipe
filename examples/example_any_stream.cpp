#include <chrono>
#include <netpipe/netpipe.hpp>
#include <thread>

int main() {
    // IPC demo (local): server accepts one client, exchanges one message.
    // This is the main ergonomic win: same AnyStream API across transports.
    auto endpoint = netpipe::AnyEndpoint::ipc_endpoint("/tmp/netpipe_any_stream_example.sock");

    netpipe::AnyStream server;
    auto listen_res = server.listen(endpoint);
    if (listen_res.is_err()) {
        return 1;
    }

    std::thread client_thread([endpoint]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        netpipe::AnyStream client;
        if (client.connect(endpoint).is_err()) {
            return;
        }

        netpipe::Message msg = {0x01, 0x02, 0x03};
        (void)client.send(msg);

        (void)client.recv();

        client.close();
    });

    auto accept_res = server.accept();
    if (accept_res.is_err()) {
        client_thread.join();
        server.close();
        return 1;
    }

    auto conn = std::move(accept_res.value());
    auto req = conn.recv();
    if (req.is_ok()) {
        netpipe::Message resp = {0xAA, 0xBB, 0xCC};
        (void)conn.send(resp);
    }

    client_thread.join();
    conn.close();
    server.close();
    return 0;
}

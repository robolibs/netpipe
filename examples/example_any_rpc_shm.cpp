#include <chrono>
#include <netpipe/netpipe.hpp>
#include <thread>

static constexpr dp::u32 METHOD_ECHO = 1;

int main() {
    // One-file SHM + RPC example using AnyStream.
    // Server: listens on SHM, accepts one client, serves a single echo method.
    // Client: connects, calls echo, validates response.
    netpipe::AnyEndpoint endpoint = netpipe::AnyEndpoint::shm_endpoint("netpipe_any_rpc_shm", 64 * 1024);

    netpipe::AnyStream listener;
    auto listen_res = listener.listen(endpoint);
    if (listen_res.is_err()) {
        return 1;
    }

    std::thread server_thread([&listener]() {
        auto accept_res = listener.accept();
        if (accept_res.is_err()) {
            return;
        }

        auto conn = std::move(accept_res.value());
        // Note: `serve()` exists only on Remote<Unidirect> in netpipe.
        // For Bidirect, a receiver thread is started automatically by the constructor.
        netpipe::Remote<netpipe::Bidirect> rpc(*conn.get());

        (void)rpc.register_method(
            METHOD_ECHO, [](const netpipe::Message &req) -> dp::Res<netpipe::Message> { return dp::result::ok(req); });

        // Keep the server alive long enough for the client call.
        std::this_thread::sleep_for(std::chrono::seconds(1));
        conn.close();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    netpipe::AnyStream client;
    auto connect_res = client.connect(endpoint);
    if (connect_res.is_err()) {
        listener.close();
        server_thread.join();
        return 1;
    }

    netpipe::Remote<netpipe::Bidirect> rpc(*client.get());
    netpipe::Message payload = {'h', 'e', 'l', 'l', 'o'};
    auto resp = rpc.call(METHOD_ECHO, payload, 5000);
    if (resp.is_err()) {
        client.close();
        listener.close();
        server_thread.join();
        return 1;
    }

    // Expect exact echo.
    if (resp.value() != payload) {
        client.close();
        listener.close();
        server_thread.join();
        return 1;
    }

    client.close();
    listener.close();
    server_thread.join();
    return 0;
}

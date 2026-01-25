#include <chrono>
#include <datapod/datapod.hpp>
#include <netpipe/netpipe.hpp>
#include <string>
#include <thread>

static constexpr dp::u32 METHOD_ECHO = 1;

int main() {
    // SHM + RPC example using AnyStream.
    // Client sends a message via RPC; server prints it inside the handler.
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

        // Important: keep the stream alive longer than Remote (receiver thread).
        {
            netpipe::Remote<netpipe::Bidirect> rpc(*conn.get());
            (void)rpc.register_method(METHOD_ECHO, [](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
                std::string s(req.begin(), req.end());
                echo::info("RPC got: ", s.c_str());
                return dp::result::ok(req);
            });

            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

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

    bool ok = false;

    // Important: keep the stream alive longer than Remote (receiver thread).
    {
        netpipe::Remote<netpipe::Bidirect> rpc(*client.get());
        netpipe::Message payload = {'h', 'e', 'l', 'l', 'o'};
        auto resp = rpc.call(METHOD_ECHO, payload, 5000);
        ok = resp.is_ok() && resp.value() == payload;
    }

    client.close();
    listener.close();
    server_thread.join();

    return ok ? 0 : 1;
}

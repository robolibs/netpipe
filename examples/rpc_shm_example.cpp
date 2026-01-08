/// RPC over Shared Memory Example
/// Demonstrates using Remote RPC with ShmRpcStream transport
/// Ultra-low latency RPC for same-machine communication
/// Usage: ./rpc_shm_example server  OR  ./rpc_shm_example client

#include <chrono>
#include <netpipe/netpipe.hpp>
#include <netpipe/stream/shm_rpc.hpp>
#include <thread>

void server() {
    echo::info("Remote RPC Server (SHM) starting...");

    // Create server-side SHM RPC stream
    auto stream_res = netpipe::ShmRpcStream::create_server("netpipe_rpc", 65536);
    if (stream_res.is_err()) {
        echo::error("Failed to create SHM server: ", stream_res.error().message.c_str());
        return;
    }

    auto stream = std::move(stream_res.value());
    echo::info("SHM RPC server ready on channel: netpipe_rpc");

    // Create Remote RPC server
    netpipe::Remote remote(*stream);

    // Handler: echo back the request with "Response: " prefix
    // Returns error if request contains "error"
    auto handler = [](const netpipe::Message &request) -> dp::Res<netpipe::Message> {
        dp::String req_str(reinterpret_cast<const char *>(request.data()), request.size());
        echo::info("Handling request: ", req_str.c_str());

        // Simulate error handling - return error if request contains "error"
        if (req_str.find("error") != dp::String::npos) {
            echo::warn("Request contains 'error', returning error response");
            return dp::result::err(dp::Error::invalid_argument("Request contains 'error' keyword"));
        }

        dp::String response_str = dp::String("Response: ") + req_str;
        netpipe::Message response(response_str.begin(), response_str.end());
        return dp::result::ok(response);
    };

    echo::info("Remote RPC server (SHM) ready, handling requests...");
    auto serve_res = remote.serve(handler);
    if (serve_res.is_err()) {
        echo::error("Serve failed: ", serve_res.error().message.c_str());
    }
}

void client() {
    echo::info("Remote RPC Client (SHM) starting...");
    std::this_thread::sleep_for(std::chrono::seconds(1)); // Wait for server

    // Create client-side SHM RPC stream
    auto stream_res = netpipe::ShmRpcStream::create_client("netpipe_rpc", 65536);
    if (stream_res.is_err()) {
        echo::error("Failed to connect to SHM server: ", stream_res.error().message.c_str());
        return;
    }

    auto stream = std::move(stream_res.value());
    echo::info("Connected to SHM RPC server!");

    // Create Remote RPC client
    netpipe::Remote remote(*stream);

    // Make 4 Remote RPC calls (including one that triggers an error)
    for (int i = 0; i < 4; i++) {
        dp::String req_str;
        if (i == 2) {
            // This request will trigger an error on the server
            req_str = dp::String("Request with error keyword");
        } else {
            req_str = dp::String("Request #") + dp::String(std::to_string(i).c_str());
        }
        netpipe::Message request(req_str.begin(), req_str.end());

        echo::info("Calling Remote RPC: ", req_str.c_str());
        auto call_res = remote.call(request, 5000);

        if (call_res.is_err()) {
            echo::error("Remote RPC call failed: ", call_res.error().message.c_str());
            // Continue to next request instead of breaking
        } else {
            auto response = call_res.value();
            dp::String resp_str(reinterpret_cast<const char *>(response.data()), response.size());
            echo::info("Got response: ", resp_str.c_str());
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    stream->close();
    echo::info("Client done");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        echo::info("Usage: ", argv[0], " [server|client]");
        return 1;
    }

    dp::String mode(argv[1]);
    if (mode == "server") {
        server();
    } else if (mode == "client") {
        client();
    } else {
        echo::error("Unknown mode: ", mode.c_str());
        return 1;
    }

    return 0;
}

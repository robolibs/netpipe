/// RPC over IPC (Unix Domain Sockets) Example
/// Demonstrates using Remote RPC with IpcStream transport
/// Usage: ./rpc_ipc_example server  OR  ./rpc_ipc_example client

#include <chrono>
#include <netpipe/netpipe.hpp>
#include <thread>

void server() {
    echo::info("Remote RPC Server (IPC) starting...");

    netpipe::IpcStream stream;
    netpipe::IpcEndpoint endpoint{"/tmp/netpipe_rpc.sock"};

    auto res = stream.listen_ipc(endpoint);
    if (res.is_err()) {
        echo::error("Listen failed: ", res.error().message.c_str());
        return;
    }

    echo::info("Waiting for client on ", endpoint.path.c_str(), "...");
    auto client_res = stream.accept();
    if (client_res.is_err()) {
        echo::error("Accept failed: ", client_res.error().message.c_str());
        return;
    }

    auto client = std::move(client_res.value());
    echo::info("Client connected!");

    // Create Remote RPC server
    netpipe::Remote remote(*client);

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

    echo::info("Remote RPC server (IPC) ready, handling requests...");
    auto serve_res = remote.serve(handler);
    if (serve_res.is_err()) {
        echo::error("Serve failed: ", serve_res.error().message.c_str());
    }
}

void client() {
    echo::info("Remote RPC Client (IPC) starting...");
    std::this_thread::sleep_for(std::chrono::seconds(1)); // Wait for server

    netpipe::IpcStream stream;
    netpipe::IpcEndpoint endpoint{"/tmp/netpipe_rpc.sock"};

    auto res = stream.connect_ipc(endpoint);
    if (res.is_err()) {
        echo::error("Connect failed: ", res.error().message.c_str());
        return;
    }

    echo::info("Connected to server via IPC!");

    // Create Remote RPC client
    netpipe::Remote remote(stream);

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

    stream.close();
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

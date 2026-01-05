#include <chrono>
#include <netpipe/netpipe.hpp>
#include <thread>

void server() {
    echo::info("RPC Server starting...");

    netpipe::TcpStream stream;
    netpipe::TcpEndpoint endpoint{"0.0.0.0", 8080};

    auto res = stream.listen(endpoint);
    if (res.is_err()) {
        echo::error("Listen failed: ", res.error().message.c_str());
        return;
    }

    echo::info("Waiting for client...");
    auto client_res = stream.accept();
    if (client_res.is_err()) {
        echo::error("Accept failed: ", client_res.error().message.c_str());
        return;
    }

    auto client = std::move(client_res.value());
    echo::info("Client connected!");

    // Create RPC server
    netpipe::Rpc rpc(*client);

    // Handler: echo back the request with "Response: " prefix
    auto handler = [](const netpipe::Message &request) -> netpipe::Message {
        dp::String req_str(reinterpret_cast<const char *>(request.data()));
        echo::info("Handling request: ", req_str.c_str());

        dp::String response_str = dp::String("Response: ") + req_str;
        netpipe::Message response(response_str.begin(), response_str.end());
        return response;
    };

    echo::info("RPC server ready, handling requests...");
    auto serve_res = rpc.serve(handler);
    if (serve_res.is_err()) {
        echo::error("Serve failed: ", serve_res.error().message.c_str());
    }
}

void client() {
    echo::info("RPC Client starting...");
    std::this_thread::sleep_for(std::chrono::seconds(1)); // Wait for server

    netpipe::TcpStream stream;
    netpipe::TcpEndpoint endpoint{"127.0.0.1", 8080};

    auto res = stream.connect(endpoint);
    if (res.is_err()) {
        echo::error("Connect failed: ", res.error().message.c_str());
        return;
    }

    echo::info("Connected to server!");

    // Create RPC client
    netpipe::Rpc rpc(stream);

    // Make 3 RPC calls
    for (int i = 0; i < 3; i++) {
        dp::String req_str = dp::String("Request #") + dp::String(std::to_string(i).c_str());
        netpipe::Message request(req_str.begin(), req_str.end());

        echo::info("Calling RPC: ", req_str.c_str());
        auto call_res = rpc.call(request, 5000);

        if (call_res.is_err()) {
            echo::error("RPC call failed: ", call_res.error().message.c_str());
            break;
        }

        auto response = call_res.value();
        dp::String resp_str(reinterpret_cast<const char *>(response.data()));
        echo::info("Got response: ", resp_str.c_str());

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

#include <chrono>
#include <netpipe/netpipe.hpp>
#include <thread>

static constexpr dp::u32 METHOD_ADD = 1;
static constexpr dp::u32 METHOD_GREET = 2;

int main() {
    // Pipe + RPC example: demonstrates using Pipe with bidirectional RPC.
    // Works with any transport - just change the endpoint.
    auto endpoint = netpipe::AnyEndpoint::ipc_endpoint("/tmp/netpipe_pipe_rpc.sock");

    // Server: listen and handle RPC calls
    auto server_res = netpipe::Pipe::listen(endpoint);
    if (server_res.is_err()) {
        echo::error("Server listen failed");
        return 1;
    }
    auto server = std::move(server_res.value());

    std::thread server_thread([&server]() {
        auto conn_res = server.accept();
        if (conn_res.is_err()) {
            echo::error("Server accept failed");
            return;
        }
        auto conn = std::move(conn_res.value());

        // Create RPC handler on the connection
        netpipe::Remote<netpipe::Bidirect> rpc(*conn.stream().get());

        // Register method: add two numbers
        rpc.register_method(METHOD_ADD, [](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            if (req.size() < 8) {
                return dp::result::err(dp::Error::invalid_argument("need 2 integers"));
            }
            dp::i32 a = *reinterpret_cast<const dp::i32 *>(req.data());
            dp::i32 b = *reinterpret_cast<const dp::i32 *>(req.data() + 4);
            dp::i32 result = a + b;
            echo::info("Server: ", a, " + ", b, " = ", result);

            netpipe::Message resp(4);
            *reinterpret_cast<dp::i32 *>(resp.data()) = result;
            return dp::result::ok(std::move(resp));
        });

        // Register method: greet
        rpc.register_method(METHOD_GREET, [](const netpipe::Message &req) -> dp::Res<netpipe::Message> {
            dp::String name(reinterpret_cast<const char *>(req.data()), req.size());
            dp::String greeting = dp::String("Hello, ") + name + "!";
            echo::info("Server greeting: ", greeting);

            netpipe::Message resp(greeting.begin(), greeting.end());
            return dp::result::ok(std::move(resp));
        });

        // Keep server alive for client calls
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        conn.close();
    });

    // Give server time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Client: connect and make RPC calls
    auto client_res = netpipe::Pipe::connect(endpoint);
    if (client_res.is_err()) {
        echo::error("Client connect failed");
        server.close();
        server_thread.join();
        return 1;
    }
    auto client = std::move(client_res.value());

    bool success = true;
    {
        netpipe::Remote<netpipe::Bidirect> rpc(*client.stream().get());

        // Call add(10, 32)
        {
            netpipe::Message req(8);
            *reinterpret_cast<dp::i32 *>(req.data()) = 10;
            *reinterpret_cast<dp::i32 *>(req.data() + 4) = 32;

            auto resp = rpc.call(METHOD_ADD, req, 5000);
            if (resp.is_ok() && resp.value().size() >= 4) {
                dp::i32 result = *reinterpret_cast<const dp::i32 *>(resp.value().data());
                echo::info("Client: add result = ", result);
                success = success && (result == 42);
            } else {
                echo::error("Client: add call failed");
                success = false;
            }
        }

        // Call greet("World")
        {
            dp::String name = "World";
            netpipe::Message req(name.begin(), name.end());

            auto resp = rpc.call(METHOD_GREET, req, 5000);
            if (resp.is_ok()) {
                dp::String greeting(reinterpret_cast<const char *>(resp.value().data()), resp.value().size());
                echo::info("Client: greeting = ", greeting);
                success = success && (greeting == "Hello, World!");
            } else {
                echo::error("Client: greet call failed");
                success = false;
            }
        }
    }

    client.close();
    server.close();
    server_thread.join();

    echo::info(success ? "Pipe RPC example: SUCCESS" : "Pipe RPC example: FAILED");
    return success ? 0 : 1;
}

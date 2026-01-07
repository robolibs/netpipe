# Netpipe

Minimal peer-to-peer transport library for C++20 with honest semantics and modern Remote RPC.

## Development Status

See [TODO.md](./TODO.md) for the complete development plan and current progress.

## Overview

**Netpipe** provides two transport families (Stream and Datagram) with multiple implementations, plus a feature-complete Remote RPC layer. It's designed with honest semantics - different transports behave differently, no leaky abstractions.

**Key Features:**
- **Header-only** - No linking required
- **Two transport families** - Stream (reliable, ordered) and Datagram (unreliable, connectionless)
- **Modern Remote RPC** - Routing, streaming, metrics, cancellation, bidirectional
- **Honest semantics** - TCP ≠ UDP ≠ SHM, each behaves as expected
- **No exceptions** - All errors via `dp::Result<T, Error>`
- **Modern C++20** - Uses `datapod` types throughout

### Architecture

```
┌─────────────────────────────────────────────────────────────────────────┐
│                              NETPIPE                                     │
├──────────────────────┬──────────────────────┬───────────────────────────┤
│   Remote RPC Layer   │                      │                           │
│  ┌────────────────┐  │                      │                           │
│  │ RemoteRouter   │  │  Stream Family       │   Datagram Family         │
│  │ RemoteAsync    │  │  (Reliable)          │   (Unreliable)            │
│  │ RemotePeer     │  │                      │                           │
│  │ Streaming      │  │  ┌──────────────┐    │   ┌──────────────┐        │
│  │ TypedRemote    │  │  │  TcpStream   │    │   │ UdpDatagram  │        │
│  │ Metrics        │  │  │  IpcStream   │    │   │ LoraDatagram │        │
│  └────────────────┘  │  │  ShmStream   │    │   └──────────────┘        │
│                      │  └──────────────┘    │                           │
└──────────────────────┴──────────────────────┴───────────────────────────┘
           │                      │                        │
           └──────────────────────┴────────────────────────┘
                                  │
                          ┌───────▼────────┐
                          │   Your App     │
                          └────────────────┘
```

**Remote Protocol Evolution:**
```
V1 (Legacy):  [request_id:4][is_error:1][length:4][payload:N]  (9 bytes)
              └─ Basic request/response

V2 (Current): [version:1][type:1][flags:2][request_id:4][method_id:4][length:4][payload:N]  (16 bytes)
              └─ Routing, streaming, metrics, cancellation
```

## Installation

### Quick Start (CMake FetchContent)

```cmake
include(FetchContent)
FetchContent_Declare(
  netpipe
  GIT_REPOSITORY https://github.com/robolibs/netpipe
  GIT_TAG main
)
FetchContent_MakeAvailable(netpipe)

target_link_libraries(your_target PRIVATE netpipe)
```

### Recommended: XMake

[XMake](https://xmake.io/) is a modern, fast, and cross-platform build system.

**Install XMake:**
```bash
curl -fsSL https://xmake.io/shget.text | bash
```

**Add to your xmake.lua:**
```lua
add_requires("netpipe")

target("your_target")
    set_kind("binary")
    add_packages("netpipe")
    add_files("src/*.cpp")
```

**Build:**
```bash
xmake
xmake run
```

## Usage

### Basic TCP Echo

```cpp
#include <netpipe/netpipe.hpp>

// Server
netpipe::TcpStream server;
server.listen({"0.0.0.0", 7447});
auto client = server.accept().value();

while (true) {
    auto msg = client->recv();
    if (msg.is_ok()) client->send(msg.value());
}

// Client
netpipe::TcpStream stream;
stream.connect({"127.0.0.1", 7447});
netpipe::Message msg = {0x48, 0x65, 0x6c, 0x6c, 0x6f}; // "Hello"
stream.send(msg);
auto echo = stream.recv().value();
```

### Remote RPC with Method Routing

```cpp
#include <netpipe/netpipe.hpp>

// Server
netpipe::TcpStream stream;
stream.listen({"0.0.0.0", 8080});
auto client = stream.accept().value();

netpipe::RemoteRouter router(*client);

// Register multiple methods
router.register_method(1, [](const netpipe::Message& req) -> dp::Res<netpipe::Message> {
    return dp::result::ok(req); // Echo
});

router.register_method(2, [](const netpipe::Message& req) -> dp::Res<netpipe::Message> {
    netpipe::Message result = {req[0] + req[1]}; // Add
    return dp::result::ok(result);
});

router.serve();

// Client
netpipe::TcpStream stream;
stream.connect({"127.0.0.1", 8080});

netpipe::RemoteAsync remote(stream);

auto resp1 = remote.call(1, {0x01, 0x02}, 5000); // Echo
auto resp2 = remote.call(2, {5, 10}, 5000);      // Add -> 15
```

### Bidirectional Remote (Peer-to-Peer)

```cpp
// Both peers can call each other!
netpipe::RemotePeer peer(stream);

// Register methods (server side)
peer.register_method(1, [](const netpipe::Message& req) -> dp::Res<netpipe::Message> {
    return dp::result::ok(req);
});

// Make calls (client side)
auto response = peer.call(2, {0x01}, 5000);
```

### Remote with Metrics

```cpp
// Enable metrics
netpipe::RemoteAsync remote(stream, 100, true); // max_concurrent=100, metrics=true

remote.call(1, request, 5000);

// Get metrics
const auto& metrics = remote.get_metrics();
echo::info("Total: ", metrics.total_requests.load());
echo::info("Success rate: ", metrics.success_rate() * 100, "%");
echo::info("Avg latency: ", metrics.avg_latency_us(), " μs");
echo::info("In-flight: ", metrics.in_flight_requests.load());
```

### Streaming Remote

```cpp
netpipe::StreamingRemote streaming(stream);

// Client streaming: multiple requests → one response
dp::Vector<netpipe::Message> chunks = {{0x01}, {0x02}, {0x03}};
auto result = streaming.client_stream(1, chunks, 5000);

// Server streaming: one request → multiple responses
streaming.server_stream(2, request, [](const netpipe::Message& chunk) {
    echo::info("Chunk: ", chunk.size(), " bytes");
}, 10000);

// Bidirectional streaming
auto stream_id = streaming.bidirectional_stream(3, [](const netpipe::Message& chunk) {
    // Receive chunks
}).value();

streaming.send_chunk(stream_id, {0x01, 0x02});
streaming.send_chunk(stream_id, {0x03, 0x04}, true); // final
streaming.end_stream(stream_id);
```

### Type-Safe Remote

```cpp
struct Point {
    int x, y;
    auto members() { return std::tie(x, y); }
};

netpipe::TypedRemote<Point> remote(stream);

Point request{10, 20};
auto response = remote.call(1, request, 5000);

if (response.is_ok()) {
    Point result = response.value();
    // result.x, result.y
}
```

### Request Cancellation

```cpp
netpipe::RemoteAsync remote(stream);

std::thread call_thread([&]() {
    auto resp = remote.call(1, request, 30000);
    if (resp.is_err() && resp.error().message == "request cancelled") {
        echo::info("Cancelled!");
    }
});

bool cancelled = remote.cancel(0); // request_id = 0
call_thread.join();
```

### Protocol Version Compatibility

```cpp
// V2 (current) - full features
netpipe::RemoteAsync remote(stream);
remote.call(method_id, request, timeout);

// V1 (legacy) - backward compatible
netpipe::Remote remote(stream);
remote.call(request, timeout); // No method_id, single handler

// Auto-detection works transparently
// V1 clients can talk to V2 servers and vice versa
```

## Features

- **Multiple Transports**
  - **TcpStream** - Network communication with length-prefix framing
  - **IpcStream** - Unix domain sockets for local IPC
  - **ShmStream** - Zero-copy shared memory with lock-free ring buffer
  - **UdpDatagram** - UDP with broadcast support
  - **LoraDatagram** - LoRa mesh via melodi serial protocol

- **Modern Remote RPC**
  - **Method routing** - Multiple methods per service
  - **Concurrent requests** - Out-of-order responses, thread-safe
  - **Bidirectional** - Peer-to-peer, both sides call each other
  - **Streaming** - Client, server, and bidirectional streaming
  - **Type-safe** - Serialization helpers for custom types
  - **Metrics** - Latency, success rate, in-flight tracking
  - **Cancellation** - Cancel in-flight requests
  - **Versioned protocol** - V1/V2 auto-detection, backward compatible

- **Design Philosophy**
  - **Honest semantics** - TCP ≠ UDP ≠ SHM, no false abstractions
  - **Blocking API** - Users handle async (threads, futures, etc.)
  - **No exceptions** - All errors via `dp::Result<T, Error>`
  - **Header-only** - Just include and use
  - **Modern C++20** - Uses `datapod` types, `echo` logging

## Building

```bash
make config   # Configure build
make build    # Build library and examples
make test     # Run tests (15 test suites, 284+ assertions)
make clean    # Clean build artifacts
```

**Build system options:**
```bash
BUILD_SYSTEM=cmake make build   # Use CMake
BUILD_SYSTEM=xmake make build   # Use XMake (default)
BUILD_SYSTEM=zig make build     # Use Zig
```

## Examples

The `examples/` directory contains:
- **rpc_example.cpp** - Remote client/server with routing
- **tcp_echo_server.cpp** / **tcp_echo_client.cpp** - Basic TCP
- **udp_broadcast.cpp** - UDP broadcast sender/receiver
- **ethernet_tunnel.cpp** - Wirebit Ethernet L2 tunneling
- **tap_tunnel.cpp** - Real Linux TAP interfaces (requires sudo)

**Run examples:**
```bash
./build/linux/x86_64/release/rpc_example server
./build/linux/x86_64/release/rpc_example client
```

## Wire Protocol

**Protocol V2** (Current):
```
[version:1][type:1][flags:2][request_id:4][method_id:4][length:4][payload:N]

version: 1=V1, 2=V2
type: 0=Request, 1=Response, 2=Error, 4=StreamData, 5=StreamEnd, 6=StreamError, 7=Cancel
flags: 0x0001=Compressed, 0x0002=Streaming, 0x0004=RequiresAck, 0x0008=Final
```

**Protocol V1** (Legacy, backward compatible):
```
[request_id:4][is_error:1][length:4][payload:N]
```

V1 messages are auto-detected and converted to V2 internally.

## API Reference

### Remote Classes

```cpp
// Basic Remote - single handler
class Remote {
    explicit Remote(Stream& stream);
    Res<Message> call(const Message& request, dp::u32 timeout_ms);
};

// RemoteRouter - multiple methods
class RemoteRouter {
    explicit RemoteRouter(Stream& stream);
    dp::Res<void> register_method(dp::u32 method_id, Handler handler);
    VoidRes serve();
};

// RemoteAsync - concurrent requests
class RemoteAsync {
    explicit RemoteAsync(Stream& stream, dp::usize max_concurrent = 100, bool enable_metrics = false);
    Res<Message> call(dp::u32 method_id, const Message& request, dp::u32 timeout_ms);
    bool cancel(dp::u32 request_id);
    const RemoteMetrics& get_metrics() const;
};

// RemotePeer - bidirectional
class RemotePeer {
    explicit RemotePeer(Stream& stream, dp::usize max_concurrent = 100, bool enable_metrics = false);
    dp::Res<void> register_method(dp::u32 method_id, Handler handler);
    Res<Message> call(dp::u32 method_id, const Message& request, dp::u32 timeout_ms);
    bool cancel(dp::u32 request_id);
};

// StreamingRemote - streaming support
class StreamingRemote {
    explicit StreamingRemote(Stream& stream);
    Res<Message> client_stream(dp::u32 method_id, const dp::Vector<Message>& chunks, dp::u32 timeout_ms);
    Res<void> server_stream(dp::u32 method_id, const Message& request, StreamCallback callback, dp::u32 timeout_ms);
    Res<dp::u32> bidirectional_stream(dp::u32 method_id, StreamCallback callback);
};

// TypedRemote - type-safe
template<typename T>
class TypedRemote {
    explicit TypedRemote(Stream& stream);
    Res<T> call(dp::u32 method_id, const T& request, dp::u32 timeout_ms);
};
```

### Common Types

```cpp
using Message = dp::Vector<dp::u8>;
using Handler = std::function<dp::Res<Message>(const Message&)>;
using StreamCallback = std::function<void(const Message&)>;
```

## License

MIT License - see [LICENSE](./LICENSE) for details.

## Acknowledgments

Made possible thanks to [these amazing projects](./ACKNOWLEDGMENTS.md).

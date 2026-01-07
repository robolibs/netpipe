# Netpipe

Minimal peer-to-peer transport library for C++20 with honest semantics and modern Remote RPC.

## Development Status

See [TODO.md](./TODO.md) for the complete development plan and current progress.

## Overview

**Netpipe** provides two transport families (Stream and Datagram) with multiple implementations, plus a feature-complete Remote RPC layer. It's designed with honest semantics - different transports behave differently, no leaky abstractions.

**Key Features:**
- **Header-only** - No linking required
- **Two transport families** - Stream (reliable, ordered) and Datagram (unreliable, connectionless)
- **Multiple transports** - TCP, IPC, SHM, UDP, LoRa
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

**Transport Characteristics:**
```
TcpStream:   Network, reliable, ordered, connection-oriented
IpcStream:   Local, reliable, ordered, Unix domain sockets
ShmStream:   Local, reliable, ordered, zero-copy, lock-free ring buffer
UdpDatagram: Network, unreliable, connectionless, broadcast support
LoraDatagram: Long-range, unreliable, mesh networking, low bandwidth
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

### TCP Stream

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

### IPC Stream (Unix Domain Sockets)

```cpp
netpipe::IpcStream server;
server.listen({"/tmp/my.sock"});
auto client = server.accept().value();

// Same API as TCP
auto msg = client->recv();
```

### Shared Memory Stream (Zero-Copy)

```cpp
// Server
netpipe::ShmStream server;
server.listen({"my_shm", 1024*1024}); // 1MB buffer
auto client = server.accept().value();

// Client
netpipe::ShmStream stream;
stream.connect({"my_shm", 1024*1024});

// Zero-copy, lock-free ring buffer!
stream.send(msg);
```

### UDP Datagram (Broadcast)

```cpp
netpipe::UdpDatagram udp;
udp.bind({"0.0.0.0", 9000});

// Send broadcast
netpipe::UdpEndpoint broadcast{"255.255.255.255", 9000};
udp.send_to({0x01, 0x02, 0x03}, broadcast);

// Receive from anyone
auto [msg, sender] = udp.recv_from().value();
echo::info("From: ", sender.host.c_str(), ":", sender.port);
```

### LoRa Datagram (Mesh Networking)

```cpp
netpipe::LoraDatagram lora;
lora.bind({"/dev/ttyUSB0", "fe80::1"});

// Send to mesh node
netpipe::LoraEndpoint dest{"/dev/ttyUSB0", "fe80::2"};
lora.send_to(msg, dest);

// Receive from mesh
auto [msg, sender] = lora.recv_from().value();
```

### Remote RPC with Routing

```cpp
// Server
netpipe::TcpStream stream;
stream.listen({"0.0.0.0", 8080});
auto client = stream.accept().value();

netpipe::RemoteRouter router(*client);

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
netpipe::RemotePeer peer(stream);

// Register methods (server side)
peer.register_method(1, [](const netpipe::Message& req) -> dp::Res<netpipe::Message> {
    return dp::result::ok(req);
});

// Make calls (client side)
auto response = peer.call(2, {0x01}, 5000);
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

### Remote with Metrics

```cpp
netpipe::RemoteAsync remote(stream, 100, true); // metrics=true

remote.call(1, request, 5000);

const auto& metrics = remote.get_metrics();
echo::info("Total: ", metrics.total_requests.load());
echo::info("Success rate: ", metrics.success_rate() * 100, "%");
echo::info("Avg latency: ", metrics.avg_latency_us(), " μs");
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
```

### Wirebit Integration (TAP Tunneling)

```cpp
#include <netpipe/netpipe.hpp>
#include <wirebit/wirebit.hpp>

// Create REAL Linux TAP interface (requires sudo!)
auto tap_link = wirebit::TapLink::create({
    .interface_name = "tap0",
    .create_if_missing = true,
    .destroy_on_close = true,
    .set_up_on_create = true
}).value();

wirebit::EthEndpoint eth(std::make_shared<wirebit::TapLink>(std::move(tap_link)),
                         config, 1, mac_addr);

// Assign IP
system("sudo ip addr add 10.0.0.1/24 dev tap0");

// Tunnel over TCP
netpipe::TcpStream stream;
stream.listen({"0.0.0.0", 9001});
auto client = stream.accept().value();

while (true) {
    // TAP -> TCP
    auto frame = eth.recv_eth();
    if (frame.is_ok()) {
        netpipe::Message msg(frame.value().begin(), frame.value().end());
        client->send(msg);
    }
    
    // TCP -> TAP
    auto tcp_msg = client->recv();
    if (tcp_msg.is_ok()) {
        wirebit::Bytes frame(tcp_msg.value().begin(), tcp_msg.value().end());
        eth.send_eth(frame);
    }
}

// Now you can: ping 10.0.0.2, tcpdump -i tap0, ssh through tunnel!
```

## Features

- **Stream Transports**
  - **TcpStream** - Network communication with length-prefix framing
  - **IpcStream** - Unix domain sockets for local IPC
  - **ShmStream** - Zero-copy shared memory with lock-free ring buffer

- **Datagram Transports**
  - **UdpDatagram** - UDP with broadcast support
  - **LoraDatagram** - LoRa mesh via melodi serial protocol

- **Modern Remote RPC**
  - **Method routing** - Multiple methods per service (RemoteRouter)
  - **Concurrent requests** - Out-of-order responses, thread-safe (RemoteAsync)
  - **Bidirectional** - Peer-to-peer, both sides call each other (RemotePeer)
  - **Streaming** - Client, server, and bidirectional streaming (StreamingRemote)
  - **Type-safe** - Serialization helpers for custom types (TypedRemote)
  - **Metrics** - Latency, success rate, in-flight tracking
  - **Cancellation** - Cancel in-flight requests
  - **Versioned protocol** - V1/V2 auto-detection, backward compatible

- **Design Philosophy**
  - **Honest semantics** - TCP ≠ UDP ≠ SHM, no false abstractions
  - **Blocking API** - Users handle async (threads, futures, etc.)
  - **No exceptions** - All errors via `dp::Result<T, Error>`
  - **Header-only** - Just include and use
  - **Modern C++20** - Uses `datapod` types, `echo` logging

- **Wirebit Integration**
  - **Ethernet L2 tunneling** - Tunnel raw Ethernet frames
  - **TAP interfaces** - Create real Linux network interfaces
  - **VPN/Bridge** - Build custom network topologies
  - **Hardware protocols** - SPI, I2C, UART tunneling

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
- **tcp_echo_server.cpp** / **tcp_echo_client.cpp** - Basic TCP echo
- **udp_broadcast.cpp** - UDP broadcast sender/receiver
- **rpc_example.cpp** - Remote client/server with routing
- **ethernet_tunnel.cpp** - Wirebit Ethernet L2 tunneling (simulation)
- **tap_tunnel.cpp** - Real Linux TAP interfaces (requires sudo)
- **pose_tunnel.cpp** - Pose data tunneling with Wirebit
- **type_tagged_tunnel.cpp** - Type-tagged frame tunneling

**Run examples:**
```bash
./build/linux/x86_64/release/tcp_echo_server
./build/linux/x86_64/release/tcp_echo_client

./build/linux/x86_64/release/udp_broadcast

./build/linux/x86_64/release/rpc_example server
./build/linux/x86_64/release/rpc_example client

# TAP tunnel (requires sudo and wirebit)
sudo ./build/linux/x86_64/release/tap_tunnel server
sudo ./build/linux/x86_64/release/tap_tunnel client
# Then: ping 10.0.0.2, tcpdump -i tap0, ip link show tap0
```

## Wire Protocols

**Stream Framing** (TCP, IPC, SHM):
```
[length:4 bytes big-endian][payload:N bytes]
```

**Remote Protocol V2** (Current):
```
[version:1][type:1][flags:2][request_id:4][method_id:4][length:4][payload:N]

version: 1=V1, 2=V2
type: 0=Request, 1=Response, 2=Error, 4=StreamData, 5=StreamEnd, 6=StreamError, 7=Cancel
flags: 0x0001=Compressed, 0x0002=Streaming, 0x0004=RequiresAck, 0x0008=Final
```

**Remote Protocol V1** (Legacy, backward compatible):
```
[request_id:4][is_error:1][length:4][payload:N]
```

**LoRa Melodi Protocol** (Serial at 115200 baud):
```
TX <ipv6_addr> <hex_payload>
RX <ipv6_addr> <hex_payload>
```

## API Reference

### Endpoints

```cpp
struct TcpEndpoint { dp::String host; dp::u16 port; };
using UdpEndpoint = TcpEndpoint;

struct IpcEndpoint { dp::String path; };

struct ShmEndpoint { dp::String name; dp::usize size; };

struct LoraEndpoint { dp::String device; dp::String address; };
```

### Stream Interface

```cpp
class Stream {
    virtual VoidRes listen(const Endpoint& endpoint) = 0;
    virtual Res<dp::Box<Stream>> accept() = 0;
    virtual VoidRes connect(const Endpoint& endpoint) = 0;
    virtual Res<Message> recv() = 0;
    virtual VoidRes send(const Message& msg) = 0;
    virtual VoidRes close() = 0;
    virtual bool is_connected() const = 0;
};
```

### Datagram Interface

```cpp
class Datagram {
    virtual VoidRes bind(const Endpoint& endpoint) = 0;
    virtual Res<dp::Pair<Message, Endpoint>> recv_from() = 0;
    virtual VoidRes send_to(const Message& msg, const Endpoint& endpoint) = 0;
    virtual VoidRes close() = 0;
    virtual bool is_bound() const = 0;
};
```

### Remote Classes

```cpp
class Remote;              // Basic Remote - single handler
class RemoteRouter;        // Multiple methods with routing
class RemoteAsync;         // Concurrent requests, metrics
class RemotePeer;          // Bidirectional peer-to-peer
class StreamingRemote;     // Streaming support
template<typename T>
class TypedRemote;         // Type-safe with serialization
```

## Error Handling

All operations return `dp::Res<T>`:

```cpp
auto res = stream.connect(endpoint);
if (res.is_err()) {
    echo::error("Error: ", res.error().message.c_str());
    return;
}

// Or unwrap (panics on error)
stream.connect(endpoint).unwrap();
```

## Logging

Uses `echo` library with compile-time filtering:

```cpp
#define LOGLEVEL Debug
#include <netpipe/netpipe.hpp>

echo::trace()  // Internal details
echo::debug()  // State changes
echo::info()   // Significant events
echo::warn()   // Recoverable issues
echo::error()  // Failures
```

## License

MIT License - see [LICENSE](./LICENSE) for details.

## Acknowledgments

Made possible thanks to [these amazing projects](./ACKNOWLEDGMENTS.md).

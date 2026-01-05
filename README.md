# Netpipe

**Netpipe** is a minimal peer-to-peer transport library for C++20 with honest semantics. It provides two transport families (Stream and Datagram) with multiple implementations, plus an RPC layer on top.

## Features

- **Header-only** - No linking required, just include and use
- **Two transport families**:
  - **Stream**: Reliable, ordered, connection-oriented (TCP, IPC, SHM)
  - **Datagram**: Unreliable, connectionless (UDP, LoRa)
- **Honest semantics** - No leaky abstractions, different transports behave differently
- **Blocking API** - Users handle async themselves (threads, futures, etc.)
- **No exceptions** - All errors via `dp::Result<T, Error>`
- **Comprehensive logging** - Uses `echo` library with compile-time filtering
- **Modern C++20** - Uses `datapod` types throughout

## Transport Implementations

### Stream (Reliable, Ordered, Connection-Oriented)

| Transport | Description | Use Case |
|-----------|-------------|----------|
| **TcpStream** | TCP/IP sockets with length-prefix framing | Network communication |
| **IpcStream** | Unix domain sockets | Local inter-process communication |
| **ShmStream** | POSIX shared memory with lock-free ring buffer | High-performance local IPC |

### Datagram (Unreliable, Connectionless)

| Transport | Description | Use Case |
|-----------|-------------|----------|
| **UdpDatagram** | UDP/IP with broadcast support | Local network discovery, multicast |
| **LoraDatagram** | LoRa mesh via melodi serial protocol | Long-range wireless mesh networks |

### Higher-Level Protocols

| Protocol | Description | Use Case |
|----------|-------------|----------|
| **Rpc** | Request/response RPC over any Stream | Client-server applications |

## Quick Start

### Installation

Netpipe is header-only. Just include it in your project:

```cpp
#include <netpipe/netpipe.hpp>
```

### Dependencies

- **datapod** - Types and error handling (`dp::Vector`, `dp::Result`, `dp::Error`, etc.)
- **echo** - Logging with compile-time filtering

### TCP Echo Server

```cpp
#include <netpipe/stream/tcp.hpp>

int main() {
    netpipe::TcpStream server;
    netpipe::TcpEndpoint endpoint{"0.0.0.0", 7447};

    // Start listening
    auto res = server.listen(endpoint);
    if (res.is_err()) {
        echo::error("Failed to listen: ", res.error().message.c_str());
        return 1;
    }

    // Accept connection
    auto client_res = server.accept();
    if (client_res.is_err()) {
        echo::error("Failed to accept: ", client_res.error().message.c_str());
        return 1;
    }

    auto client = std::move(client_res.value());

    // Echo loop
    while (true) {
        auto msg_res = client->recv();
        if (msg_res.is_err()) break;
        
        auto msg = msg_res.value();
        client->send(msg);
    }

    return 0;
}
```

### TCP Echo Client

```cpp
#include <netpipe/stream/tcp.hpp>

int main() {
    netpipe::TcpStream stream;
    netpipe::TcpEndpoint endpoint{"127.0.0.1", 7447};

    // Connect to server
    auto res = stream.connect(endpoint);
    if (res.is_err()) {
        echo::error("Connect failed: ", res.error().message.c_str());
        return 1;
    }

    // Send message
    netpipe::Message msg = {0x48, 0x65, 0x6c, 0x6c, 0x6f}; // "Hello"
    stream.send(msg);

    // Receive echo
    auto recv_res = stream.recv();
    if (recv_res.is_ok()) {
        auto echo_msg = recv_res.value();
        // Process echo_msg
    }

    return 0;
}
```

### UDP Broadcast

```cpp
#include <netpipe/datagram/udp.hpp>

int main() {
    netpipe::UdpDatagram udp;
    netpipe::UdpEndpoint local{"0.0.0.0", 9000};
    
    auto res = udp.bind(local);
    if (res.is_err()) return 1;

    // Send broadcast
    netpipe::UdpEndpoint broadcast{"255.255.255.255", 9000};
    netpipe::Message msg = {0x01, 0x02, 0x03};
    udp.send_to(msg, broadcast);

    // Receive from anyone
    auto recv_res = udp.recv_from();
    if (recv_res.is_ok()) {
        auto [msg, sender] = recv_res.value();
        echo::info("Received from ", sender.host.c_str(), ":", sender.port);
    }

    return 0;
}
```

### RPC Example

```cpp
#include <netpipe/netpipe.hpp>

// Server
void server() {
    netpipe::TcpStream stream;
    stream.listen({"0.0.0.0", 8080});
    auto client = stream.accept().value();
    
    netpipe::Rpc rpc(*client);
    
    auto handler = [](const netpipe::Message& req) -> netpipe::Message {
        // Process request, return response
        return req; // Echo
    };
    
    rpc.serve(handler);
}

// Client
void client() {
    netpipe::TcpStream stream;
    stream.connect({"127.0.0.1", 8080});
    
    netpipe::Rpc rpc(stream);
    
    netpipe::Message request = {0x01, 0x02};
    auto response = rpc.call(request, 5000); // 5 second timeout
    
    if (response.is_ok()) {
        // Process response
    }
}
```

## API Reference

### Common Types

```cpp
namespace netpipe {
    // Message type - vector of bytes
    using Message = dp::Vector<dp::u8>;
    
    // Result types
    template<typename T>
    using Res = dp::Res<T>;
    using VoidRes = dp::VoidRes;
}
```

### Endpoints

```cpp
// TCP/UDP endpoint
struct TcpEndpoint {
    dp::String host;  // IP address or hostname
    dp::u16 port;     // Port number
};
using UdpEndpoint = TcpEndpoint;

// IPC endpoint
struct IpcEndpoint {
    dp::String path;  // Unix socket path
};

// Shared memory endpoint
struct ShmEndpoint {
    dp::String name;  // Shared memory name
    dp::usize size;   // Buffer size in bytes
};

// LoRa endpoint
struct LoraEndpoint {
    dp::String device;  // Serial device path (e.g., "/dev/ttyUSB0")
    dp::String address; // IPv6 address for routing
};
```

### Stream Interface

All stream implementations provide:

```cpp
class Stream {
    // Server operations
    virtual VoidRes listen(const Endpoint& endpoint) = 0;
    virtual Res<dp::Box<Stream>> accept() = 0;
    
    // Client operations
    virtual VoidRes connect(const Endpoint& endpoint) = 0;
    
    // I/O operations
    virtual Res<Message> recv() = 0;
    virtual VoidRes send(const Message& msg) = 0;
    
    // Lifecycle
    virtual VoidRes close() = 0;
    virtual bool is_connected() const = 0;
};
```

### Datagram Interface

All datagram implementations provide:

```cpp
class Datagram {
    // Bind to local endpoint
    virtual VoidRes bind(const Endpoint& endpoint) = 0;
    
    // I/O operations
    virtual Res<dp::Pair<Message, Endpoint>> recv_from() = 0;
    virtual VoidRes send_to(const Message& msg, const Endpoint& endpoint) = 0;
    
    // Lifecycle
    virtual VoidRes close() = 0;
    virtual bool is_bound() const = 0;
};
```

### RPC Interface

```cpp
class Rpc {
    // Constructor takes any Stream
    explicit Rpc(Stream& stream);
    
    // Client: Make RPC call with timeout (milliseconds)
    Res<Message> call(const Message& request, dp::u32 timeout_ms);
    
    // Server: Handle requests with handler function
    template<typename Handler>
    VoidRes serve(Handler handler);
};
```

## Wire Formats

### Stream Framing (TCP, IPC, SHM)

All stream transports use 4-byte big-endian length prefix:

```
[length:4 bytes][payload:N bytes]
```

### RPC Wire Format

RPC messages include request ID for matching:

```
[request_id:4 bytes][length:4 bytes][payload:N bytes]
```

### LoRa Melodi Protocol

Uses serial protocol at 115200 baud:

```
TX <ipv6_addr> <hex_payload>
RX <ipv6_addr> <hex_payload>
```

Messages are fragmented to 128 bytes max.

## Error Handling

All operations return `dp::Res<T>` or `dp::VoidRes`:

```cpp
auto res = stream.connect(endpoint);
if (res.is_err()) {
    echo::error("Error: ", res.error().message.c_str());
    return;
}

// Or unwrap (panics on error)
stream.connect(endpoint).unwrap();
```

Common error types:
- `dp::Error::io_error()` - I/O operation failed
- `dp::Error::timeout()` - Operation timed out
- `dp::Error::invalid_argument()` - Invalid parameter
- `dp::Error::not_found()` - Resource not found

## Logging

Netpipe uses the `echo` library with compile-time filtering:

```cpp
// Set log level at compile time
#define LOGLEVEL Debug
#include <netpipe/netpipe.hpp>

// Log levels (from most to least verbose):
echo::trace()  // Internal details (socket ops, byte counts)
echo::debug()  // State changes (connected, disconnected)
echo::info()   // Significant events (server started)
echo::warn()   // Recoverable issues
echo::error()  // Failures
```

## Examples

The `examples/` directory contains:

- **tcp_echo_server.cpp** - TCP echo server
- **tcp_echo_client.cpp** - TCP echo client
- **udp_broadcast.cpp** - UDP broadcast sender/receiver
- **rpc_example.cpp** - RPC client/server
- **ethernet_tunnel.cpp** - Wirebit Ethernet L2 tunneling over TCP (simulation with ShmLink)
- **tap_tunnel.cpp** - **REAL Linux TAP interfaces** - Creates actual network interfaces visible to OS!

Build examples:

```bash
make build
```

Run examples:

```bash
# TCP echo
./build/linux/x86_64/release/tcp_echo_server
./build/linux/x86_64/release/tcp_echo_client

# UDP broadcast
./build/linux/x86_64/release/udp_broadcast

# RPC
./build/linux/x86_64/release/rpc_example server
./build/linux/x86_64/release/rpc_example client

# Ethernet tunnel (simulation - requires wirebit)
./build/linux/x86_64/release/ethernet_tunnel server
./build/linux/x86_64/release/ethernet_tunnel client

# TAP tunnel (REAL hardware - requires sudo and wirebit)
sudo ./build/linux/x86_64/release/tap_tunnel server
sudo ./build/linux/x86_64/release/tap_tunnel client
# Then try: ping 10.0.0.2, sudo tcpdump -i tap0, ip link show tap0
```

## Building

Netpipe supports multiple build systems:

### XMake (Recommended)

```bash
make config   # Configure build
make build    # Build library and examples
make test     # Run tests
make clean    # Clean build artifacts
```

### CMake

```bash
BUILD_SYSTEM=cmake make config
BUILD_SYSTEM=cmake make build
```

### Zig

```bash
BUILD_SYSTEM=zig make build
```

## Testing

Run tests:

```bash
make test
```

Run specific test:

```bash
make test TEST=test_common
```

## Integration with Wirebit

Netpipe integrates seamlessly with the [wirebit](https://github.com/robolibs/wirebit) library for hardware protocol tunneling.

### Simulation Mode (ShmLink)

Use shared memory for fast, simulated network interfaces:

```cpp
#include <netpipe/netpipe.hpp>
#include <wirebit/wirebit.hpp>

// Create simulated Ethernet link
auto shm_link = wirebit::ShmLink::create("eth0", 1024*1024).value();
wirebit::EthEndpoint eth(std::make_shared<wirebit::ShmLink>(std::move(shm_link)), 
                         config, 1, mac_addr);

// Tunnel frames over TCP
netpipe::TcpStream stream;
stream.connect({"192.168.1.100", 9001});

auto frame = eth.recv_eth().value();
netpipe::Message msg(frame.begin(), frame.end());
stream.send(msg);
```

See `examples/ethernet_tunnel.cpp` for a complete example.

### Real Hardware Mode (TapLink)

Create **actual Linux network interfaces** visible to the entire OS:

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

// Assign IP address
system("sudo ip addr add 10.0.0.1/24 dev tap0");

// Now you can:
// - ping through the interface
// - tcpdump -i tap0
// - ip link show tap0
// - Use it like any network interface!

// Tunnel all traffic over TCP
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
```

See `examples/tap_tunnel.cpp` for a complete example with bidirectional bridging.

**What you can do with TAP tunnel:**
- Create VPNs and network bridges
- Test embedded devices remotely
- Build custom network topologies
- Monitor traffic with tcpdump/wireshark
- Use standard networking tools (ping, nc, ssh, etc.)
- Full OS-level routing and firewall support

## Design Philosophy

### Honest Semantics

Netpipe doesn't hide transport differences behind a unified interface. Different transports behave differently:

- **TCP**: Reliable, ordered, connection-oriented
- **UDP**: Unreliable, unordered, connectionless, size limits
- **IPC**: Like TCP but local-only
- **SHM**: Zero-copy, lock-free, local-only, fixed buffer size
- **LoRa**: Unreliable, long-range, low bandwidth, fragmentation

### Blocking API

All operations block until complete. Users handle async:

```cpp
// Thread per connection
std::thread([&]() {
    auto client = server.accept().unwrap();
    handle_client(client);
}).detach();

// Or use futures
auto future = std::async(std::launch::async, [&]() {
    return stream.recv();
});
```

### No Exceptions

All errors are returned via `dp::Result`:

```cpp
auto res = stream.send(msg);
if (res.is_err()) {
    // Handle error
}
```

## License

See LICENSE file for details.

## Contributing

Contributions welcome! Please follow the existing code style:

- **snake_case** for functions and variables
- **CamelCase** for classes and enums
- **ALL_CAPS** for constants
- Use `dp::` types everywhere
- Use `echo::` for logging
- Return `dp::Res<T>` for all fallible operations
- Header-only, all functions `inline`

## See Also

- [datapod](https://github.com/robolibs/datapod) - Data types and error handling
- [echo](https://github.com/robolibs/echo) - Compile-time logging
- [wirebit](https://github.com/robolibs/wirebit) - Hardware protocol library

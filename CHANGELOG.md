# Changelog

## [0.0.19] - 2026-02-10

### <!-- 0 -->⛰️  Features

- Add pipelining queue model and upgrade path
- Add incremental parsing and chunked trailer support
- Enforce stricter frame semantic validation
- Add HPACK Huffman string decode support
- Extend selector with http2 connection factory
- Implement connection startup and graceful shutdown orchestration
- Add request response framing helpers to connection
- Integrate settings stream flow and shutdown engines
- Add connection session scaffold

### <!-- 2 -->🚜 Refactor

- Rename http11 module and APIs to http1

### Examples

- Migrate http2 example to connection session api

## [0.0.17] - 2026-02-09

### <!-- 0 -->⛰️  Features

- Add unified HTTP protocol selector
- Add ALPN negotiation for http1 and http2
- Implement stream reset and connection shutdown
- Add flow control and priority scheduling
- Implement stream lifecycle and header reassembly
- Add preface and settings state machine
- Implement hpack static and dynamic tables
- Implement core frame types and codec
- Add interop modes and compatibility checks
- Add stateful client/server connection helpers
- Implement content-length and chunked body handling
- Add request and response serialization
- Add request/status line and header parser
- Add stream adapter layer for HTTP protocols
- Scaffold http1/http2 modules and shared HTTP foundation
- Add unified Pipe for transport-agnostic communication

### <!-- 3 -->📚 Documentation

- Add phased plan for http1 and http2 implementation

### <!-- 4 -->⚡ Performance

- Add baseline throughput and memory sanity checks

### <!-- 6 -->🧪 Testing

- Add fuzz regression corpus and malformed input coverage

### <!-- 7 -->⚙️ Miscellaneous Tasks

- Remove development plan document
- Conformance sweep and final protocol stabilization
- Refactor CMake configuration and remove netpipe

### Examples

- Add http1 and http2 usage demos
- Send dp::String over RPC in SHM example
- Use echo logging in AnyStream SHM RPC
- Print payload in AnyStream SHM RPC handler
- Fix AnyStream SHM RPC shutdown
- Add AnyStream SHM RPC example

## [0.0.16] - 2026-01-27

### <!-- 0 -->⛰️  Features

- Add unified Pipe for transport-agnostic communication

### Examples

- Send dp::String over RPC in SHM example
- Use echo logging in AnyStream SHM RPC
- Print payload in AnyStream SHM RPC handler
- Fix AnyStream SHM RPC shutdown
- Add AnyStream SHM RPC example

## [0.0.15] - 2026-01-25

### Netpipe

- Add AnyStream/AnyEndpoint with tests

## [0.0.14] - 2026-01-18

### <!-- 7 -->⚙️ Miscellaneous Tasks

- Refactor CMake project configuration

## [0.0.13] - 2026-01-18

### <!-- 0 -->⛰️  Features

- Integrate Keylock for cryptographic operations

## [0.0.12] - 2026-01-18

### <!-- 7 -->⚙️ Miscellaneous Tasks

- Migrate build system to CMake

## [0.0.11] - 2026-01-18

### <!-- 7 -->⚙️ Miscellaneous Tasks

- Bump keylock version to 0.0.12

## [0.0.10] - 2026-01-18

### <!-- 0 -->⛰️  Features

- Implement HTTP/3 and QUIC connection management
- Refactor Netpipe internal header structure
- Implement basic HTTP/3 protocol
- Add initial QUIC protocol implementation

### <!-- 3 -->📚 Documentation

- Add TLS 1.3 encryption documentation and example
- Add TLS 1.3 encryption documentation and example

## [0.0.9] - 2026-01-17

### <!-- 0 -->⛰️  Features

- Implement TLS 1.3 handshake and record layer

### <!-- 1 -->🐛 Bug Fixes

- Improve certificate verification and cipher suite selection

### <!-- 7 -->⚙️ Miscellaneous Tasks

- Update dependencies and development environment

## [0.0.8] - 2026-01-15

### <!-- 0 -->⛰️  Features

- Add IPv6 support and receive timeouts

## [0.0.7] - 2026-01-09

### <!-- 1 -->🐛 Bug Fixes

- Handle bidirection remote handler cancellation correctly

## [0.0.6] - 2026-01-09

### <!-- 0 -->⛰️  Features

- Improve ShmStream message framing and robustness
- Add SHM RPC support in netpipe library

### <!-- 1 -->🐛 Bug Fixes

- Improve IPC test stability and logic
- Refactor ShmStream for bulk shared memory transfer
- Refactor ShmStream for TCP-like accept/connect semantics
- Adjust SHM/IPC/TCP payload and buffer sizes
- Resolve all bidirectional RPC deadlocks and test hangs
- Resolve TCP bidirectional RPC deadlock and test hangs

### <!-- 7 -->⚙️ Miscellaneous Tasks

- Enable optional big transfer tests
- Update echo library version to 0.0.23

## [0.0.5] - 2026-01-07

### <!-- 0 -->⛰️  Features

- Add streaming RPC support (client, server, bidirectional)
- Add request cancellation support
- Add metrics and observability to Remote
- Implement bidirectional Remote for peer-to-peer RPC
- Add serialization helpers for type-safe Remote calls
- Support concurrent requests with request tracking
- Add method/service routing system
- Improve wire protocol with versioning and extensibility
- Improve error handling with Res<Message> handler returns

### <!-- 1 -->🐛 Bug Fixes

- Implement actual timeout functionality in Remote::call()

### <!-- 2 -->🚜 Refactor

- Rename RPC to Remote and create remote/ folder structure

### <!-- 3 -->📚 Documentation

- Fix README to include ALL netpipe features
- Update documentation and examples for Remote system

### <!-- 6 -->🧪 Testing

- Add comprehensive Remote test suite

## [0.0.4] - 2026-01-06

### <!-- 0 -->⛰️  Features

- Use dp::RingBuffer<> intead of custom one

### <!-- 1 -->🐛 Bug Fixes

- Ethernet_tunnel now works with proper architecture

### <!-- 7 -->⚙️ Miscellaneous Tasks

- Add internet check to Makefile
- Remove devbox.lock for cleanup

## [0.0.2] - 2026-01-05

### <!-- 0 -->⛰️  Features

- Add type-tagged and pose tunnel examples
- Add tap_tunnel example using REAL Linux TAP interfaces
- Add ethernet_tunnel example with wirebit integration
- Introduce core NetPipe library and examples
- Initialize project with build systems and configurations

### <!-- 3 -->📚 Documentation

- Update README with tap_tunnel example and real hardware usage
- Add comprehensive README documentation

### Build

- Update wirebit dependency to 0.0.9 for TAP support


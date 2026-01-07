# Changelog

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


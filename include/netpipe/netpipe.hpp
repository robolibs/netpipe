#pragma once

// Netpipe - Minimal peer-to-peer transport library
// Two transport families: Stream (reliable, ordered, connection-oriented)
//                         Datagram (unreliable, connectionless)

// Core types and utilities
#include <netpipe/core/common.hpp>
#include <netpipe/core/endpoint.hpp>

// Base classes
#include <netpipe/core/datagram.hpp>
#include <netpipe/core/stream.hpp>

// Stream implementations
#include <netpipe/transport/stream/ipc.hpp>
#include <netpipe/transport/stream/shm.hpp>
#include <netpipe/transport/stream/tcp.hpp>

// Unified stream wrapper
#include <netpipe/transport/any.hpp>

// High-level pipe (combines endpoint + stream)
#include <netpipe/transport/pipe.hpp>

// QUIC transport (UDP-based, multiplexed streams)
#include <netpipe/transport/stream/quic.hpp>

// Datagram implementations
#include <netpipe/transport/datagram/lora.hpp>
#include <netpipe/transport/datagram/udp.hpp>

// Higher-level protocols
#include <netpipe/protocol/rpc.hpp>
#include <netpipe/protocol/rpc/async.hpp>
#include <netpipe/protocol/rpc/metrics.hpp>
#include <netpipe/protocol/rpc/serialization.hpp>
#include <netpipe/protocol/rpc/streaming.hpp>

// All types are in the netpipe:: namespace
// Available types:
//   - netpipe::Message (dp::Vector<dp::u8>)
//   - netpipe::TcpEndpoint, UdpEndpoint, IpcEndpoint, ShmEndpoint, LoraEndpoint
//   - netpipe::Stream (base class)
//   - netpipe::TcpStream, IpcStream, ShmStream
//   - netpipe::AnyEndpoint, AnyStream - type-erased wrappers
//   - netpipe::Pipe - high-level pipe combining endpoint + stream
//   - netpipe::QuicEndpoint, QuicStream - QUIC transport over UDP
//   - netpipe::Datagram (base class)
//   - netpipe::UdpDatagram, LoraDatagram
//   - netpipe::Remote<Unidirect> - Simple client-server RPC
//   - netpipe::Remote<Bidirect> - Bidirectional peer-to-peer RPC with concurrency

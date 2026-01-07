#pragma once

// Netpipe - Minimal peer-to-peer transport library
// Two transport families: Stream (reliable, ordered, connection-oriented)
//                         Datagram (unreliable, connectionless)

// Core types and utilities
#include <netpipe/common.hpp>
#include <netpipe/endpoint.hpp>

// Base classes
#include <netpipe/datagram.hpp>
#include <netpipe/stream.hpp>

// Stream implementations
#include <netpipe/stream/ipc.hpp>
#include <netpipe/stream/shm.hpp>
#include <netpipe/stream/tcp.hpp>

// Datagram implementations
#include <netpipe/datagram/lora.hpp>
#include <netpipe/datagram/udp.hpp>

// Higher-level protocols
#include <netpipe/remote/remote.hpp>
#include <netpipe/rpc.hpp> // Deprecated: use netpipe/remote/remote.hpp

// All types are in the netpipe:: namespace
// Available types:
//   - netpipe::Message (dp::Vector<dp::u8>)
//   - netpipe::TcpEndpoint, UdpEndpoint, IpcEndpoint, ShmEndpoint, LoraEndpoint
//   - netpipe::Stream (base class)
//   - netpipe::TcpStream, IpcStream, ShmStream
//   - netpipe::Datagram (base class)
//   - netpipe::UdpDatagram, LoraDatagram
//   - netpipe::Remote (new name for RPC)
//   - netpipe::Rpc (deprecated: use Remote)

#pragma once

/// QUIC (RFC 9000) transport for netpipe
///
/// QUIC provides encrypted, multiplexed streams over UDP with built-in TLS 1.3.
///
/// Basic usage (TCP-like API):
/// @code
/// // Client
/// netpipe::quic::QuicStream quic;
/// quic.connect({"127.0.0.1", 4433});
/// quic.send(message);
/// auto response = quic.recv();
///
/// // Server
/// netpipe::quic::QuicStream server;
/// server.listen({"0.0.0.0", 4433});
/// auto client = server.accept();
/// auto msg = client->recv();
/// client->send(response);
/// @endcode
///
/// Stream multiplexing:
/// @code
/// auto stream2 = quic.open_stream();
/// stream2->send(other_message);
/// @endcode

// Core types
#include <netpipe/quic/types.hpp>
#include <netpipe/quic/varint.hpp>

// Protocol components
#include <netpipe/quic/crypto.hpp>
#include <netpipe/quic/frame.hpp>
#include <netpipe/quic/packet.hpp>
#include <netpipe/quic/transport_params.hpp>

// Stream management
#include <netpipe/quic/stream.hpp>

// Connection management
#include <netpipe/quic/connection.hpp>

// Reliability and congestion
#include <netpipe/quic/ack_manager.hpp>
#include <netpipe/quic/congestion_control.hpp>
#include <netpipe/quic/flow_control.hpp>
#include <netpipe/quic/loss_detection.hpp>

// TLS integration
#include <netpipe/quic/tls_adapter.hpp>

// Main API (Stream interface)
#include <netpipe/quic/endpoint.hpp>

// Re-export main types into netpipe namespace for convenience
namespace netpipe {
    using QuicStream = quic::QuicStream;
    using QuicStreamHandle = quic::QuicStreamHandle;
    using QuicConfig = quic::QuicConfig;
} // namespace netpipe

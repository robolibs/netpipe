#pragma once

/// HTTP/1.1 protocol module (scaffold)
///
/// Current scope:
/// - Request/status line parsing and serialization
/// - Header parsing with strict and lenient modes
/// - Content-Length and chunked body support
/// - Connection keep-alive behavior modeling
///
/// Currently unsupported:
/// - Trailer section parsing and emission for chunked transfer
/// - Upgrade handshake state machine
/// - Pipelining queue management

#include <netpipe/protocol/http/common.hpp>
#include <netpipe/protocol/http/transport_adapter.hpp>
#include <netpipe/protocol/http1/body.hpp>
#include <netpipe/protocol/http1/connection.hpp>
#include <netpipe/protocol/http1/incremental.hpp>
#include <netpipe/protocol/http1/parser.hpp>
#include <netpipe/protocol/http1/pipelining.hpp>
#include <netpipe/protocol/http1/serialize.hpp>
#include <netpipe/protocol/http1/types.hpp>

namespace netpipe {
    using Http1Request = http1::Request;
    using Http1Response = http1::Response;
} // namespace netpipe

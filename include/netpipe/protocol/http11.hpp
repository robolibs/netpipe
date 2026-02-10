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
#include <netpipe/protocol/http11/body.hpp>
#include <netpipe/protocol/http11/connection.hpp>
#include <netpipe/protocol/http11/incremental.hpp>
#include <netpipe/protocol/http11/parser.hpp>
#include <netpipe/protocol/http11/serialize.hpp>
#include <netpipe/protocol/http11/types.hpp>

namespace netpipe {
    using Http11Request = http11::Request;
    using Http11Response = http11::Response;
} // namespace netpipe

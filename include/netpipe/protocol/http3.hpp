#pragma once

/// HTTP/3 (RFC 9114) support for netpipe
///
/// HTTP/3 runs over QUIC and provides efficient HTTP semantics with
/// built-in multiplexing, header compression (QPACK), and encryption.
///
/// Basic usage:
/// @code
/// // Create HTTP/3 connection (client)
/// netpipe::http3::Connection conn(true);
/// auto init_data = conn.initialize().value();
/// // Send init_data on control stream
///
/// // Create request
/// auto stream_id = conn.create_request_stream().value();
/// netpipe::http3::Request req;
/// req.method = "GET";
/// req.scheme = "https";
/// req.authority = "example.com";
/// req.path = "/api/data";
///
/// auto headers_data = conn.encode_request(stream_id, req).value();
/// // Send headers_data on stream_id
///
/// // Process response
/// conn.process_request_stream(stream_id, response_data);
/// auto response = conn.get_response(stream_id);
/// @endcode

#include <netpipe/protocol/http3/connection.hpp>
#include <netpipe/protocol/http3/frame.hpp>
#include <netpipe/protocol/http3/qpack.hpp>
#include <netpipe/protocol/http3/types.hpp>

// Re-export main types
namespace netpipe {
    using Http3Request = http3::Request;
    using Http3Response = http3::Response;
    using Http3Settings = http3::Settings;
    using Http3Connection = http3::Connection;
} // namespace netpipe

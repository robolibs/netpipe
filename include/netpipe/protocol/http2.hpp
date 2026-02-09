#pragma once

/// HTTP/2 protocol module (scaffold)

#include <netpipe/protocol/http/common.hpp>
#include <netpipe/protocol/http2/types.hpp>

namespace netpipe {
    using Http2Request = http2::Request;
    using Http2Response = http2::Response;
} // namespace netpipe

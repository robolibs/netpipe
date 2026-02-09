#pragma once

/// HTTP/1.1 protocol module (scaffold)

#include <netpipe/protocol/http/common.hpp>
#include <netpipe/protocol/http/transport_adapter.hpp>
#include <netpipe/protocol/http11/body.hpp>
#include <netpipe/protocol/http11/parser.hpp>
#include <netpipe/protocol/http11/serialize.hpp>
#include <netpipe/protocol/http11/types.hpp>

namespace netpipe {
    using Http11Request = http11::Request;
    using Http11Response = http11::Response;
} // namespace netpipe

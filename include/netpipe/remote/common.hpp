#pragma once

#include <functional>
#include <netpipe/common.hpp>

namespace netpipe {
    namespace remote {

        /// Handler function type for Remote RPC
        /// Takes a request message and returns a response message
        using Handler = std::function<Message(const Message &)>;

    } // namespace remote
} // namespace netpipe

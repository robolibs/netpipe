#pragma once

#include <functional>
#include <netpipe/common.hpp>

namespace netpipe {
    namespace remote {

        /// Handler function type for Remote RPC
        /// Takes a request message and returns a response message or error
        /// If the handler returns an error, it will be sent back to the client as an error response
        using Handler = std::function<dp::Res<Message>(const Message &)>;

        /// Legacy handler type for backward compatibility
        /// Takes a request message and returns a response message (cannot return errors)
        using LegacyHandler = std::function<Message(const Message &)>;

    } // namespace remote
} // namespace netpipe

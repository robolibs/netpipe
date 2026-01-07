#pragma once

#include <memory>
#include <netpipe/endpoint.hpp>

namespace netpipe {

    // Abstract base class for all stream-oriented (connection-based) transports
    // Streams are reliable, ordered, connection-oriented byte pipes
    class Stream {
      public:
        virtual ~Stream() = default;

        // Connection establishment - client side
        // Blocks until connected or fails
        virtual dp::Res<void> connect(const TcpEndpoint &endpoint) = 0;

        // Connection establishment - server side
        // Binds to local endpoint and starts listening
        virtual dp::Res<void> listen(const TcpEndpoint &endpoint) = 0;

        // Accept incoming connection - server side
        // Blocks until a client connects
        // Returns a new Stream instance for the client connection
        // The original stream continues listening
        virtual dp::Res<std::unique_ptr<Stream>> accept() = 0;

        // Send a message over the connection
        // Blocks until the entire message is handed to the OS
        // Message is framed with length prefix internally
        virtual dp::Res<void> send(const Message &msg) = 0;

        // Receive a message from the connection
        // Blocks until a complete message arrives
        // Returns the message payload (without framing)
        virtual dp::Res<Message> recv() = 0;

        // Set receive timeout in milliseconds
        // 0 means no timeout (blocking forever)
        // Returns error if timeout cannot be set
        virtual dp::Res<void> set_recv_timeout(dp::u32 timeout_ms) = 0;

        // Close the connection and release resources
        virtual void close() = 0;

        // Check if the connection is active
        virtual bool is_connected() const = 0;
    };

} // namespace netpipe

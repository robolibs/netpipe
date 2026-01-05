#pragma once

#include <netpipe/endpoint.hpp>

namespace netpipe {

    // Abstract base class for all datagram-oriented (connectionless) transports
    // Datagrams are unreliable, unordered, connectionless messages
    class Datagram {
      public:
        virtual ~Datagram() = default;

        // Bind to local address for receiving
        // Must be called before recv_from()
        virtual dp::Res<void> bind(const UdpEndpoint &endpoint) = 0;

        // Send a message to a specific destination
        // Fire and forget - may or may not arrive
        // No framing needed - message boundaries preserved by transport
        virtual dp::Res<void> send_to(const Message &msg, const UdpEndpoint &dest) = 0;

        // Broadcast a message to all reachable destinations
        // UDP: broadcasts to subnet
        // LoRa: broadcasts to mesh
        virtual dp::Res<void> broadcast(const Message &msg) = 0;

        // Receive a message
        // Blocks until a message arrives
        // Returns the message and the source endpoint
        virtual dp::Res<dp::Pair<Message, UdpEndpoint>> recv_from() = 0;

        // Close and release resources
        virtual void close() = 0;
    };

} // namespace netpipe

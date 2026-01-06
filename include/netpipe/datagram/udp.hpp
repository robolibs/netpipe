#pragma once

#include <netpipe/datagram.hpp>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace netpipe {

    // UDP datagram implementation using BSD sockets
    // Unreliable, unordered, connectionless transport
    // Message boundaries preserved - no framing needed
    class UdpDatagram : public Datagram {
      private:
        dp::i32 fd_;
        bool bound_;
        UdpEndpoint local_endpoint_;

        static constexpr dp::usize MAX_UDP_SIZE = 1400; // Safe size to avoid fragmentation

      public:
        UdpDatagram() : fd_(-1), bound_(false) { echo::trace("UdpDatagram constructed"); }

        ~UdpDatagram() override {
            if (fd_ >= 0) {
                close();
            }
        }

        // Bind to local address for receiving
        dp::Res<void> bind(const UdpEndpoint &endpoint) override {
            echo::trace("binding to ", endpoint.to_string());

            // Create socket
            fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
            if (fd_ < 0) {
                echo::error("socket creation failed: ", strerror(errno));
                return dp::result::err(dp::Error::io_error("io error"));
            }
            echo::trace("udp socket created fd=", fd_);

            // Set SO_REUSEADDR
            dp::i32 opt = 1;
            if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
                echo::warn("setsockopt SO_REUSEADDR failed: ", strerror(errno));
            }

            // Bind to address
            struct sockaddr_in addr = {};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(endpoint.port);

            if (endpoint.host == "0.0.0.0" || endpoint.host.empty()) {
                addr.sin_addr.s_addr = INADDR_ANY;
            } else {
                if (::inet_pton(AF_INET, endpoint.host.c_str(), &addr.sin_addr) <= 0) {
                    ::close(fd_);
                    fd_ = -1;
                    echo::error("invalid address: ", endpoint.host.c_str());
                    return dp::result::err(dp::Error::invalid_argument("invalid argument"));
                }
            }

            if (::bind(fd_, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
                ::close(fd_);
                fd_ = -1;
                echo::error("bind failed: ", strerror(errno));
                return dp::result::err(dp::Error::io_error("io error"));
            }

            bound_ = true;
            local_endpoint_ = endpoint;
            echo::debug("UdpDatagram bound to port ", endpoint.port);
            echo::info("UdpDatagram listening on ", endpoint.to_string());

            return dp::result::ok();
        }

        // Send a message to a specific destination
        dp::Res<void> send_to(const Message &msg, const UdpEndpoint &dest) override {
            if (fd_ < 0) {
                // Create socket if not already created
                fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
                if (fd_ < 0) {
                    echo::error("socket creation failed: ", strerror(errno));
                    return dp::result::err(dp::Error::io_error("io error"));
                }
                echo::trace("udp socket created fd=", fd_);
            }

            // Check message size
            if (msg.size() > MAX_UDP_SIZE) {
                echo::warn("message too large: ", msg.size(), " > ", MAX_UDP_SIZE);
                return dp::result::err(dp::Error::invalid_argument(dp::String("message too large: ") +
                                                                   std::to_string(msg.size()).c_str()));
            }

            echo::trace("sendto ", dest.to_string(), " len=", msg.size());

            // Resolve hostname
            struct addrinfo hints = {};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;

            struct addrinfo *result = nullptr;
            dp::String port_str(std::to_string(dest.port).c_str());
            dp::i32 ret = ::getaddrinfo(dest.host.c_str(), port_str.c_str(), &hints, &result);
            if (ret != 0) {
                echo::error("getaddrinfo failed: ", gai_strerror(ret));
                return dp::result::err(dp::Error::io_error("io error"));
            }

            // Send
            dp::isize n = ::sendto(fd_, msg.data(), msg.size(), 0, result->ai_addr, result->ai_addrlen);
            ::freeaddrinfo(result);

            if (n < 0) {
                echo::error("sendto failed: ", strerror(errno));
                return dp::result::err(dp::Error::io_error("io error"));
            }

            echo::debug("sent ", n, " bytes to ", dest.to_string());
            return dp::result::ok();
        }

        // Broadcast a message
        dp::Res<void> broadcast(const Message &msg) override {
            if (fd_ < 0) {
                // Create socket if not already created
                fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
                if (fd_ < 0) {
                    echo::error("socket creation failed: ", strerror(errno));
                    return dp::result::err(dp::Error::io_error("io error"));
                }
                echo::trace("udp socket created fd=", fd_);
            }

            // Enable broadcast
            dp::i32 broadcast_enable = 1;
            if (::setsockopt(fd_, SOL_SOCKET, SO_BROADCAST, &broadcast_enable, sizeof(broadcast_enable)) < 0) {
                echo::error("setsockopt SO_BROADCAST failed: ", strerror(errno));
                return dp::result::err(dp::Error::io_error("io error"));
            }
            echo::debug("broadcast enabled");

            // Check message size
            if (msg.size() > MAX_UDP_SIZE) {
                echo::warn("message too large: ", msg.size(), " > ", MAX_UDP_SIZE);
                return dp::result::err(dp::Error::invalid_argument(dp::String("message too large: ") +
                                                                   std::to_string(msg.size()).c_str()));
            }

            echo::trace("broadcasting len=", msg.size());

            // Broadcast to 255.255.255.255
            struct sockaddr_in addr = {};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(bound_ ? local_endpoint_.port : 7447); // Use bound port or default
            addr.sin_addr.s_addr = INADDR_BROADCAST;

            dp::isize n = ::sendto(fd_, msg.data(), msg.size(), 0, (struct sockaddr *)&addr, sizeof(addr));
            if (n < 0) {
                echo::error("broadcast sendto failed: ", strerror(errno));
                return dp::result::err(dp::Error::io_error("io error"));
            }

            echo::debug("broadcast ", n, " bytes");
            return dp::result::ok();
        }

        // Receive a message
        dp::Res<dp::Pair<Message, UdpEndpoint>> recv_from() override {
            if (!bound_) {
                echo::error("recv_from called but not bound");
                return dp::result::err(dp::Error::invalid_argument("not bound"));
            }

            echo::trace("recv_from waiting for message");

            // Receive
            Message msg(MAX_UDP_SIZE);
            struct sockaddr_in src_addr = {};
            socklen_t src_len = sizeof(src_addr);

            dp::isize n = ::recvfrom(fd_, msg.data(), msg.size(), 0, (struct sockaddr *)&src_addr, &src_len);
            if (n < 0) {
                echo::error("recvfrom failed: ", strerror(errno));
                return dp::result::err(dp::Error::io_error("io error"));
            }

            // Resize message to actual received size
            msg.resize(static_cast<dp::usize>(n));

            // Get source address
            char src_ip[INET_ADDRSTRLEN];
            ::inet_ntop(AF_INET, &src_addr.sin_addr, src_ip, sizeof(src_ip));
            dp::u16 src_port = ntohs(src_addr.sin_port);

            UdpEndpoint src_endpoint{dp::String(src_ip), src_port};

            echo::trace("recvfrom got ", n, " bytes from ", src_endpoint.to_string());
            echo::debug("received ", n, " bytes from ", src_endpoint.to_string());

            return dp::result::ok(dp::Pair<Message, UdpEndpoint>(std::move(msg), src_endpoint));
        }

        // Close the socket
        void close() override {
            if (fd_ >= 0) {
                echo::trace("closing fd=", fd_);
                ::close(fd_);
                fd_ = -1;
                bound_ = false;
                echo::debug("UdpDatagram closed");
            }
        }
    };

} // namespace netpipe

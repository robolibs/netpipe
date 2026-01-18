#pragma once

#include <netpipe/core/datagram.hpp>

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

            // Determine address family based on endpoint
            bool is_ipv6 = (endpoint.host.find(':') != dp::String::npos) || endpoint.host == "::";
            bool is_any = endpoint.host == "0.0.0.0" || endpoint.host == "::" || endpoint.host.empty();
            dp::i32 family = is_ipv6 ? AF_INET6 : AF_INET;

            // Create socket
            fd_ = ::socket(family, SOCK_DGRAM, 0);
            if (fd_ < 0) {
                echo::error("socket creation failed: ", strerror(errno));
                return dp::result::err(dp::Error::io_error("io error"));
            }
            echo::trace("udp socket created fd=", fd_, " family=", is_ipv6 ? "IPv6" : "IPv4");

            // Set SO_REUSEADDR
            dp::i32 opt = 1;
            if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
                echo::warn("setsockopt SO_REUSEADDR failed: ", strerror(errno));
            }

            // Bind to address (IPv4 or IPv6)
            struct sockaddr_storage addr_storage = {};
            socklen_t addr_len;

            if (is_ipv6) {
                auto *addr6 = reinterpret_cast<struct sockaddr_in6 *>(&addr_storage);
                addr6->sin6_family = AF_INET6;
                addr6->sin6_port = htons(endpoint.port);
                addr_len = sizeof(struct sockaddr_in6);

                if (is_any) {
                    addr6->sin6_addr = in6addr_any;
                } else {
                    if (::inet_pton(AF_INET6, endpoint.host.c_str(), &addr6->sin6_addr) <= 0) {
                        ::close(fd_);
                        fd_ = -1;
                        echo::error("invalid IPv6 address: ", endpoint.host.c_str());
                        return dp::result::err(dp::Error::invalid_argument("invalid argument"));
                    }
                }
            } else {
                auto *addr4 = reinterpret_cast<struct sockaddr_in *>(&addr_storage);
                addr4->sin_family = AF_INET;
                addr4->sin_port = htons(endpoint.port);
                addr_len = sizeof(struct sockaddr_in);

                if (is_any) {
                    addr4->sin_addr.s_addr = INADDR_ANY;
                } else {
                    if (::inet_pton(AF_INET, endpoint.host.c_str(), &addr4->sin_addr) <= 0) {
                        ::close(fd_);
                        fd_ = -1;
                        echo::error("invalid IPv4 address: ", endpoint.host.c_str());
                        return dp::result::err(dp::Error::invalid_argument("invalid argument"));
                    }
                }
            }

            if (::bind(fd_, reinterpret_cast<struct sockaddr *>(&addr_storage), addr_len) < 0) {
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
            // Check message size
            if (msg.size() > MAX_UDP_SIZE) {
                echo::warn("message too large: ", msg.size(), " > ", MAX_UDP_SIZE);
                return dp::result::err(dp::Error::invalid_argument(dp::String("message too large: ") +
                                                                   std::to_string(msg.size()).c_str()));
            }

            echo::trace("sendto ", dest.to_string(), " len=", msg.size());

            // Resolve hostname (supports both IPv4 and IPv6)
            struct addrinfo hints = {};
            hints.ai_family = AF_UNSPEC; // Allow IPv4 or IPv6
            hints.ai_socktype = SOCK_DGRAM;

            struct addrinfo *result = nullptr;
            dp::String port_str(std::to_string(dest.port).c_str());
            dp::i32 ret = ::getaddrinfo(dest.host.c_str(), port_str.c_str(), &hints, &result);
            if (ret != 0) {
                echo::error("getaddrinfo failed: ", gai_strerror(ret));
                return dp::result::err(dp::Error::io_error("io error"));
            }

            // Create socket if not already created, or if family doesn't match
            if (fd_ < 0) {
                fd_ = ::socket(result->ai_family, SOCK_DGRAM, 0);
                if (fd_ < 0) {
                    ::freeaddrinfo(result);
                    echo::error("socket creation failed: ", strerror(errno));
                    return dp::result::err(dp::Error::io_error("io error"));
                }
                echo::trace("udp socket created fd=", fd_, " family=", result->ai_family == AF_INET6 ? "IPv6" : "IPv4");
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

            // Receive (use sockaddr_storage to handle both IPv4 and IPv6)
            Message msg(MAX_UDP_SIZE);
            struct sockaddr_storage src_addr = {};
            socklen_t src_len = sizeof(src_addr);

            dp::isize n =
                ::recvfrom(fd_, msg.data(), msg.size(), 0, reinterpret_cast<struct sockaddr *>(&src_addr), &src_len);
            if (n < 0) {
                echo::error("recvfrom failed: ", strerror(errno));
                return dp::result::err(dp::Error::io_error("io error"));
            }

            // Resize message to actual received size
            msg.resize(static_cast<dp::usize>(n));

            // Get source address (IPv4 or IPv6)
            char src_ip[INET6_ADDRSTRLEN];
            dp::u16 src_port;

            if (src_addr.ss_family == AF_INET6) {
                auto *addr6 = reinterpret_cast<struct sockaddr_in6 *>(&src_addr);
                ::inet_ntop(AF_INET6, &addr6->sin6_addr, src_ip, sizeof(src_ip));
                src_port = ntohs(addr6->sin6_port);
            } else {
                auto *addr4 = reinterpret_cast<struct sockaddr_in *>(&src_addr);
                ::inet_ntop(AF_INET, &addr4->sin_addr, src_ip, sizeof(src_ip));
                src_port = ntohs(addr4->sin_port);
            }

            UdpEndpoint src_endpoint{dp::String(src_ip), src_port};

            echo::trace("recvfrom got ", n, " bytes from ", src_endpoint.to_string());
            echo::debug("received ", n, " bytes from ", src_endpoint.to_string());

            return dp::result::ok(dp::Pair<Message, UdpEndpoint>(std::move(msg), src_endpoint));
        }

        // Set receive timeout in milliseconds
        dp::Res<void> set_recv_timeout(dp::u32 timeout_ms) override {
            if (fd_ < 0) {
                echo::error("set_recv_timeout called but socket not created");
                return dp::result::err(dp::Error::invalid_argument("socket not created"));
            }

            struct timeval tv;
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;

            if (::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
                echo::error("setsockopt SO_RCVTIMEO failed: ", strerror(errno));
                return dp::result::err(
                    dp::Error::io_error(dp::String("failed to set recv timeout: ") + strerror(errno)));
            }

            echo::trace("set recv timeout to ", timeout_ms, "ms");
            return dp::result::ok();
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

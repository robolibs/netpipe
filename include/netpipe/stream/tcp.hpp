#pragma once

#include <netpipe/stream.hpp>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace netpipe {

    // TCP stream implementation using BSD sockets
    // Reliable, ordered, connection-oriented transport
    class TcpStream : public Stream {
      private:
        dp::i32 fd_;
        bool connected_;
        bool listening_;
        TcpEndpoint local_endpoint_;
        TcpEndpoint remote_endpoint_;

        // Private constructor for accepted connections
        TcpStream(dp::i32 fd, const TcpEndpoint &local, const TcpEndpoint &remote)
            : fd_(fd), connected_(true), listening_(false), local_endpoint_(local), remote_endpoint_(remote) {
            echo::debug("TcpStream created from accepted connection fd=", fd);
        }

      public:
        TcpStream() : fd_(-1), connected_(false), listening_(false) { echo::trace("TcpStream constructed"); }

        ~TcpStream() override {
            if (fd_ >= 0) {
                close();
            }
        }

        // Client side: connect to remote endpoint
        dp::Res<void> connect(const TcpEndpoint &endpoint) override {
            echo::trace("connecting to ", endpoint.to_string());

            // Create socket
            fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
            if (fd_ < 0) {
                echo::error("socket creation failed: ", strerror(errno));
                return dp::result::err(dp::Error::io_error("io error"));
            }
            echo::trace("socket created fd=", fd_);

            // Resolve hostname
            struct addrinfo hints = {};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;

            struct addrinfo *result = nullptr;
            dp::String port_str(std::to_string(endpoint.port).c_str());
            dp::i32 ret = ::getaddrinfo(endpoint.host.c_str(), port_str.c_str(), &hints, &result);
            if (ret != 0) {
                ::close(fd_);
                fd_ = -1;
                echo::error("getaddrinfo failed: ", gai_strerror(ret));
                return dp::result::err(dp::Error::io_error("getaddrinfo failed"));
            }

            // Try to connect
            ret = ::connect(fd_, result->ai_addr, result->ai_addrlen);
            ::freeaddrinfo(result);

            if (ret < 0) {
                ::close(fd_);
                fd_ = -1;
                echo::error("connect failed: ", strerror(errno));
                return dp::result::err(dp::Error::io_error("connect failed"));
            }

            connected_ = true;
            remote_endpoint_ = endpoint;
            echo::debug("connected to ", endpoint.to_string());
            echo::info("TcpStream connected to ", endpoint.to_string());

            return dp::result::ok();
        }

        // Server side: bind and listen
        dp::Res<void> listen(const TcpEndpoint &endpoint) override {
            echo::trace("listening on ", endpoint.to_string());

            // Create socket
            fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
            if (fd_ < 0) {
                echo::error("socket creation failed: ", strerror(errno));
                return dp::result::err(dp::Error::io_error("io error"));
            }
            echo::trace("socket created fd=", fd_);

            // Set SO_REUSEADDR to avoid "address already in use" errors
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

            // Start listening
            if (::listen(fd_, SOMAXCONN) < 0) {
                ::close(fd_);
                fd_ = -1;
                echo::error("listen failed: ", strerror(errno));
                return dp::result::err(dp::Error::io_error("io error"));
            }

            listening_ = true;
            local_endpoint_ = endpoint;
            echo::debug("listening on ", endpoint.to_string());
            echo::info("TcpStream listening on ", endpoint.to_string());

            return dp::result::ok();
        }

        // Server side: accept incoming connection
        dp::Res<std::unique_ptr<Stream>> accept() override {
            if (!listening_) {
                echo::error("accept called but not listening");
                return dp::result::err(dp::Error::invalid_argument("not listening"));
            }

            echo::trace("waiting for connection on fd=", fd_);

            struct sockaddr_in client_addr = {};
            socklen_t client_len = sizeof(client_addr);

            dp::i32 client_fd = ::accept(fd_, (struct sockaddr *)&client_addr, &client_len);
            if (client_fd < 0) {
                echo::error("accept failed: ", strerror(errno));
                return dp::result::err(dp::Error::io_error("io error"));
            }

            // Get client address
            char client_ip[INET_ADDRSTRLEN];
            ::inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
            dp::u16 client_port = ntohs(client_addr.sin_port);

            TcpEndpoint client_endpoint{dp::String(client_ip), client_port};

            echo::debug("accepted connection from ", client_endpoint.to_string(), " fd=", client_fd);
            echo::info("TcpStream accepted connection from ", client_endpoint.to_string());

            // Create new TcpStream for the client
            auto client_stream = std::unique_ptr<Stream>(new TcpStream(client_fd, local_endpoint_, client_endpoint));

            return dp::result::ok(std::move(client_stream));
        }

        // Send a message with length-prefix framing
        dp::Res<void> send(const Message &msg) override {
            if (!connected_) {
                echo::error("send called but not connected");
                return dp::result::err(dp::Error::not_found("not connected"));
            }

            echo::trace("send ", msg.size(), " bytes");

            // Encode length prefix (4 bytes big-endian)
            auto length_bytes = encode_u32_be(static_cast<dp::u32>(msg.size()));

            // Send length prefix
            auto res = write_exact(fd_, length_bytes.data(), 4);
            if (res.is_err()) {
                connected_ = false;
                echo::error("send length failed");
                return res;
            }

            // Send payload
            if (!msg.empty()) {
                res = write_exact(fd_, msg.data(), msg.size());
                if (res.is_err()) {
                    connected_ = false;
                    echo::error("send payload failed");
                    return res;
                }
            }

            echo::debug("sent ", msg.size(), " bytes");
            return dp::result::ok();
        }

        // Receive a message with length-prefix framing
        dp::Res<Message> recv() override {
            if (!connected_) {
                echo::error("recv called but not connected");
                return dp::result::err(dp::Error::not_found("not connected"));
            }

            echo::trace("recv waiting for message");

            // Read length prefix (4 bytes)
            dp::Array<dp::u8, 4> length_bytes;
            auto res = read_exact(fd_, length_bytes.data(), 4);
            if (res.is_err()) {
                connected_ = false;
                echo::error("recv length failed");
                return dp::result::err(res.error());
            }

            dp::u32 length = decode_u32_be(length_bytes.data());
            echo::trace("recv expecting ", length, " bytes");

            // Read payload
            Message msg(length);
            if (length > 0) {
                res = read_exact(fd_, msg.data(), length);
                if (res.is_err()) {
                    connected_ = false;
                    echo::error("recv payload failed");
                    return dp::result::err(res.error());
                }
            }

            echo::debug("received ", length, " bytes");
            return dp::result::ok(std::move(msg));
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
                return dp::result::err(dp::Error::io_error("failed to set timeout"));
            }

            echo::trace("set recv timeout to ", timeout_ms, "ms");
            return dp::result::ok();
        }

        // Close the connection
        void close() override {
            if (fd_ >= 0) {
                echo::trace("closing fd=", fd_);
                ::close(fd_);
                fd_ = -1;
                connected_ = false;
                listening_ = false;
                echo::debug("TcpStream closed");
            }
        }

        // Check if connected
        bool is_connected() const override { return connected_; }
    };

} // namespace netpipe

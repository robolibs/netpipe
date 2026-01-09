#pragma once

#include <netpipe/remote/protocol.hpp>
#include <netpipe/stream.hpp>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace netpipe {

    // IPC stream implementation using Unix domain sockets
    // Same semantics as TCP but local-only with filesystem paths
    class IpcStream : public Stream {
      private:
        dp::i32 fd_;
        bool connected_;
        bool listening_;
        IpcEndpoint local_endpoint_;
        IpcEndpoint remote_endpoint_;
        bool should_unlink_; // Track if we should unlink the socket file on close

        // Private constructor for accepted connections
        IpcStream(dp::i32 fd, const IpcEndpoint &local, const IpcEndpoint &remote)
            : fd_(fd), connected_(true), listening_(false), local_endpoint_(local), remote_endpoint_(remote),
              should_unlink_(false) {
            echo::debug("IpcStream created from accepted connection fd=", fd);
        }

      public:
        IpcStream() : fd_(-1), connected_(false), listening_(false), should_unlink_(false) {
            echo::trace("IpcStream constructed");
        }

        ~IpcStream() override {
            if (fd_ >= 0) {
                close();
            }
        }

        // Client side: connect to Unix domain socket
        dp::Res<void> connect(const TcpEndpoint &endpoint) override {
            // IpcStream uses IpcEndpoint, but interface requires TcpEndpoint
            // This is a limitation of the base class design
            // For now, treat endpoint.host as the path
            IpcEndpoint ipc_endpoint{endpoint.host};
            return connect_ipc(ipc_endpoint);
        }

        dp::Res<void> connect_ipc(const IpcEndpoint &endpoint) {
            echo::trace("connecting to ", endpoint.to_string());

            // Create socket
            fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
            if (fd_ < 0) {
                echo::error("socket creation failed: ", strerror(errno));
                return dp::result::err(dp::Error::io_error("io error"));
            }
            echo::trace("ipc socket created fd=", fd_);

            // Check path length
            if (endpoint.path.size() >= sizeof(sockaddr_un::sun_path)) {
                ::close(fd_);
                fd_ = -1;
                echo::error("path too long: ", endpoint.path.c_str());
                return dp::result::err(dp::Error::invalid_argument("path too long"));
            }

            // Setup address
            struct sockaddr_un addr = {};
            addr.sun_family = AF_UNIX;
            std::strncpy(addr.sun_path, endpoint.path.c_str(), sizeof(addr.sun_path) - 1);

            // Connect
            if (::connect(fd_, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
                ::close(fd_);
                fd_ = -1;
                echo::error("connect failed: ", strerror(errno));
                return dp::result::err(dp::Error::io_error("io error"));
            }

            connected_ = true;
            remote_endpoint_ = endpoint;
            echo::debug("connected to ", endpoint.to_string());
            echo::info("IpcStream connected to ", endpoint.to_string());

            return dp::result::ok();
        }

        // Server side: bind and listen
        dp::Res<void> listen(const TcpEndpoint &endpoint) override {
            // IpcStream uses IpcEndpoint, but interface requires TcpEndpoint
            IpcEndpoint ipc_endpoint{endpoint.host};
            return listen_ipc(ipc_endpoint);
        }

        dp::Res<void> listen_ipc(const IpcEndpoint &endpoint) {
            echo::trace("listening on ", endpoint.to_string());

            // Create socket
            fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
            if (fd_ < 0) {
                echo::error("socket creation failed: ", strerror(errno));
                return dp::result::err(dp::Error::io_error("io error"));
            }
            echo::trace("ipc socket created fd=", fd_);

            // Check path length
            if (endpoint.path.size() >= sizeof(sockaddr_un::sun_path)) {
                ::close(fd_);
                fd_ = -1;
                echo::error("path too long: ", endpoint.path.c_str());
                return dp::result::err(dp::Error::invalid_argument("path too long"));
            }

            // Unlink existing socket file (clean up stale sockets)
            if (::unlink(endpoint.path.c_str()) < 0 && errno != ENOENT) {
                echo::warn("unlink failed for ", endpoint.path.c_str(), " (may not exist)");
            }

            // Setup address
            struct sockaddr_un addr = {};
            addr.sun_family = AF_UNIX;
            std::strncpy(addr.sun_path, endpoint.path.c_str(), sizeof(addr.sun_path) - 1);

            // Bind
            if (::bind(fd_, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
                ::close(fd_);
                fd_ = -1;
                echo::error("bind failed: ", strerror(errno));
                return dp::result::err(dp::Error::io_error("io error"));
            }
            echo::trace("binding to path ", endpoint.path.c_str());

            // Start listening
            if (::listen(fd_, SOMAXCONN) < 0) {
                ::close(fd_);
                fd_ = -1;
                echo::error("listen failed: ", strerror(errno));
                return dp::result::err(dp::Error::io_error("io error"));
            }

            listening_ = true;
            should_unlink_ = true; // We created the socket, so we should clean it up
            local_endpoint_ = endpoint;
            echo::debug("IpcStream listening on ", endpoint.to_string());
            echo::info("IpcStream bound to ", endpoint.to_string());

            return dp::result::ok();
        }

        // Server side: accept incoming connection
        dp::Res<std::unique_ptr<Stream>> accept() override {
            if (!listening_) {
                echo::error("accept called but not listening");
                return dp::result::err(dp::Error::invalid_argument("not listening"));
            }

            echo::trace("waiting for ipc connection on fd=", fd_);

            struct sockaddr_un client_addr = {};
            socklen_t client_len = sizeof(client_addr);

            dp::i32 client_fd = ::accept(fd_, (struct sockaddr *)&client_addr, &client_len);
            if (client_fd < 0) {
                echo::error("accept failed: ", strerror(errno));
                return dp::result::err(dp::Error::io_error("io error"));
            }

            IpcEndpoint client_endpoint{dp::String("client")};

            echo::debug("accepted ipc connection fd=", client_fd);
            echo::info("IpcStream accepted connection");

            // Create new IpcStream for the client
            auto client_stream = std::unique_ptr<Stream>(new IpcStream(client_fd, local_endpoint_, client_endpoint));

            return dp::result::ok(std::move(client_stream));
        }

        // Send a message with length-prefix framing (same as TCP)
        dp::Res<void> send(const Message &msg) override {
            if (!connected_ || fd_ < 0) {
                echo::trace("send called but not connected");
                return dp::result::err(dp::Error::not_found("not connected"));
            }

            echo::trace("send ", msg.size(), " bytes");

            // Encode length prefix (4 bytes big-endian)
            auto length_bytes = encode_u32_be(static_cast<dp::u32>(msg.size()));

            // Send length prefix
            auto res = write_exact(fd_, length_bytes.data(), 4);
            if (res.is_err()) {
                connected_ = false;
                echo::trace("send length failed: ", res.error().message.c_str());
                return res;
            }

            // Send payload
            if (!msg.empty()) {
                res = write_exact(fd_, msg.data(), msg.size());
                if (res.is_err()) {
                    connected_ = false;
                    echo::trace("send payload failed: ", res.error().message.c_str());
                    return res;
                }
            }

            echo::debug("sent ", msg.size(), " bytes");
            return dp::result::ok();
        }

        // Receive a message with length-prefix framing (same as TCP)
        // TIMEOUT HANDLING:
        // - Timeouts are expected behavior (not errors) when using set_recv_timeout()
        // - Connection stays alive after timeout - caller can retry
        // - Only fatal errors (connection closed, I/O errors) mark connection as disconnected
        // - This allows RPC to implement request timeouts without breaking the connection
        // - Behavior matches TcpStream for consistency
        dp::Res<Message> recv() override {
            if (!connected_ || fd_ < 0) {
                echo::trace("recv called but not connected");
                return dp::result::err(dp::Error::not_found("not connected"));
            }

            echo::trace("recv waiting for message");

            // Read length prefix (4 bytes)
            dp::Array<dp::u8, 4> length_bytes;
            auto res = read_exact(fd_, length_bytes.data(), 4);
            if (res.is_err()) {
                // Only mark as disconnected for non-timeout errors
                // Timeout is expected behavior - connection stays alive
                if (res.error().code != dp::Error::TIMEOUT) {
                    connected_ = false;
                    echo::trace("recv length failed: ", res.error().message.c_str());
                }
                return dp::result::err(res.error());
            }

            dp::u32 length = decode_u32_be(length_bytes.data());
            echo::trace("recv expecting ", length, " bytes");

            // Validate message size before allocating
            if (length > remote::MAX_MESSAGE_SIZE) {
                echo::error("received message too large: ", length, " bytes (max: ", remote::MAX_MESSAGE_SIZE, ")");
                connected_ = false;
                return dp::result::err(dp::Error::invalid_argument("message exceeds maximum size"));
            }

            // Allocate message buffer with exception handling
            Message msg;
            try {
                msg.resize(length);
            } catch (const std::bad_alloc &e) {
                echo::error("failed to allocate message buffer: ", length, " bytes - ", e.what());
                connected_ = false;
                return dp::result::err(dp::Error::io_error("memory allocation failed"));
            } catch (const std::exception &e) {
                echo::error("unexpected error allocating message buffer: ", e.what());
                connected_ = false;
                return dp::result::err(dp::Error::io_error("allocation error"));
            }
            if (length > 0) {
                res = read_exact(fd_, msg.data(), length);
                if (res.is_err()) {
                    // Only mark as disconnected for non-timeout errors
                    // Timeout during payload read keeps connection alive
                    if (res.error().code != dp::Error::TIMEOUT) {
                        connected_ = false;
                        echo::trace("recv payload failed: ", res.error().message.c_str());
                    }
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

                // Unlink socket file if we created it (listening socket)
                if (listening_ && should_unlink_) {
                    if (::unlink(local_endpoint_.path.c_str()) < 0) {
                        echo::warn("unlink failed for ", local_endpoint_.path.c_str());
                    } else {
                        echo::trace("unlinked socket file ", local_endpoint_.path.c_str());
                    }
                    should_unlink_ = false;
                }

                listening_ = false;
                echo::debug("IpcStream closed");
            }
        }

        // Check if connected
        bool is_connected() const override { return connected_; }
    };

} // namespace netpipe

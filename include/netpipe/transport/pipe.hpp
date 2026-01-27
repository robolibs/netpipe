#pragma once

#include <atomic>
#include <functional>
#include <thread>

#include <netpipe/transport/any.hpp>

namespace netpipe {

    /// Unified pipe class combining AnyEndpoint and AnyStream.
    ///
    /// Provides a simple API for transport-agnostic communication:
    /// - Static factories return already-connected/listening pipes
    /// - Supports blocking and async receive
    /// - Stores endpoint for reconnection support
    ///
    /// Example usage:
    /// ```cpp
    /// // Client
    /// auto client = Pipe::connect(AnyEndpoint::tcp_endpoint("localhost", 8080));
    /// if (client) {
    ///     client->send(msg);
    ///     auto resp = client->recv();
    /// }
    ///
    /// // Server
    /// auto server = Pipe::listen(AnyEndpoint::ipc_endpoint("/tmp/app.sock"));
    /// while (auto conn = server->accept()) {
    ///     conn->recv([](dp::Res<Message> msg) {
    ///         // handle async
    ///     });
    /// }
    /// ```
    class Pipe {
      public:
        using RecvCallback = std::function<void(dp::Res<Message>)>;

        // -- Factories (return ready-to-use pipes) --

        /// Connect to an endpoint (client mode).
        /// Returns already-connected pipe or error.
        static dp::Res<Pipe> connect(const AnyEndpoint &endpoint) {
            Pipe pipe;
            pipe.endpoint_ = endpoint;
            pipe.is_listener_ = false;

            auto res = pipe.stream_.connect(endpoint);
            if (res.is_err()) {
                return dp::result::err(res.error());
            }

            return dp::result::ok(std::move(pipe));
        }

        /// Listen on an endpoint (server mode).
        /// Returns already-listening pipe or error.
        static dp::Res<Pipe> listen(const AnyEndpoint &endpoint) {
            Pipe pipe;
            pipe.endpoint_ = endpoint;
            pipe.is_listener_ = true;

            auto res = pipe.stream_.listen(endpoint);
            if (res.is_err()) {
                return dp::result::err(res.error());
            }

            return dp::result::ok(std::move(pipe));
        }

        // -- Constructors --

        Pipe(const Pipe &) = delete;
        Pipe &operator=(const Pipe &) = delete;

        Pipe(Pipe &&other) noexcept
            : endpoint_(std::move(other.endpoint_)), stream_(std::move(other.stream_)),
              is_listener_(other.is_listener_), has_endpoint_(other.has_endpoint_) {
            other.has_endpoint_ = false;
        }

        Pipe &operator=(Pipe &&other) noexcept {
            if (this != &other) {
                close();
                endpoint_ = std::move(other.endpoint_);
                stream_ = std::move(other.stream_);
                is_listener_ = other.is_listener_;
                has_endpoint_ = other.has_endpoint_;
                other.has_endpoint_ = false;
            }
            return *this;
        }

        ~Pipe() { close(); }

        // -- Server operations --

        /// Accept an incoming connection (server mode only).
        /// Returns a new connected Pipe for the client connection.
        dp::Res<Pipe> accept() {
            auto res = stream_.accept();
            if (res.is_err()) {
                return dp::result::err(res.error());
            }

            Pipe pipe;
            pipe.stream_ = std::move(res.value());
            pipe.has_endpoint_ = false; // accepted connections can't reconnect
            pipe.is_listener_ = false;
            return dp::result::ok(std::move(pipe));
        }

        // -- Messaging --

        /// Send a message.
        dp::Res<void> send(const Message &msg) { return stream_.send(msg); }

        /// Receive a message (blocking).
        /// Waits until a message arrives or error occurs.
        dp::Res<Message> recv() { return stream_.recv(); }

        /// Receive a message (async).
        /// Returns immediately; callback is invoked on a background thread
        /// when a message arrives or error occurs.
        void recv(RecvCallback callback) {
            // Spawn a detached thread to do the blocking receive
            std::thread([this, cb = std::move(callback)]() {
                auto result = stream_.recv();
                cb(std::move(result));
            }).detach();
        }

        // -- Connection management --

        /// Reconnect using the stored endpoint.
        /// Only works for client pipes (not server or accepted connections).
        dp::Res<void> reconnect() {
            if (!has_endpoint_) {
                return dp::result::err(
                    dp::Error::invalid_argument("cannot reconnect: no endpoint stored (accepted connection?)"));
            }
            if (is_listener_) {
                return dp::result::err(
                    dp::Error::invalid_argument("cannot reconnect: this is a server pipe, use listen()"));
            }

            stream_.close();
            return stream_.connect(endpoint_);
        }

        /// Close the connection.
        void close() { stream_.close(); }

        /// Check if connected.
        bool is_connected() const { return stream_.is_connected(); }

        // -- Configuration --

        /// Set receive timeout in milliseconds.
        /// 0 = no timeout (blocking forever).
        dp::Res<void> set_recv_timeout(dp::u32 timeout_ms) { return stream_.set_recv_timeout(timeout_ms); }

        // -- Accessors --

        /// Get the endpoint (for logging/debugging).
        /// Note: accepted connections have an empty/default endpoint.
        const AnyEndpoint &endpoint() const { return endpoint_; }

        /// Check if this pipe has a valid endpoint (for reconnection).
        bool has_endpoint() const { return has_endpoint_; }

        /// Check if this is a listener (server) pipe.
        bool is_listener() const { return is_listener_; }

        /// Access the underlying stream (for advanced use).
        AnyStream &stream() { return stream_; }
        const AnyStream &stream() const { return stream_; }

      private:
        Pipe() = default;

        AnyEndpoint endpoint_{};
        AnyStream stream_{};
        bool is_listener_ = false;
        bool has_endpoint_ = true;
    };

} // namespace netpipe

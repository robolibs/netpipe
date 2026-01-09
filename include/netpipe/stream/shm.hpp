#pragma once

#include <atomic>
#include <chrono>
#include <cstring>
#include <datapod/pods/lockfree/ring_buffer.hpp>
#include <mutex>
#include <netpipe/remote/protocol.hpp>
#include <netpipe/stream.hpp>
#include <thread>

namespace netpipe {

    /// Connection request structure for SHM handshake
    /// Client writes this to connection queue, server reads and creates dedicated buffers
    struct ShmConnRequest {
        dp::u64 client_id;   // Unique client identifier (e.g., PID + timestamp)
        dp::u32 buffer_size; // Requested buffer size
        dp::u32 reserved;    // Padding for alignment
    };

    /// Connection response - server writes back to signal buffers are ready
    struct ShmConnResponse {
        dp::u64 client_id; // Echo back client ID
        dp::u64 conn_id;   // Server-assigned connection ID
        dp::u32 status;    // 0 = success, non-zero = error
        dp::u32 reserved;
    };

    /// Bidirectional shared memory stream with TCP-like connection semantics
    ///
    /// Supports multiple independent connections via accept()/connect() pattern:
    /// - Server: listen_shm() -> accept() loop (returns new ShmStream per client)
    /// - Client: connect_shm() (establishes dedicated buffer pair with server)
    ///
    /// Architecture:
    /// - Connection queue: MPMC buffer where clients register
    /// - Response queue: MPMC buffer where server ACKs connections
    /// - Per-connection: dedicated s2c/c2s buffer pair
    ///
    /// Message framing: [4-byte length (big-endian)] [payload]
    class ShmStream : public Stream {
      private:
        // Per-connection buffers (for established connections)
        dp::RingBuffer<dp::MPMC, dp::u8> send_buffer_;
        dp::RingBuffer<dp::MPMC, dp::u8> recv_buffer_;

        // Listening state (for server accepting connections)
        dp::RingBuffer<dp::MPMC, dp::u8> conn_queue_; // Clients write connection requests
        dp::RingBuffer<dp::MPMC, dp::u8> resp_queue_; // Server writes responses
        std::atomic<dp::u64> next_conn_id_{0};

        bool connected_;
        bool listening_;
        bool is_server_;
        bool owns_shm_; // True if we created the SHM (should unlink on close)
        dp::String channel_name_;
        dp::u64 conn_id_; // Connection ID for this stream
        dp::usize buffer_size_;
        dp::u32 recv_timeout_ms_;
        static constexpr dp::u32 POLL_INTERVAL_US = 10;
        static constexpr dp::usize CONN_QUEUE_SIZE = 4096; // Small queue for connection handshakes

        mutable std::mutex send_mutex_;

        /// Private constructor for accepted connections (server-side)
        ShmStream(dp::RingBuffer<dp::MPMC, dp::u8> &&send_buf, dp::RingBuffer<dp::MPMC, dp::u8> &&recv_buf,
                  const dp::String &channel_name, dp::u64 conn_id, dp::usize buffer_size)
            : send_buffer_(std::move(send_buf)), recv_buffer_(std::move(recv_buf)), connected_(true), listening_(false),
              is_server_(true), owns_shm_(true), channel_name_(channel_name), conn_id_(conn_id),
              buffer_size_(buffer_size), recv_timeout_ms_(0) {
            echo::debug("ShmStream created for connection ", conn_id);
        }

        /// Generate unique client ID
        static dp::u64 generate_client_id() {
            auto now = std::chrono::steady_clock::now().time_since_epoch();
            auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
            return static_cast<dp::u64>(::getpid()) ^ static_cast<dp::u64>(ns);
        }

        /// Write a struct to ring buffer byte-by-byte
        template <typename T>
        bool write_struct(dp::RingBuffer<dp::MPMC, dp::u8> &buf, const T &data, dp::u32 timeout_ms) {
            const auto *bytes = reinterpret_cast<const dp::u8 *>(&data);
            auto start = std::chrono::steady_clock::now();

            for (dp::usize i = 0; i < sizeof(T); i++) {
                while (buf.push(bytes[i]).is_err()) {
                    if (timeout_ms > 0) {
                        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now() - start)
                                           .count();
                        if (elapsed >= timeout_ms)
                            return false;
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(POLL_INTERVAL_US));
                }
            }
            return true;
        }

        /// Read a struct from ring buffer byte-by-byte
        template <typename T> bool read_struct(dp::RingBuffer<dp::MPMC, dp::u8> &buf, T &data, dp::u32 timeout_ms) {
            auto *bytes = reinterpret_cast<dp::u8 *>(&data);
            auto start = std::chrono::steady_clock::now();

            for (dp::usize i = 0; i < sizeof(T); i++) {
                while (true) {
                    auto res = buf.pop();
                    if (res.is_ok()) {
                        bytes[i] = res.value();
                        break;
                    }
                    if (timeout_ms > 0) {
                        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now() - start)
                                           .count();
                        if (elapsed >= timeout_ms)
                            return false;
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(POLL_INTERVAL_US));
                }
            }
            return true;
        }

      public:
        ShmStream()
            : connected_(false), listening_(false), is_server_(false), owns_shm_(false), conn_id_(0), buffer_size_(0),
              recv_timeout_ms_(0) {
            echo::trace("ShmStream constructed");
        }

        ~ShmStream() override {
            if (connected_ || listening_) {
                close();
            }
        }

        // Delete copy
        ShmStream(const ShmStream &) = delete;
        ShmStream &operator=(const ShmStream &) = delete;

        // Allow move
        ShmStream(ShmStream &&other) noexcept
            : send_buffer_(std::move(other.send_buffer_)), recv_buffer_(std::move(other.recv_buffer_)),
              conn_queue_(std::move(other.conn_queue_)), resp_queue_(std::move(other.resp_queue_)),
              connected_(other.connected_), listening_(other.listening_), is_server_(other.is_server_),
              owns_shm_(other.owns_shm_), channel_name_(std::move(other.channel_name_)), conn_id_(other.conn_id_),
              buffer_size_(other.buffer_size_), recv_timeout_ms_(other.recv_timeout_ms_) {
            next_conn_id_.store(other.next_conn_id_.load());
            other.connected_ = false;
            other.listening_ = false;
            other.owns_shm_ = false;
        }

        ShmStream &operator=(ShmStream &&other) noexcept {
            if (this != &other) {
                if (connected_ || listening_)
                    close();

                send_buffer_ = std::move(other.send_buffer_);
                recv_buffer_ = std::move(other.recv_buffer_);
                conn_queue_ = std::move(other.conn_queue_);
                resp_queue_ = std::move(other.resp_queue_);
                next_conn_id_.store(other.next_conn_id_.load());
                connected_ = other.connected_;
                listening_ = other.listening_;
                is_server_ = other.is_server_;
                owns_shm_ = other.owns_shm_;
                channel_name_ = std::move(other.channel_name_);
                conn_id_ = other.conn_id_;
                buffer_size_ = other.buffer_size_;
                recv_timeout_ms_ = other.recv_timeout_ms_;

                other.connected_ = false;
                other.listening_ = false;
                other.owns_shm_ = false;
            }
            return *this;
        }

        /// Server side: create connection queue and start listening
        dp::Res<void> listen(const TcpEndpoint &endpoint) override {
            ShmEndpoint shm_endpoint{endpoint.host, static_cast<dp::usize>(endpoint.port)};
            return listen_shm(shm_endpoint);
        }

        dp::Res<void> listen_shm(const ShmEndpoint &endpoint) {
            echo::trace("ShmStream listen on ", endpoint.to_string());

            if (endpoint.name.size() > 240) {
                echo::error("shm name too long (max 240 chars): ", endpoint.name.size());
                return dp::result::err(dp::Error::invalid_argument("shm name exceeds limit"));
            }

            is_server_ = true;
            owns_shm_ = true;
            channel_name_ = endpoint.name;
            buffer_size_ = endpoint.size;

            // Create connection queue names
            char connq_name[256];
            char respq_name[256];
            snprintf(connq_name, sizeof(connq_name), "/%s_connq", endpoint.name.c_str());
            snprintf(respq_name, sizeof(respq_name), "/%s_respq", endpoint.name.c_str());

            // Clean up any existing shared memory
            ::shm_unlink(connq_name);
            ::shm_unlink(respq_name);

            // Create connection queue (clients write requests here)
            auto connq_res = dp::RingBuffer<dp::MPMC, dp::u8>::create_shm(dp::String(connq_name), CONN_QUEUE_SIZE);
            if (connq_res.is_err()) {
                echo::error("failed to create connection queue");
                return dp::result::err(dp::Error::io_error("failed to create connection queue"));
            }
            conn_queue_ = std::move(connq_res.value());

            // Create response queue (server writes ACKs here)
            auto respq_res = dp::RingBuffer<dp::MPMC, dp::u8>::create_shm(dp::String(respq_name), CONN_QUEUE_SIZE);
            if (respq_res.is_err()) {
                echo::error("failed to create response queue");
                return dp::result::err(dp::Error::io_error("failed to create response queue"));
            }
            resp_queue_ = std::move(respq_res.value());

            listening_ = true;
            echo::info("ShmStream listening on channel: ", endpoint.name.c_str());

            return dp::result::ok();
        }

        /// Server side: accept incoming connection
        /// Blocks until a client connects, then returns a NEW ShmStream for that connection
        dp::Res<std::unique_ptr<Stream>> accept() override {
            if (!listening_) {
                echo::error("accept called but not listening");
                return dp::result::err(dp::Error::invalid_argument("not listening"));
            }

            echo::trace("ShmStream waiting for connection");

            // Read connection request from queue
            ShmConnRequest req;
            if (!read_struct(conn_queue_, req, recv_timeout_ms_)) {
                return dp::result::err(dp::Error::timeout("accept timeout"));
            }

            echo::debug("received connection request from client ", req.client_id);

            // Assign connection ID
            dp::u64 conn_id = next_conn_id_.fetch_add(1);

            // Create dedicated buffer pair for this connection
            char s2c_name[256];
            char c2s_name[256];
            snprintf(s2c_name, sizeof(s2c_name), "/%s_%llu_s2c", channel_name_.c_str(),
                     static_cast<unsigned long long>(conn_id));
            snprintf(c2s_name, sizeof(c2s_name), "/%s_%llu_c2s", channel_name_.c_str(),
                     static_cast<unsigned long long>(conn_id));

            // Clean up any stale buffers
            ::shm_unlink(s2c_name);
            ::shm_unlink(c2s_name);

            dp::usize buf_size = req.buffer_size > 0 ? req.buffer_size : buffer_size_;

            // Create server-to-client buffer
            auto s2c_res = dp::RingBuffer<dp::MPMC, dp::u8>::create_shm(dp::String(s2c_name), buf_size);
            if (s2c_res.is_err()) {
                echo::error("failed to create s2c buffer for conn ", conn_id);
                // Send error response
                ShmConnResponse resp{req.client_id, conn_id, 1, 0};
                write_struct(resp_queue_, resp, 1000);
                return dp::result::err(dp::Error::io_error("failed to create connection buffers"));
            }

            // Create client-to-server buffer
            auto c2s_res = dp::RingBuffer<dp::MPMC, dp::u8>::create_shm(dp::String(c2s_name), buf_size);
            if (c2s_res.is_err()) {
                echo::error("failed to create c2s buffer for conn ", conn_id);
                ShmConnResponse resp{req.client_id, conn_id, 2, 0};
                write_struct(resp_queue_, resp, 1000);
                return dp::result::err(dp::Error::io_error("failed to create connection buffers"));
            }

            // Send success response to client
            ShmConnResponse resp{req.client_id, conn_id, 0, 0};
            if (!write_struct(resp_queue_, resp, 5000)) {
                echo::error("failed to send connection response");
                return dp::result::err(dp::Error::io_error("failed to send connection response"));
            }

            echo::info("ShmStream accepted connection ", conn_id, " from client ", req.client_id);

            // Create new ShmStream for this connection
            // Server sends on s2c, receives on c2s
            auto client_stream = std::unique_ptr<Stream>(new ShmStream(std::move(s2c_res.value()), // send = s2c
                                                                       std::move(c2s_res.value()), // recv = c2s
                                                                       channel_name_, conn_id, buf_size));

            return dp::result::ok(std::move(client_stream));
        }

        /// Client side: connect to server
        dp::Res<void> connect(const TcpEndpoint &endpoint) override {
            ShmEndpoint shm_endpoint{endpoint.host, static_cast<dp::usize>(endpoint.port)};
            return connect_shm(shm_endpoint);
        }

        dp::Res<void> connect_shm(const ShmEndpoint &endpoint) {
            echo::trace("ShmStream connecting to ", endpoint.to_string());

            if (endpoint.name.size() > 240) {
                echo::error("shm name too long (max 240 chars): ", endpoint.name.size());
                return dp::result::err(dp::Error::invalid_argument("shm name exceeds limit"));
            }

            is_server_ = false;
            owns_shm_ = false;
            channel_name_ = endpoint.name;
            buffer_size_ = endpoint.size;

            // Attach to connection queues
            char connq_name[256];
            char respq_name[256];
            snprintf(connq_name, sizeof(connq_name), "/%s_connq", endpoint.name.c_str());
            snprintf(respq_name, sizeof(respq_name), "/%s_respq", endpoint.name.c_str());

            auto connq_res = dp::RingBuffer<dp::MPMC, dp::u8>::attach_shm(dp::String(connq_name));
            if (connq_res.is_err()) {
                echo::error("failed to attach to connection queue (server not listening?)");
                return dp::result::err(dp::Error::io_error("server not listening"));
            }
            conn_queue_ = std::move(connq_res.value());

            auto respq_res = dp::RingBuffer<dp::MPMC, dp::u8>::attach_shm(dp::String(respq_name));
            if (respq_res.is_err()) {
                echo::error("failed to attach to response queue");
                return dp::result::err(dp::Error::io_error("failed to attach to response queue"));
            }
            resp_queue_ = std::move(respq_res.value());

            // Generate unique client ID and send connection request
            dp::u64 client_id = generate_client_id();
            ShmConnRequest req{client_id, static_cast<dp::u32>(endpoint.size), 0};

            echo::debug("sending connection request, client_id=", client_id);

            if (!write_struct(conn_queue_, req, 5000)) {
                echo::error("timeout sending connection request");
                return dp::result::err(dp::Error::timeout("connection timeout"));
            }

            // Wait for response (need to filter for our client_id)
            auto start = std::chrono::steady_clock::now();
            constexpr dp::u32 CONNECT_TIMEOUT_MS = 10000;

            while (true) {
                ShmConnResponse resp;
                if (read_struct(resp_queue_, resp, 100)) {
                    if (resp.client_id == client_id) {
                        if (resp.status != 0) {
                            echo::error("server rejected connection, status=", resp.status);
                            return dp::result::err(dp::Error::io_error("connection rejected"));
                        }

                        conn_id_ = resp.conn_id;
                        echo::debug("received connection response, conn_id=", conn_id_);

                        // Attach to our dedicated buffers
                        char s2c_name[256];
                        char c2s_name[256];
                        snprintf(s2c_name, sizeof(s2c_name), "/%s_%llu_s2c", channel_name_.c_str(),
                                 static_cast<unsigned long long>(conn_id_));
                        snprintf(c2s_name, sizeof(c2s_name), "/%s_%llu_c2s", channel_name_.c_str(),
                                 static_cast<unsigned long long>(conn_id_));

                        // Client sends on c2s, receives on s2c
                        auto send_res = dp::RingBuffer<dp::MPMC, dp::u8>::attach_shm(dp::String(c2s_name));
                        if (send_res.is_err()) {
                            echo::error("failed to attach to c2s buffer");
                            return dp::result::err(dp::Error::io_error("failed to attach to buffers"));
                        }
                        send_buffer_ = std::move(send_res.value());

                        auto recv_res = dp::RingBuffer<dp::MPMC, dp::u8>::attach_shm(dp::String(s2c_name));
                        if (recv_res.is_err()) {
                            echo::error("failed to attach to s2c buffer");
                            return dp::result::err(dp::Error::io_error("failed to attach to buffers"));
                        }
                        recv_buffer_ = std::move(recv_res.value());

                        connected_ = true;
                        echo::info("ShmStream connected, conn_id=", conn_id_);
                        return dp::result::ok();
                    }
                    // Response for different client - this shouldn't happen often
                    // but could in race conditions. The other client will retry.
                    echo::trace("got response for different client, continuing");
                }

                auto elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
                        .count();
                if (elapsed >= CONNECT_TIMEOUT_MS) {
                    echo::error("connection timeout waiting for server response");
                    return dp::result::err(dp::Error::timeout("connection timeout"));
                }
            }
        }

        /// Send message with length-prefix framing
        dp::Res<void> send(const Message &msg) override {
            std::lock_guard<std::mutex> lock(send_mutex_);

            if (!connected_) {
                echo::trace("send called but not connected");
                return dp::result::err(dp::Error::not_found("not connected"));
            }

            dp::usize max_message_size = buffer_size_ / 2;
            if (msg.size() > max_message_size) {
                echo::error("message too large: ", msg.size(), " bytes (max ", max_message_size, ")");
                return dp::result::err(dp::Error::invalid_argument("message exceeds maximum size"));
            }

            echo::trace("shm send ", msg.size(), " bytes");

            dp::usize total_bytes = 4 + msg.size();

            // Wait for space
            dp::u32 retry_interval_us = POLL_INTERVAL_US;
            constexpr dp::u32 MAX_RETRY_MS = 10000;
            auto start_time = std::chrono::steady_clock::now();

            while (true) {
                dp::usize available = send_buffer_.capacity() - send_buffer_.size();
                if (available >= total_bytes)
                    break;

                auto elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time)
                        .count();
                if (elapsed >= MAX_RETRY_MS) {
                    echo::error("ring buffer full timeout");
                    return dp::result::err(dp::Error::timeout("buffer full"));
                }

                std::this_thread::sleep_for(std::chrono::microseconds(retry_interval_us));
                if (retry_interval_us < 1000) {
                    retry_interval_us = std::min(retry_interval_us * 2, 1000u);
                }
            }

            // Write length prefix
            auto length_bytes = encode_u32_be(static_cast<dp::u32>(msg.size()));
            for (dp::usize i = 0; i < 4; i++) {
                while (send_buffer_.push(length_bytes[i]).is_err()) {
                    std::this_thread::yield();
                }
            }

            // Write payload
            for (dp::usize i = 0; i < msg.size(); i++) {
                while (send_buffer_.push(msg[i]).is_err()) {
                    std::this_thread::yield();
                }
            }

            echo::debug("sent ", msg.size(), " bytes");
            return dp::result::ok();
        }

        /// Receive message with length-prefix framing
        dp::Res<Message> recv() override {
            if (!connected_) {
                echo::trace("recv called but not connected");
                return dp::result::err(dp::Error::not_found("not connected"));
            }

            echo::trace("shm recv waiting");

            auto start_time = std::chrono::steady_clock::now();
            dp::u32 poll_interval_us = POLL_INTERVAL_US;

            // Read length prefix
            dp::Array<dp::u8, 4> length_bytes;
            for (dp::usize i = 0; i < 4; i++) {
                while (true) {
                    auto res = recv_buffer_.pop();
                    if (res.is_ok()) {
                        length_bytes[i] = res.value();
                        poll_interval_us = POLL_INTERVAL_US;
                        break;
                    }

                    if (recv_timeout_ms_ > 0) {
                        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now() - start_time)
                                           .count();
                        if (elapsed >= recv_timeout_ms_) {
                            return dp::result::err(dp::Error::timeout("recv timeout"));
                        }
                    }

                    std::this_thread::sleep_for(std::chrono::microseconds(poll_interval_us));
                    if (poll_interval_us < 1000) {
                        poll_interval_us = std::min(poll_interval_us * 2, 1000u);
                    }
                }
            }

            dp::u32 length = decode_u32_be(length_bytes.data());
            echo::trace("recv expecting ", length, " bytes");

            if (length > remote::MAX_MESSAGE_SIZE) {
                echo::error("message too large: ", length, " bytes");
                connected_ = false;
                return dp::result::err(dp::Error::invalid_argument("message exceeds maximum size"));
            }

            Message msg;
            try {
                msg.resize(length);
            } catch (const std::bad_alloc &) {
                echo::error("allocation failed: ", length, " bytes");
                connected_ = false;
                return dp::result::err(dp::Error::io_error("memory allocation failed"));
            }

            for (dp::usize i = 0; i < length; i++) {
                while (true) {
                    auto res = recv_buffer_.pop();
                    if (res.is_ok()) {
                        msg[i] = res.value();
                        poll_interval_us = POLL_INTERVAL_US;
                        break;
                    }

                    if (recv_timeout_ms_ > 0) {
                        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now() - start_time)
                                           .count();
                        if (elapsed >= recv_timeout_ms_) {
                            return dp::result::err(dp::Error::timeout("recv timeout"));
                        }
                    }

                    std::this_thread::sleep_for(std::chrono::microseconds(poll_interval_us));
                    if (poll_interval_us < 1000) {
                        poll_interval_us = std::min(poll_interval_us * 2, 1000u);
                    }
                }
            }

            echo::debug("received ", length, " bytes");
            return dp::result::ok(std::move(msg));
        }

        dp::Res<void> set_recv_timeout(dp::u32 timeout_ms) override {
            recv_timeout_ms_ = timeout_ms;
            echo::trace("set recv timeout to ", timeout_ms, "ms");
            return dp::result::ok();
        }

        void close() override {
            if (connected_) {
                echo::trace("closing shm connection ", conn_id_);
                connected_ = false;

                // If we own the SHM (server-side accepted connection), unlink buffers
                if (owns_shm_ && conn_id_ > 0) {
                    char s2c_name[256];
                    char c2s_name[256];
                    snprintf(s2c_name, sizeof(s2c_name), "/%s_%llu_s2c", channel_name_.c_str(),
                             static_cast<unsigned long long>(conn_id_));
                    snprintf(c2s_name, sizeof(c2s_name), "/%s_%llu_c2s", channel_name_.c_str(),
                             static_cast<unsigned long long>(conn_id_));
                    ::shm_unlink(s2c_name);
                    ::shm_unlink(c2s_name);
                }
            }

            if (listening_) {
                echo::trace("closing shm listener");
                listening_ = false;

                if (owns_shm_) {
                    char connq_name[256];
                    char respq_name[256];
                    snprintf(connq_name, sizeof(connq_name), "/%s_connq", channel_name_.c_str());
                    snprintf(respq_name, sizeof(respq_name), "/%s_respq", channel_name_.c_str());
                    ::shm_unlink(connq_name);
                    ::shm_unlink(respq_name);
                }
            }

            echo::debug("ShmStream closed");
        }

        bool is_connected() const override { return connected_; }
        bool is_listening() const { return listening_; }
        const dp::String &channel_name() const { return channel_name_; }
        dp::u64 connection_id() const { return conn_id_; }
        bool is_server() const { return is_server_; }
    };

} // namespace netpipe

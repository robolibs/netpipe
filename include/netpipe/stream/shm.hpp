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

    /// Bidirectional shared memory stream using TWO SPSC ring buffers
    /// Ultra-low latency, same machine only
    /// Server writes to s2c, reads from c2s
    /// Client writes to c2s, reads from s2c
    ///
    /// IMPORTANT: This implementation uses SPSC (Single Producer Single Consumer) buffers.
    /// The send() method is protected by a mutex to serialize multiple producers, but
    /// the underlying buffer is still SPSC. For best performance, prefer single-threaded
    /// send patterns or use TCP/IPC for heavy multi-threaded bidirectional RPC.
    ///
    /// Message framing: [4-byte length (big-endian)] [payload]
    /// No boundary markers - length prefix is sufficient for framing.
    class ShmStream : public Stream {
      private:
        dp::RingBuffer<dp::SPSC, dp::u8> send_buffer_;
        dp::RingBuffer<dp::SPSC, dp::u8> recv_buffer_;
        bool connected_;
        bool is_server_;
        dp::String channel_name_;
        dp::usize buffer_size_;
        dp::u32 recv_timeout_ms_;
        static constexpr dp::u32 POLL_INTERVAL_US = 10; // Start with 10us for low latency

        // Mutex to protect send operations (SPSC requires single producer)
        // Multiple threads may call send() concurrently in bidirectional mode
        mutable std::mutex send_mutex_;

      public:
        ShmStream() : connected_(false), is_server_(false), recv_timeout_ms_(0) {
            echo::trace("ShmStream constructed");
        }
        ~ShmStream() override {
            if (connected_) {
                close();
            }
        }

        // Delete copy constructor and assignment (mutex is not copyable)
        ShmStream(const ShmStream &) = delete;
        ShmStream &operator=(const ShmStream &) = delete;

        // Allow move
        ShmStream(ShmStream &&other) noexcept
            : send_buffer_(std::move(other.send_buffer_)), recv_buffer_(std::move(other.recv_buffer_)),
              connected_(other.connected_), is_server_(other.is_server_), channel_name_(std::move(other.channel_name_)),
              buffer_size_(other.buffer_size_), recv_timeout_ms_(other.recv_timeout_ms_) {
            other.connected_ = false;
        }
        ShmStream &operator=(ShmStream &&other) noexcept {
            if (this != &other) {
                send_buffer_ = std::move(other.send_buffer_);
                recv_buffer_ = std::move(other.recv_buffer_);
                connected_ = other.connected_;
                is_server_ = other.is_server_;
                channel_name_ = std::move(other.channel_name_);
                buffer_size_ = other.buffer_size_;
                recv_timeout_ms_ = other.recv_timeout_ms_;
                other.connected_ = false;
            }
            return *this;
        }

        /// Server side: create BOTH ring buffers
        dp::Res<void> listen(const TcpEndpoint &endpoint) override {
            ShmEndpoint shm_endpoint{endpoint.host, static_cast<dp::usize>(endpoint.port)};
            return listen_shm(shm_endpoint);
        }

        dp::Res<void> listen_shm(const ShmEndpoint &endpoint) {
            echo::trace("creating shm bidirectional buffers ", endpoint.to_string());

            // Validate name length (need room for "/_s2c" suffix)
            if (endpoint.name.size() > 250) {
                echo::error("shm name too long (max 250 chars): ", endpoint.name.size());
                return dp::result::err(dp::Error::invalid_argument("shm name exceeds 250 character limit"));
            }

            is_server_ = true;
            channel_name_ = endpoint.name;
            buffer_size_ = endpoint.size;

            // Server writes to s2c, reads from c2s
            char s2c_name[256];
            char c2s_name[256];
            snprintf(s2c_name, sizeof(s2c_name), "/%s_s2c", endpoint.name.c_str());
            snprintf(c2s_name, sizeof(c2s_name), "/%s_c2s", endpoint.name.c_str());

            // Clean up any existing shared memory
            ::shm_unlink(s2c_name);
            ::shm_unlink(c2s_name);

            // Create send buffer (server-to-client)
            auto send_res = dp::RingBuffer<dp::SPSC, dp::u8>::create_shm(dp::String(s2c_name), endpoint.size);
            if (send_res.is_err()) {
                echo::error("failed to create s2c ring buffer");
                return dp::result::err(dp::Error::io_error("failed to create s2c buffer"));
            }
            send_buffer_ = std::move(send_res.value());

            // Create recv buffer (client-to-server)
            auto recv_res = dp::RingBuffer<dp::SPSC, dp::u8>::create_shm(dp::String(c2s_name), endpoint.size);
            if (recv_res.is_err()) {
                echo::error("failed to create c2s ring buffer");
                return dp::result::err(dp::Error::io_error("failed to create c2s buffer"));
            }
            recv_buffer_ = std::move(recv_res.value());

            connected_ = true;
            echo::info("ShmStream server created on channel: ", endpoint.name.c_str());

            return dp::result::ok();
        }

        /// Client side: attach to BOTH ring buffers
        dp::Res<void> connect(const TcpEndpoint &endpoint) override {
            ShmEndpoint shm_endpoint{endpoint.host, static_cast<dp::usize>(endpoint.port)};
            return connect_shm(shm_endpoint);
        }

        dp::Res<void> connect_shm(const ShmEndpoint &endpoint) {
            echo::trace("connecting to shm bidirectional buffers ", endpoint.to_string());

            // Validate name length
            if (endpoint.name.size() > 250) {
                echo::error("shm name too long (max 250 chars): ", endpoint.name.size());
                return dp::result::err(dp::Error::invalid_argument("shm name exceeds 250 character limit"));
            }

            is_server_ = false;
            channel_name_ = endpoint.name;
            buffer_size_ = endpoint.size;

            // Client writes to c2s, reads from s2c
            char s2c_name[256];
            char c2s_name[256];
            snprintf(s2c_name, sizeof(s2c_name), "/%s_s2c", endpoint.name.c_str());
            snprintf(c2s_name, sizeof(c2s_name), "/%s_c2s", endpoint.name.c_str());

            // Attach to send buffer (client-to-server)
            auto send_res = dp::RingBuffer<dp::SPSC, dp::u8>::attach_shm(dp::String(c2s_name));
            if (send_res.is_err()) {
                echo::error("failed to attach to c2s ring buffer");
                return dp::result::err(dp::Error::io_error("failed to attach to c2s buffer"));
            }
            send_buffer_ = std::move(send_res.value());

            // Attach to recv buffer (server-to-client)
            auto recv_res = dp::RingBuffer<dp::SPSC, dp::u8>::attach_shm(dp::String(s2c_name));
            if (recv_res.is_err()) {
                echo::error("failed to attach to s2c ring buffer");
                return dp::result::err(dp::Error::io_error("failed to attach to s2c buffer"));
            }
            recv_buffer_ = std::move(recv_res.value());

            connected_ = true;
            echo::info("ShmStream client connected to channel: ", endpoint.name.c_str());

            return dp::result::ok();
        }

        /// Accept not applicable for SHM (connection established when client attaches)
        dp::Res<std::unique_ptr<Stream>> accept() override {
            echo::error("accept not supported for ShmStream");
            return dp::result::err(dp::Error::invalid_argument("accept not supported"));
        }

        /// Send message with length-prefix framing
        /// Thread-safe: protected by mutex for multi-producer scenarios
        ///
        /// The entire message (length + payload) is written in a tight loop without
        /// yielding to ensure atomicity from the consumer's perspective.
        dp::Res<void> send(const Message &msg) override {
            // Lock mutex to ensure single producer (SPSC requirement)
            std::lock_guard<std::mutex> lock(send_mutex_);

            if (!connected_) {
                echo::trace("send called but not connected");
                return dp::result::err(dp::Error::not_found("not connected"));
            }

            // Validate message size (max is buffer_size / 2 for bidirectional)
            dp::usize max_message_size = buffer_size_ / 2;
            if (msg.size() > max_message_size) {
                echo::error("message too large: ", msg.size(), " bytes (max ", max_message_size, " bytes)");
                return dp::result::err(dp::Error::invalid_argument("message exceeds maximum size"));
            }

            echo::trace("shm send ", msg.size(), " bytes");

            // Total bytes: 4 byte length + payload
            dp::usize total_bytes = 4 + msg.size();

            // Wait for enough space with exponential backoff
            dp::u32 retry_interval_us = POLL_INTERVAL_US;
            constexpr dp::u32 MAX_RETRY_MS = 10000; // 10 second timeout
            auto start_time = std::chrono::steady_clock::now();

            while (true) {
                dp::usize buffer_capacity = send_buffer_.capacity();
                dp::usize current_size = send_buffer_.size();
                dp::usize available = buffer_capacity - current_size;

                if (available >= total_bytes) {
                    break;
                }

                auto elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time)
                        .count();
                if (elapsed >= MAX_RETRY_MS) {
                    echo::error("ring buffer full timeout: need ", total_bytes, " bytes, have ", available);
                    return dp::result::err(dp::Error::io_error("ring buffer full - timeout"));
                }

                std::this_thread::sleep_for(std::chrono::microseconds(retry_interval_us));
                if (retry_interval_us < 1000) {
                    retry_interval_us = std::min(retry_interval_us * 2, 1000u);
                }
            }

            // Encode length prefix (4 bytes big-endian)
            auto length_bytes = encode_u32_be(static_cast<dp::u32>(msg.size()));

            // Push length prefix - tight loop, no yielding
            for (dp::usize i = 0; i < 4; i++) {
                while (send_buffer_.push(length_bytes[i]).is_err()) {
                    // Spin-wait - should rarely happen since we checked space
                    std::this_thread::yield();
                }
            }

            // Push payload - tight loop, no yielding
            for (dp::usize i = 0; i < msg.size(); i++) {
                while (send_buffer_.push(msg[i]).is_err()) {
                    // Spin-wait - should rarely happen since we checked space
                    std::this_thread::yield();
                }
            }

            echo::debug("sent ", msg.size(), " bytes");
            return dp::result::ok();
        }

        /// Receive message with length-prefix framing
        /// Blocks until a complete message is available or timeout
        dp::Res<Message> recv() override {
            if (!connected_) {
                echo::trace("recv called but not connected");
                return dp::result::err(dp::Error::not_found("not connected"));
            }

            echo::trace("shm recv waiting");

            auto start_time = std::chrono::steady_clock::now();
            dp::u32 poll_interval_us = POLL_INTERVAL_US;

            // Read length prefix (4 bytes) with polling
            dp::Array<dp::u8, 4> length_bytes;
            for (dp::usize i = 0; i < 4; i++) {
                while (true) {
                    auto res = recv_buffer_.pop();
                    if (res.is_ok()) {
                        length_bytes[i] = res.value();
                        poll_interval_us = POLL_INTERVAL_US; // Reset backoff
                        break;
                    }

                    // Check timeout
                    if (recv_timeout_ms_ > 0) {
                        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now() - start_time)
                                           .count();
                        if (elapsed >= recv_timeout_ms_) {
                            echo::trace("recv timeout waiting for length byte ", i);
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

            // Validate message size
            if (length > remote::MAX_MESSAGE_SIZE) {
                echo::error("received message too large: ", length, " bytes");
                connected_ = false;
                return dp::result::err(dp::Error::invalid_argument("message exceeds maximum size"));
            }

            dp::usize max_buffer_size = buffer_size_ / 2;
            if (length > max_buffer_size) {
                echo::error("received message exceeds buffer capacity: ", length, " bytes");
                connected_ = false;
                return dp::result::err(dp::Error::invalid_argument("message exceeds buffer capacity"));
            }

            // Allocate message buffer
            Message msg;
            try {
                msg.resize(length);
            } catch (const std::bad_alloc &e) {
                echo::error("failed to allocate message buffer: ", length, " bytes");
                connected_ = false;
                return dp::result::err(dp::Error::io_error("memory allocation failed"));
            }

            // Read payload with polling
            for (dp::usize i = 0; i < length; i++) {
                while (true) {
                    auto res = recv_buffer_.pop();
                    if (res.is_ok()) {
                        msg[i] = res.value();
                        poll_interval_us = POLL_INTERVAL_US; // Reset backoff
                        break;
                    }

                    // Check timeout
                    if (recv_timeout_ms_ > 0) {
                        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now() - start_time)
                                           .count();
                        if (elapsed >= recv_timeout_ms_) {
                            echo::trace("recv timeout waiting for payload byte ", i);
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

        /// Set receive timeout in milliseconds
        dp::Res<void> set_recv_timeout(dp::u32 timeout_ms) override {
            recv_timeout_ms_ = timeout_ms;
            echo::trace("set recv timeout to ", timeout_ms, "ms");
            return dp::result::ok();
        }

        void close() override {
            if (connected_) {
                echo::trace("closing shm stream");
                connected_ = false;
                echo::debug("ShmStream closed");
            }
        }

        bool is_connected() const override { return connected_; }

        /// Get the channel name
        const dp::String &channel_name() const { return channel_name_; }

        /// Check if this is the server side
        bool is_server() const { return is_server_; }
    };

} // namespace netpipe

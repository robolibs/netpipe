#pragma once

#include <atomic>
#include <chrono>
#include <datapod/pods/lockfree/ring_buffer.hpp>
#include <netpipe/stream.hpp>
#include <thread>

namespace netpipe {

    /// Bidirectional shared memory stream using TWO SPSC ring buffers
    /// Ultra-low latency, same machine only
    /// Server writes to s2c, reads from c2s
    /// Client writes to c2s, reads from s2c
    class ShmStream : public Stream {
      private:
        dp::RingBuffer<dp::SPSC, dp::u8> send_buffer_;
        dp::RingBuffer<dp::SPSC, dp::u8> recv_buffer_;
        bool connected_;
        bool is_server_;
        dp::String channel_name_;
        dp::usize buffer_size_;
        dp::u32 recv_timeout_ms_;
        static constexpr dp::u32 POLL_INTERVAL_US = 100;

      public:
        ShmStream() : connected_(false), is_server_(false), recv_timeout_ms_(0) {
            echo::trace("ShmStream constructed");
        }
        ~ShmStream() override {
            if (connected_) {
                close();
            }
        }

        /// Server side: create BOTH ring buffers
        dp::Res<void> listen(const TcpEndpoint &endpoint) override {
            ShmEndpoint shm_endpoint{endpoint.host, static_cast<dp::usize>(endpoint.port)};
            return listen_shm(shm_endpoint);
        }

        dp::Res<void> listen_shm(const ShmEndpoint &endpoint) {
            echo::trace("creating shm bidirectional buffers ", endpoint.to_string());

            // Validate name length (need room for "/_s2c" suffix)
            // With "/" prefix + "_s2c" suffix = 5 extra chars
            // POSIX NAME_MAX is 255, so max base name is 255 - 5 = 250
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
        dp::Res<void> send(const Message &msg) override {
            if (!connected_) {
                echo::trace("send called but not connected");
                return dp::result::err(dp::Error::not_found("not connected"));
            }

            echo::trace("shm send ", msg.size(), " bytes");

            // Encode length prefix (4 bytes big-endian)
            auto length_bytes = encode_u32_be(static_cast<dp::u32>(msg.size()));

            // Push length prefix bytes
            for (dp::usize i = 0; i < 4; i++) {
                auto res = send_buffer_.push(length_bytes[i]);
                if (res.is_err()) {
                    echo::warn("ring buffer full, cannot send length");
                    return dp::result::err(dp::Error::io_error("ring buffer full"));
                }
            }

            // Push payload bytes
            for (dp::usize i = 0; i < msg.size(); i++) {
                auto res = send_buffer_.push(msg[i]);
                if (res.is_err()) {
                    echo::warn("ring buffer full, cannot send payload");
                    return dp::result::err(dp::Error::io_error("ring buffer full"));
                }
            }

            echo::debug("sent ", msg.size(), " bytes");
            return dp::result::ok();
        }

        /// Receive message with length-prefix framing
        /// Supports blocking with timeout via polling
        /// TIMEOUT HANDLING:
        /// - Timeouts are expected behavior (not errors) when using set_recv_timeout()
        /// - Connection stays alive after timeout - caller can retry
        /// - Timeout does NOT mark connection as disconnected
        /// - This allows RPC to implement request timeouts without breaking the connection
        /// - Behavior matches TcpStream and IpcStream for consistency
        dp::Res<Message> recv() override {
            if (!connected_) {
                echo::trace("recv called but not connected");
                return dp::result::err(dp::Error::not_found("not connected"));
            }

            echo::trace("shm recv waiting");

            auto start_time = std::chrono::steady_clock::now();
            dp::u32 poll_interval_us = POLL_INTERVAL_US; // Start with base interval

            // Read length prefix (4 bytes) with polling and exponential backoff
            dp::Array<dp::u8, 4> length_bytes;
            for (dp::usize i = 0; i < 4; i++) {
                while (true) {
                    auto res = recv_buffer_.pop();
                    if (res.is_ok()) {
                        length_bytes[i] = res.value();
                        poll_interval_us = POLL_INTERVAL_US; // Reset backoff on success
                        break;
                    }

                    // Check timeout (only if timeout is set)
                    if (recv_timeout_ms_ > 0) {
                        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now() - start_time)
                                           .count();
                        if (elapsed >= recv_timeout_ms_) {
                            echo::trace("recv timeout waiting for length byte ", i);
                            // Timeout keeps connection alive - caller can retry
                            return dp::result::err(dp::Error::timeout("recv timeout"));
                        }
                    }

                    // Poll with exponential backoff to reduce CPU usage
                    // Start at 100us, double up to max 10ms
                    std::this_thread::sleep_for(std::chrono::microseconds(poll_interval_us));
                    if (poll_interval_us < 10000) {
                        poll_interval_us = std::min(poll_interval_us * 2, 10000u);
                    }
                }
            }

            dp::u32 length = decode_u32_be(length_bytes.data());
            echo::trace("recv expecting ", length, " bytes");

            // Read payload with polling and exponential backoff
            Message msg(length);
            for (dp::usize i = 0; i < length; i++) {
                while (true) {
                    auto res = recv_buffer_.pop();
                    if (res.is_ok()) {
                        msg[i] = res.value();
                        poll_interval_us = POLL_INTERVAL_US; // Reset backoff on success
                        break;
                    }

                    // Check timeout (only if timeout is set)
                    if (recv_timeout_ms_ > 0) {
                        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now() - start_time)
                                           .count();
                        if (elapsed >= recv_timeout_ms_) {
                            echo::trace("recv timeout waiting for payload byte ", i);
                            // Timeout keeps connection alive - caller can retry
                            return dp::result::err(dp::Error::timeout("recv timeout"));
                        }
                    }

                    // Poll with exponential backoff to reduce CPU usage
                    std::this_thread::sleep_for(std::chrono::microseconds(poll_interval_us));
                    if (poll_interval_us < 10000) {
                        poll_interval_us = std::min(poll_interval_us * 2, 10000u);
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

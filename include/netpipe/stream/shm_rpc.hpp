#pragma once

#include <atomic>
#include <chrono>
#include <datapod/pods/lockfree/ring_buffer.hpp>
#include <netpipe/stream.hpp>
#include <thread>

namespace netpipe {

    /// Bidirectional shared memory stream for RPC
    /// Uses two SPSC ring buffers: one for sending, one for receiving
    /// Supports blocking recv with timeout via polling
    ///
    /// Usage pattern:
    ///   Server: ShmRpcStream::create_server("channel", size) -> creates both buffers
    ///   Client: ShmRpcStream::create_client("channel", size) -> attaches to both buffers
    ///
    /// The naming convention:
    ///   - Server writes to "channel_s2c" (server-to-client), reads from "channel_c2s"
    ///   - Client writes to "channel_c2s" (client-to-server), reads from "channel_s2c"
    class ShmRpcStream : public Stream {
      private:
        dp::RingBuffer<dp::SPSC, dp::u8> send_buffer_;
        dp::RingBuffer<dp::SPSC, dp::u8> recv_buffer_;
        bool connected_;
        bool is_server_;
        dp::String channel_name_;
        dp::usize buffer_size_;
        dp::u32 recv_timeout_ms_;
        static constexpr dp::u32 POLL_INTERVAL_US = 100; // Polling interval in microseconds

        ShmRpcStream() : connected_(false), is_server_(false), recv_timeout_ms_(0) {
            echo::trace("ShmRpcStream constructed");
        }

      public:
        ~ShmRpcStream() override {
            if (connected_) {
                close();
            }
        }

        /// Create server-side SHM RPC stream
        /// Creates both ring buffers for bidirectional communication
        static dp::Res<std::unique_ptr<ShmRpcStream>> create_server(const dp::String &channel_name,
                                                                    dp::usize buffer_size = 65536) {
            auto stream = std::unique_ptr<ShmRpcStream>(new ShmRpcStream());
            stream->is_server_ = true;
            stream->channel_name_ = channel_name;
            stream->buffer_size_ = buffer_size;

            // Server writes to s2c, reads from c2s
            char s2c_name[256];
            char c2s_name[256];
            snprintf(s2c_name, sizeof(s2c_name), "/%s_s2c", channel_name.c_str());
            snprintf(c2s_name, sizeof(c2s_name), "/%s_c2s", channel_name.c_str());

            // Clean up any existing shared memory
            ::shm_unlink(s2c_name);
            ::shm_unlink(c2s_name);

            // Create send buffer (server-to-client)
            auto send_res = dp::RingBuffer<dp::SPSC, dp::u8>::create_shm(dp::String(s2c_name), buffer_size);
            if (send_res.is_err()) {
                echo::error("failed to create s2c ring buffer");
                return dp::result::err(dp::Error::io_error("failed to create s2c buffer"));
            }
            stream->send_buffer_ = std::move(send_res.value());

            // Create recv buffer (client-to-server)
            auto recv_res = dp::RingBuffer<dp::SPSC, dp::u8>::create_shm(dp::String(c2s_name), buffer_size);
            if (recv_res.is_err()) {
                echo::error("failed to create c2s ring buffer");
                return dp::result::err(dp::Error::io_error("failed to create c2s buffer"));
            }
            stream->recv_buffer_ = std::move(recv_res.value());

            stream->connected_ = true;
            echo::info("ShmRpcStream server created on channel: ", channel_name.c_str());

            return dp::result::ok(std::move(stream));
        }

        /// Create client-side SHM RPC stream
        /// Attaches to existing ring buffers created by server
        static dp::Res<std::unique_ptr<ShmRpcStream>> create_client(const dp::String &channel_name,
                                                                    dp::usize buffer_size = 65536) {
            auto stream = std::unique_ptr<ShmRpcStream>(new ShmRpcStream());
            stream->is_server_ = false;
            stream->channel_name_ = channel_name;
            stream->buffer_size_ = buffer_size;

            // Client writes to c2s, reads from s2c
            char s2c_name[256];
            char c2s_name[256];
            snprintf(s2c_name, sizeof(s2c_name), "/%s_s2c", channel_name.c_str());
            snprintf(c2s_name, sizeof(c2s_name), "/%s_c2s", channel_name.c_str());

            // Attach to send buffer (client-to-server)
            auto send_res = dp::RingBuffer<dp::SPSC, dp::u8>::attach_shm(dp::String(c2s_name));
            if (send_res.is_err()) {
                echo::error("failed to attach to c2s ring buffer");
                return dp::result::err(dp::Error::io_error("failed to attach to c2s buffer"));
            }
            stream->send_buffer_ = std::move(send_res.value());

            // Attach to recv buffer (server-to-client)
            auto recv_res = dp::RingBuffer<dp::SPSC, dp::u8>::attach_shm(dp::String(s2c_name));
            if (recv_res.is_err()) {
                echo::error("failed to attach to s2c ring buffer");
                return dp::result::err(dp::Error::io_error("failed to attach to s2c buffer"));
            }
            stream->recv_buffer_ = std::move(recv_res.value());

            stream->connected_ = true;
            echo::info("ShmRpcStream client connected to channel: ", channel_name.c_str());

            return dp::result::ok(std::move(stream));
        }

        // Stream interface - not used for ShmRpcStream (use static factory methods instead)
        dp::Res<void> connect(const TcpEndpoint &endpoint) override {
            echo::error("use ShmRpcStream::create_client() instead of connect()");
            return dp::result::err(dp::Error::invalid_argument("use create_client()"));
        }

        dp::Res<void> listen(const TcpEndpoint &endpoint) override {
            echo::error("use ShmRpcStream::create_server() instead of listen()");
            return dp::result::err(dp::Error::invalid_argument("use create_server()"));
        }

        dp::Res<std::unique_ptr<Stream>> accept() override {
            echo::error("accept not supported for ShmRpcStream");
            return dp::result::err(dp::Error::invalid_argument("accept not supported"));
        }

        /// Send message with length-prefix framing
        dp::Res<void> send(const Message &msg) override {
            if (!connected_) {
                echo::error("send called but not connected");
                return dp::result::err(dp::Error::not_found("not connected"));
            }

            echo::trace("shm_rpc send ", msg.size(), " bytes");

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
        dp::Res<Message> recv() override {
            if (!connected_) {
                echo::error("recv called but not connected");
                return dp::result::err(dp::Error::not_found("not connected"));
            }

            echo::trace("shm_rpc recv waiting");

            auto start_time = std::chrono::steady_clock::now();

            // Read length prefix (4 bytes) with polling
            dp::Array<dp::u8, 4> length_bytes;
            for (dp::usize i = 0; i < 4; i++) {
                while (true) {
                    auto res = recv_buffer_.pop();
                    if (res.is_ok()) {
                        length_bytes[i] = res.value();
                        break;
                    }

                    // Check timeout
                    if (recv_timeout_ms_ > 0) {
                        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now() - start_time)
                                           .count();
                        if (elapsed >= recv_timeout_ms_) {
                            echo::warn("recv timeout waiting for length byte ", i);
                            return dp::result::err(dp::Error::io_error("recv timeout"));
                        }
                    }

                    // Poll with small sleep to avoid busy-waiting
                    std::this_thread::sleep_for(std::chrono::microseconds(POLL_INTERVAL_US));
                }
            }

            dp::u32 length = decode_u32_be(length_bytes.data());
            echo::trace("recv expecting ", length, " bytes");

            // Read payload with polling
            Message msg(length);
            for (dp::usize i = 0; i < length; i++) {
                while (true) {
                    auto res = recv_buffer_.pop();
                    if (res.is_ok()) {
                        msg[i] = res.value();
                        break;
                    }

                    // Check timeout
                    if (recv_timeout_ms_ > 0) {
                        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now() - start_time)
                                           .count();
                        if (elapsed >= recv_timeout_ms_) {
                            echo::warn("recv timeout waiting for payload byte ", i);
                            return dp::result::err(dp::Error::io_error("recv timeout"));
                        }
                    }

                    std::this_thread::sleep_for(std::chrono::microseconds(POLL_INTERVAL_US));
                }
            }

            echo::debug("received ", length, " bytes");
            return dp::result::ok(std::move(msg));
        }

        /// Set receive timeout in milliseconds
        /// 0 means no timeout (blocking forever)
        dp::Res<void> set_recv_timeout(dp::u32 timeout_ms) override {
            recv_timeout_ms_ = timeout_ms;
            echo::trace("set recv timeout to ", timeout_ms, "ms");
            return dp::result::ok();
        }

        void close() override {
            if (connected_) {
                echo::trace("closing shm_rpc stream");
                connected_ = false;
                echo::debug("ShmRpcStream closed");
            }
        }

        bool is_connected() const override { return connected_; }

        /// Get the channel name
        const dp::String &channel_name() const { return channel_name_; }

        /// Check if this is the server side
        bool is_server() const { return is_server_; }
    };

} // namespace netpipe

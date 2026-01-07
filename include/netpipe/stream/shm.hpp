#pragma once

#include <datapod/pods/lockfree/ring_buffer.hpp>
#include <netpipe/stream.hpp>

namespace netpipe {

    // Shared memory stream implementation using datapod's RingBuffer
    // Ultra-low latency, same machine only
    // Uses datapod's lock-free SPSC ring buffer with u8 elements
    class ShmStream : public Stream {
      private:
        dp::RingBuffer<dp::SPSC, dp::u8> ring_buffer_;
        bool connected_;
        bool is_creator_;
        ShmEndpoint local_endpoint_;

      public:
        ShmStream() : connected_(false), is_creator_(false) { echo::trace("ShmStream constructed"); }

        ~ShmStream() override {
            if (connected_) {
                close();
            }
        }

        // Client side: attach to existing shared memory ring buffer
        dp::Res<void> connect(const TcpEndpoint &endpoint) override {
            ShmEndpoint shm_endpoint{endpoint.host, static_cast<dp::usize>(endpoint.port)};
            return connect_shm(shm_endpoint);
        }

        dp::Res<void> connect_shm(const ShmEndpoint &endpoint) {
            echo::trace("connecting to shm ring buffer ", endpoint.to_string());

            // Attach to existing shared memory ring buffer
            // datapod RingBuffer expects name to start with '/'
            char shm_name_buf[256];
            snprintf(shm_name_buf, sizeof(shm_name_buf), "/%s", endpoint.name.c_str());
            dp::String shm_name(shm_name_buf);

            auto result = dp::RingBuffer<dp::SPSC, dp::u8>::attach_shm(shm_name);
            if (result.is_err()) {
                echo::error("failed to attach to ring buffer");
                return dp::result::err(dp::Error::io_error("attach failed"));
            }

            ring_buffer_ = std::move(result.value());
            connected_ = true;
            is_creator_ = false;
            local_endpoint_ = endpoint;

            echo::debug("ShmStream connected to ", endpoint.to_string());
            echo::info("ShmStream connected to ", endpoint.name.c_str());

            return dp::result::ok();
        }

        // Server side: create shared memory ring buffer
        dp::Res<void> listen(const TcpEndpoint &endpoint) override {
            ShmEndpoint shm_endpoint{endpoint.host, static_cast<dp::usize>(endpoint.port)};
            return listen_shm(shm_endpoint);
        }

        dp::Res<void> listen_shm(const ShmEndpoint &endpoint) {
            echo::trace("creating shm ring buffer ", endpoint.to_string());

            // datapod RingBuffer expects name to start with '/'
            char shm_name_buf[256];
            snprintf(shm_name_buf, sizeof(shm_name_buf), "/%s", endpoint.name.c_str());
            dp::String shm_name(shm_name_buf);

            // Try to unlink any existing shared memory first (cleanup from previous runs)
            ::shm_unlink(shm_name.c_str());

            // Create shared memory ring buffer
            // Size is the capacity in bytes
            auto result = dp::RingBuffer<dp::SPSC, dp::u8>::create_shm(shm_name, endpoint.size);
            if (result.is_err()) {
                echo::error("failed to create ring buffer");
                return dp::result::err(dp::Error::io_error("create failed"));
            }

            ring_buffer_ = std::move(result.value());
            connected_ = true;
            is_creator_ = true;
            local_endpoint_ = endpoint;

            echo::debug("ShmStream created ", endpoint.to_string());
            echo::info("ShmStream created ", endpoint.name.c_str(), " size=", endpoint.size);

            return dp::result::ok();
        }

        // Accept not applicable for SHM (1:1 connection)
        dp::Res<std::unique_ptr<Stream>> accept() override {
            echo::error("accept not supported for ShmStream");
            return dp::result::err(dp::Error::invalid_argument("accept not supported"));
        }

        // Send message to ring buffer with length-prefix framing
        dp::Res<void> send(const Message &msg) override {
            if (!connected_) {
                echo::error("send called but not connected");
                return dp::result::err(dp::Error::not_found("not connected"));
            }

            echo::trace("shm send ", msg.size(), " bytes");

            // Encode length prefix (4 bytes big-endian)
            auto length_bytes = encode_u32_be(static_cast<dp::u32>(msg.size()));

            // Push length prefix bytes
            for (dp::usize i = 0; i < 4; i++) {
                auto res = ring_buffer_.push(length_bytes[i]);
                if (res.is_err()) {
                    echo::warn("ring buffer full, cannot send length");
                    return dp::result::err(dp::Error::io_error("ring buffer full"));
                }
            }

            // Push payload bytes
            for (dp::usize i = 0; i < msg.size(); i++) {
                auto res = ring_buffer_.push(msg[i]);
                if (res.is_err()) {
                    echo::warn("ring buffer full, cannot send payload");
                    // Note: This is problematic - we've already sent the length!
                    // In production, you'd want to check space before starting
                    return dp::result::err(dp::Error::io_error("ring buffer full"));
                }
            }

            echo::debug("sent ", msg.size(), " bytes");
            return dp::result::ok();
        }

        // Receive message from ring buffer with length-prefix framing
        dp::Res<Message> recv() override {
            if (!connected_) {
                echo::error("recv called but not connected");
                return dp::result::err(dp::Error::not_found("not connected"));
            }

            echo::trace("shm recv waiting");

            // Read length prefix (4 bytes)
            dp::Array<dp::u8, 4> length_bytes;
            for (dp::usize i = 0; i < 4; i++) {
                auto res = ring_buffer_.pop();
                if (res.is_err()) {
                    // No data available
                    return dp::result::err(dp::Error::io_error("no data available"));
                }
                length_bytes[i] = res.value();
            }

            dp::u32 length = decode_u32_be(length_bytes.data());
            echo::trace("recv expecting ", length, " bytes");

            // Read payload
            Message msg(length);
            for (dp::usize i = 0; i < length; i++) {
                auto res = ring_buffer_.pop();
                if (res.is_err()) {
                    echo::error("incomplete message in ring buffer");
                    return dp::result::err(dp::Error::io_error("incomplete message"));
                }
                msg[i] = res.value();
            }

            echo::debug("received ", length, " bytes");
            return dp::result::ok(std::move(msg));
        }

        // Set receive timeout in milliseconds
        // Note: ShmStream uses lock-free ring buffer, timeout not directly supported
        // This is a no-op for compatibility with Stream interface
        dp::Res<void> set_recv_timeout(dp::u32 timeout_ms) override {
            echo::warn("set_recv_timeout not supported for ShmStream (lock-free ring buffer)");
            // Return ok to maintain compatibility, but timeout won't be enforced
            return dp::result::ok();
        }

        // Close and cleanup
        void close() override {
            if (connected_) {
                echo::trace("closing shm ring buffer");
                // RingBuffer destructor handles cleanup
                connected_ = false;
                is_creator_ = false;
                echo::debug("ShmStream closed");
            }
        }

        // Check if connected
        bool is_connected() const override { return connected_; }
    };

} // namespace netpipe

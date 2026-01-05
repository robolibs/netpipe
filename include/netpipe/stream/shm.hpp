#pragma once

#include <netpipe/stream.hpp>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <datapod/pods/lockfree/ring_buffer.hpp>

namespace netpipe {

    // Shared memory stream implementation using POSIX shared memory
    // Ultra-low latency, same machine only
    // Uses datapod's lock-free SPSC ring buffer
    class ShmStream : public Stream {
      private:
        dp::i32 shm_fd_;
        void *shm_ptr_;
        dp::usize shm_size_;
        bool connected_;
        bool is_creator_; // Track if we created the shm (for cleanup)
        ShmEndpoint local_endpoint_;

        // Ring buffer for messages
        // Note: datapod's RingBuffer requires trivially copyable types
        // We'll use a simpler approach: store length-prefixed messages directly in shared memory

        struct ShmHeader {
            std::atomic<dp::u64> write_pos;
            std::atomic<dp::u64> read_pos;
            dp::u64 buffer_size;
        };

        ShmHeader *header_;
        dp::u8 *buffer_;

      public:
        ShmStream()
            : shm_fd_(-1), shm_ptr_(nullptr), shm_size_(0), connected_(false), is_creator_(false), header_(nullptr),
              buffer_(nullptr) {
            echo::trace("ShmStream constructed");
        }

        ~ShmStream() override {
            if (shm_fd_ >= 0) {
                close();
            }
        }

        // Client side: connect to existing shared memory
        dp::Res<void> connect(const TcpEndpoint &endpoint) override {
            // ShmStream uses ShmEndpoint, but interface requires TcpEndpoint
            // Treat endpoint.host as the shm name and port as size
            ShmEndpoint shm_endpoint{endpoint.host, static_cast<dp::usize>(endpoint.port)};
            return connect_shm(shm_endpoint);
        }

        dp::Res<void> connect_shm(const ShmEndpoint &endpoint) {
            echo::trace("connecting to shm ", endpoint.to_string());

            // Open existing shared memory
            shm_fd_ = ::shm_open(endpoint.name.c_str(), O_RDWR, 0666);
            if (shm_fd_ < 0) {
                echo::error("shm_open failed: ", strerror(errno));
                return dp::result::err(dp::Error::io_error(dp::String("shm_open failed: ") + strerror(errno)));
            }
            echo::trace("shm_open ", endpoint.name.c_str(), " fd=", shm_fd_);

            // Get size
            struct stat st;
            if (::fstat(shm_fd_, &st) < 0) {
                ::close(shm_fd_);
                shm_fd_ = -1;
                echo::error("fstat failed: ", strerror(errno));
                return dp::result::err(dp::Error::io_error(dp::String("fstat failed: ") + strerror(errno)));
            }
            shm_size_ = static_cast<dp::usize>(st.st_size);

            // Map to memory
            shm_ptr_ = ::mmap(nullptr, shm_size_, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0);
            if (shm_ptr_ == MAP_FAILED) {
                ::close(shm_fd_);
                shm_fd_ = -1;
                echo::error("mmap failed: ", strerror(errno));
                return dp::result::err(dp::Error::io_error(dp::String("mmap failed: ") + strerror(errno)));
            }
            echo::trace("mmap size=", shm_size_, " addr=", shm_ptr_);

            // Setup header and buffer pointers
            header_ = static_cast<ShmHeader *>(shm_ptr_);
            buffer_ = static_cast<dp::u8 *>(shm_ptr_) + sizeof(ShmHeader);

            connected_ = true;
            is_creator_ = false;
            local_endpoint_ = endpoint;

            echo::debug("ShmStream connected to ", endpoint.to_string());
            echo::info("ShmStream connected to ", endpoint.name.c_str());

            return dp::result::ok();
        }

        // Server side: create shared memory
        dp::Res<void> listen(const TcpEndpoint &endpoint) override {
            // ShmStream uses ShmEndpoint
            ShmEndpoint shm_endpoint{endpoint.host, static_cast<dp::usize>(endpoint.port)};
            return listen_shm(shm_endpoint);
        }

        dp::Res<void> listen_shm(const ShmEndpoint &endpoint) {
            echo::trace("creating shm ", endpoint.to_string());

            // Unlink existing (cleanup)
            ::shm_unlink(endpoint.name.c_str());

            // Create shared memory
            shm_fd_ = ::shm_open(endpoint.name.c_str(), O_CREAT | O_RDWR, 0666);
            if (shm_fd_ < 0) {
                echo::error("shm_open failed: ", strerror(errno));
                return dp::result::err(dp::Error::io_error(dp::String("shm_open failed: ") + strerror(errno)));
            }
            echo::trace("shm_open ", endpoint.name.c_str(), " fd=", shm_fd_);

            // Set size
            shm_size_ = endpoint.size;
            if (::ftruncate(shm_fd_, static_cast<off_t>(shm_size_)) < 0) {
                ::close(shm_fd_);
                ::shm_unlink(endpoint.name.c_str());
                shm_fd_ = -1;
                echo::error("ftruncate failed: ", strerror(errno));
                return dp::result::err(dp::Error::io_error(dp::String("ftruncate failed: ") + strerror(errno)));
            }

            // Map to memory
            shm_ptr_ = ::mmap(nullptr, shm_size_, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0);
            if (shm_ptr_ == MAP_FAILED) {
                ::close(shm_fd_);
                ::shm_unlink(endpoint.name.c_str());
                shm_fd_ = -1;
                echo::error("mmap failed: ", strerror(errno));
                return dp::result::err(dp::Error::io_error(dp::String("mmap failed: ") + strerror(errno)));
            }
            echo::trace("mmap size=", shm_size_, " addr=", shm_ptr_);

            // Initialize header
            header_ = static_cast<ShmHeader *>(shm_ptr_);
            buffer_ = static_cast<dp::u8 *>(shm_ptr_) + sizeof(ShmHeader);

            header_->write_pos.store(0, std::memory_order_release);
            header_->read_pos.store(0, std::memory_order_release);
            header_->buffer_size = shm_size_ - sizeof(ShmHeader);

            connected_ = true;
            is_creator_ = true;
            local_endpoint_ = endpoint;

            echo::debug("ShmStream created ", endpoint.to_string());
            echo::info("ShmStream created ", endpoint.name.c_str(), " size=", shm_size_);

            return dp::result::ok();
        }

        // Accept not really applicable for SHM (1:1 connection)
        dp::Res<std::unique_ptr<Stream>> accept() override {
            echo::error("accept not supported for ShmStream");
            return dp::result::err(dp::Error::invalid_argument("accept not supported"));
        }

        // Send message to ring buffer
        dp::Res<void> send(const Message &msg) override {
            if (!connected_) {
                echo::error("send called but not connected");
                return dp::result::err(dp::Error::not_found("not connected"));
            }

            echo::trace("shm send ", msg.size(), " bytes");

            // Calculate total size needed (4-byte length + payload)
            dp::usize total_size = 4 + msg.size();

            // Check if there's space
            dp::u64 write_pos = header_->write_pos.load(std::memory_order_acquire);
            dp::u64 read_pos = header_->read_pos.load(std::memory_order_acquire);
            dp::u64 available = header_->buffer_size - (write_pos - read_pos);

            if (available < total_size) {
                echo::warn("ring buffer full, blocking");
                // In a real implementation, we'd block/retry here
                return dp::result::err(dp::Error::io_error("ring buffer full"));
            }

            // Write length prefix
            dp::u32 length = static_cast<dp::u32>(msg.size());
            auto length_bytes = encode_u32_be(length);
            for (dp::usize i = 0; i < 4; i++) {
                buffer_[(write_pos + i) % header_->buffer_size] = length_bytes[i];
            }
            write_pos += 4;

            // Write payload
            for (dp::usize i = 0; i < msg.size(); i++) {
                buffer_[(write_pos + i) % header_->buffer_size] = msg[i];
            }
            write_pos += msg.size();

            // Update write position
            header_->write_pos.store(write_pos, std::memory_order_release);

            echo::trace("ring write head=", write_pos, " len=", msg.size());
            echo::debug("sent ", msg.size(), " bytes");

            return dp::result::ok();
        }

        // Receive message from ring buffer
        dp::Res<Message> recv() override {
            if (!connected_) {
                echo::error("recv called but not connected");
                return dp::result::err(dp::Error::not_found("not connected"));
            }

            echo::trace("shm recv waiting");

            // Check if there's data
            dp::u64 write_pos = header_->write_pos.load(std::memory_order_acquire);
            dp::u64 read_pos = header_->read_pos.load(std::memory_order_acquire);

            if (write_pos == read_pos) {
                // No data available - in real implementation, we'd block/retry
                return dp::result::err(dp::Error::io_error("no data available"));
            }

            // Read length prefix
            dp::Array<dp::u8, 4> length_bytes;
            for (dp::usize i = 0; i < 4; i++) {
                length_bytes[i] = buffer_[(read_pos + i) % header_->buffer_size];
            }
            read_pos += 4;

            dp::u32 length = decode_u32_be(length_bytes.data());
            echo::trace("ring read tail=", read_pos, " len=", length);

            // Read payload
            Message msg(length);
            for (dp::usize i = 0; i < length; i++) {
                msg[i] = buffer_[(read_pos + i) % header_->buffer_size];
            }
            read_pos += length;

            // Update read position
            header_->read_pos.store(read_pos, std::memory_order_release);

            echo::debug("received ", length, " bytes");

            return dp::result::ok(std::move(msg));
        }

        // Close and cleanup
        void close() override {
            if (shm_ptr_ != nullptr) {
                echo::trace("munmap addr=", shm_ptr_);
                ::munmap(shm_ptr_, shm_size_);
                shm_ptr_ = nullptr;
                header_ = nullptr;
                buffer_ = nullptr;
            }

            if (shm_fd_ >= 0) {
                echo::trace("closing shm fd=", shm_fd_);
                ::close(shm_fd_);

                // Unlink if we created it
                if (is_creator_) {
                    if (::shm_unlink(local_endpoint_.name.c_str()) < 0) {
                        echo::warn("shm_unlink failed: ", strerror(errno));
                    } else {
                        echo::trace("unlinked shm ", local_endpoint_.name.c_str());
                    }
                }

                shm_fd_ = -1;
                connected_ = false;
                is_creator_ = false;
                echo::debug("ShmStream closed");
            }
        }

        // Check if connected
        bool is_connected() const override { return connected_; }
    };

} // namespace netpipe

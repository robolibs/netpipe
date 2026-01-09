#pragma once

#include <atomic>
#include <chrono>
#include <cstring>
#include <datapod/pods/lockfree/ring_buffer.hpp>
#include <fcntl.h>
#include <mutex>
#include <netpipe/remote/protocol.hpp>
#include <netpipe/stream.hpp>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

namespace netpipe {

    /// Connection request structure for SHM handshake
    struct ShmConnRequest {
        dp::u64 client_id;
        dp::u32 buffer_size;
        dp::u32 reserved;
    };

    /// Connection response
    struct ShmConnResponse {
        dp::u64 client_id;
        dp::u64 conn_id;
        dp::u32 status;
        dp::u32 reserved;
    };

    /// Fast shared memory message buffer header
    /// Uses bulk memcpy instead of byte-by-byte operations
    struct alignas(64) ShmMsgHeader {
        std::atomic<dp::u64> msg_size;  // Size of current message (0 = no message)
        std::atomic<dp::u32> msg_ready; // 1 = message ready to read, 0 = buffer free
        dp::u32 capacity;               // Max message size
        dp::u8 padding[48];             // Pad to cache line
    };

    /// Bidirectional shared memory stream with TCP-like connection semantics
    /// Now uses bulk memcpy for high-performance data transfer
    class ShmStream : public Stream {
      private:
        // Fast message buffers (raw shared memory with bulk memcpy)
        void *send_shm_ptr_;
        void *recv_shm_ptr_;
        dp::usize send_shm_size_;
        dp::usize recv_shm_size_;
        int send_shm_fd_;
        int recv_shm_fd_;

        // Connection queue (still uses ring buffer for small handshake messages)
        dp::RingBuffer<dp::MPMC, dp::u8> conn_queue_;
        dp::RingBuffer<dp::MPMC, dp::u8> resp_queue_;
        std::atomic<dp::u64> next_conn_id_{0};

        bool connected_;
        bool listening_;
        bool is_server_;
        bool owns_shm_;
        dp::String channel_name_;
        dp::u64 conn_id_;
        dp::usize buffer_size_;
        dp::u32 recv_timeout_ms_;

        static constexpr dp::u32 POLL_INTERVAL_US = 1;       // Start with 1us
        static constexpr dp::u32 MAX_POLL_INTERVAL_US = 100; // Max 100us backoff
        static constexpr dp::usize CONN_QUEUE_SIZE = 4096;

        mutable std::mutex send_mutex_;

        /// Get header and data pointers from shm region
        static ShmMsgHeader *get_header(void *shm_ptr) { return static_cast<ShmMsgHeader *>(shm_ptr); }

        static dp::u8 *get_data(void *shm_ptr) { return static_cast<dp::u8 *>(shm_ptr) + sizeof(ShmMsgHeader); }

        /// Create a shared memory message buffer
        static bool create_msg_buffer(const char *name, dp::usize capacity, void *&ptr, dp::usize &size, int &fd) {
            // Total size = header + data
            size = sizeof(ShmMsgHeader) + capacity;

            fd = ::shm_open(name, O_CREAT | O_RDWR | O_EXCL, 0666);
            if (fd < 0) {
                // Try to unlink and recreate
                ::shm_unlink(name);
                fd = ::shm_open(name, O_CREAT | O_RDWR | O_EXCL, 0666);
                if (fd < 0) {
                    echo::error("shm_open failed: ", name, " - ", strerror(errno));
                    return false;
                }
            }

            if (::ftruncate(fd, static_cast<off_t>(size)) < 0) {
                echo::error("ftruncate failed: ", strerror(errno));
                ::close(fd);
                ::shm_unlink(name);
                return false;
            }

            ptr = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (ptr == MAP_FAILED) {
                echo::error("mmap failed: ", strerror(errno));
                ::close(fd);
                ::shm_unlink(name);
                return false;
            }

            // Initialize header
            auto *header = get_header(ptr);
            new (&header->msg_size) std::atomic<dp::u64>(0);
            new (&header->msg_ready) std::atomic<dp::u32>(0);
            header->capacity = static_cast<dp::u32>(capacity);

            return true;
        }

        /// Attach to existing shared memory message buffer
        static bool attach_msg_buffer(const char *name, void *&ptr, dp::usize &size, int &fd) {
            fd = ::shm_open(name, O_RDWR, 0666);
            if (fd < 0) {
                echo::error("shm_open attach failed: ", name, " - ", strerror(errno));
                return false;
            }

            // Get actual size
            struct stat st;
            if (::fstat(fd, &st) < 0) {
                echo::error("fstat failed: ", strerror(errno));
                ::close(fd);
                return false;
            }
            size = static_cast<dp::usize>(st.st_size);

            ptr = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (ptr == MAP_FAILED) {
                echo::error("mmap attach failed: ", strerror(errno));
                ::close(fd);
                return false;
            }

            return true;
        }

        /// Close a shared memory buffer
        static void close_msg_buffer(void *ptr, dp::usize size, int fd, const char *name, bool unlink) {
            if (ptr && ptr != MAP_FAILED) {
                ::munmap(ptr, size);
            }
            if (fd >= 0) {
                ::close(fd);
            }
            if (unlink && name) {
                ::shm_unlink(name);
            }
        }

        /// Private constructor for accepted connections
        ShmStream(void *send_ptr, dp::usize send_size, int send_fd, void *recv_ptr, dp::usize recv_size, int recv_fd,
                  const dp::String &channel_name, dp::u64 conn_id, dp::usize buffer_size)
            : send_shm_ptr_(send_ptr), recv_shm_ptr_(recv_ptr), send_shm_size_(send_size), recv_shm_size_(recv_size),
              send_shm_fd_(send_fd), recv_shm_fd_(recv_fd), connected_(true), listening_(false), is_server_(true),
              owns_shm_(true), channel_name_(channel_name), conn_id_(conn_id), buffer_size_(buffer_size),
              recv_timeout_ms_(0) {
            echo::debug("ShmStream created for connection ", conn_id);
        }

        static dp::u64 generate_client_id() {
            auto now = std::chrono::steady_clock::now().time_since_epoch();
            auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
            return static_cast<dp::u64>(::getpid()) ^ static_cast<dp::u64>(ns);
        }

        /// Write a struct to ring buffer (for handshake only)
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

        /// Read a struct from ring buffer (for handshake only)
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
            : send_shm_ptr_(nullptr), recv_shm_ptr_(nullptr), send_shm_size_(0), recv_shm_size_(0), send_shm_fd_(-1),
              recv_shm_fd_(-1), connected_(false), listening_(false), is_server_(false), owns_shm_(false), conn_id_(0),
              buffer_size_(0), recv_timeout_ms_(0) {
            echo::trace("ShmStream constructed");
        }

        ~ShmStream() override {
            if (connected_ || listening_) {
                close();
            }
        }

        ShmStream(const ShmStream &) = delete;
        ShmStream &operator=(const ShmStream &) = delete;

        ShmStream(ShmStream &&other) noexcept
            : send_shm_ptr_(other.send_shm_ptr_), recv_shm_ptr_(other.recv_shm_ptr_),
              send_shm_size_(other.send_shm_size_), recv_shm_size_(other.recv_shm_size_),
              send_shm_fd_(other.send_shm_fd_), recv_shm_fd_(other.recv_shm_fd_),
              conn_queue_(std::move(other.conn_queue_)), resp_queue_(std::move(other.resp_queue_)),
              connected_(other.connected_), listening_(other.listening_), is_server_(other.is_server_),
              owns_shm_(other.owns_shm_), channel_name_(std::move(other.channel_name_)), conn_id_(other.conn_id_),
              buffer_size_(other.buffer_size_), recv_timeout_ms_(other.recv_timeout_ms_) {
            next_conn_id_.store(other.next_conn_id_.load());
            other.send_shm_ptr_ = nullptr;
            other.recv_shm_ptr_ = nullptr;
            other.send_shm_fd_ = -1;
            other.recv_shm_fd_ = -1;
            other.connected_ = false;
            other.listening_ = false;
            other.owns_shm_ = false;
        }

        ShmStream &operator=(ShmStream &&other) noexcept {
            if (this != &other) {
                if (connected_ || listening_)
                    close();

                send_shm_ptr_ = other.send_shm_ptr_;
                recv_shm_ptr_ = other.recv_shm_ptr_;
                send_shm_size_ = other.send_shm_size_;
                recv_shm_size_ = other.recv_shm_size_;
                send_shm_fd_ = other.send_shm_fd_;
                recv_shm_fd_ = other.recv_shm_fd_;
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

                other.send_shm_ptr_ = nullptr;
                other.recv_shm_ptr_ = nullptr;
                other.send_shm_fd_ = -1;
                other.recv_shm_fd_ = -1;
                other.connected_ = false;
                other.listening_ = false;
                other.owns_shm_ = false;
            }
            return *this;
        }

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

            char connq_name[256];
            char respq_name[256];
            snprintf(connq_name, sizeof(connq_name), "/%s_connq", endpoint.name.c_str());
            snprintf(respq_name, sizeof(respq_name), "/%s_respq", endpoint.name.c_str());

            ::shm_unlink(connq_name);
            ::shm_unlink(respq_name);

            auto connq_res = dp::RingBuffer<dp::MPMC, dp::u8>::create_shm(dp::String(connq_name), CONN_QUEUE_SIZE);
            if (connq_res.is_err()) {
                echo::error("failed to create connection queue");
                return dp::result::err(dp::Error::io_error("failed to create connection queue"));
            }
            conn_queue_ = std::move(connq_res.value());

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

        dp::Res<std::unique_ptr<Stream>> accept() override {
            if (!listening_) {
                echo::error("accept called but not listening");
                return dp::result::err(dp::Error::invalid_argument("not listening"));
            }

            echo::trace("ShmStream waiting for connection");

            ShmConnRequest req;
            if (!read_struct(conn_queue_, req, recv_timeout_ms_)) {
                return dp::result::err(dp::Error::timeout("accept timeout"));
            }

            echo::debug("received connection request from client ", req.client_id);

            dp::u64 conn_id = next_conn_id_.fetch_add(1);

            char s2c_name[256];
            char c2s_name[256];
            snprintf(s2c_name, sizeof(s2c_name), "/%s_%llu_s2c", channel_name_.c_str(),
                     static_cast<unsigned long long>(conn_id));
            snprintf(c2s_name, sizeof(c2s_name), "/%s_%llu_c2s", channel_name_.c_str(),
                     static_cast<unsigned long long>(conn_id));

            ::shm_unlink(s2c_name);
            ::shm_unlink(c2s_name);

            dp::usize buf_size = req.buffer_size > 0 ? req.buffer_size : buffer_size_;

            // Create fast message buffers
            void *s2c_ptr = nullptr;
            void *c2s_ptr = nullptr;
            dp::usize s2c_size = 0, c2s_size = 0;
            int s2c_fd = -1, c2s_fd = -1;

            if (!create_msg_buffer(s2c_name, buf_size, s2c_ptr, s2c_size, s2c_fd)) {
                echo::error("failed to create s2c buffer for conn ", conn_id);
                ShmConnResponse resp{req.client_id, conn_id, 1, 0};
                write_struct(resp_queue_, resp, 1000);
                return dp::result::err(dp::Error::io_error("failed to create connection buffers"));
            }

            if (!create_msg_buffer(c2s_name, buf_size, c2s_ptr, c2s_size, c2s_fd)) {
                echo::error("failed to create c2s buffer for conn ", conn_id);
                close_msg_buffer(s2c_ptr, s2c_size, s2c_fd, s2c_name, true);
                ShmConnResponse resp{req.client_id, conn_id, 2, 0};
                write_struct(resp_queue_, resp, 1000);
                return dp::result::err(dp::Error::io_error("failed to create connection buffers"));
            }

            ShmConnResponse resp{req.client_id, conn_id, 0, 0};
            if (!write_struct(resp_queue_, resp, 5000)) {
                echo::error("failed to send connection response");
                close_msg_buffer(s2c_ptr, s2c_size, s2c_fd, s2c_name, true);
                close_msg_buffer(c2s_ptr, c2s_size, c2s_fd, c2s_name, true);
                return dp::result::err(dp::Error::io_error("failed to send connection response"));
            }

            echo::info("ShmStream accepted connection ", conn_id, " from client ", req.client_id);

            // Server sends on s2c, receives on c2s
            auto client_stream = std::unique_ptr<Stream>(
                new ShmStream(s2c_ptr, s2c_size, s2c_fd, c2s_ptr, c2s_size, c2s_fd, channel_name_, conn_id, buf_size));

            return dp::result::ok(std::move(client_stream));
        }

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

            dp::u64 client_id = generate_client_id();
            ShmConnRequest req{client_id, static_cast<dp::u32>(endpoint.size), 0};

            echo::debug("sending connection request, client_id=", client_id);

            if (!write_struct(conn_queue_, req, 5000)) {
                echo::error("timeout sending connection request");
                return dp::result::err(dp::Error::timeout("connection timeout"));
            }

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

                        char s2c_name[256];
                        char c2s_name[256];
                        snprintf(s2c_name, sizeof(s2c_name), "/%s_%llu_s2c", channel_name_.c_str(),
                                 static_cast<unsigned long long>(conn_id_));
                        snprintf(c2s_name, sizeof(c2s_name), "/%s_%llu_c2s", channel_name_.c_str(),
                                 static_cast<unsigned long long>(conn_id_));

                        // Client sends on c2s, receives on s2c
                        if (!attach_msg_buffer(c2s_name, send_shm_ptr_, send_shm_size_, send_shm_fd_)) {
                            echo::error("failed to attach to c2s buffer");
                            return dp::result::err(dp::Error::io_error("failed to attach to buffers"));
                        }

                        if (!attach_msg_buffer(s2c_name, recv_shm_ptr_, recv_shm_size_, recv_shm_fd_)) {
                            echo::error("failed to attach to s2c buffer");
                            close_msg_buffer(send_shm_ptr_, send_shm_size_, send_shm_fd_, nullptr, false);
                            return dp::result::err(dp::Error::io_error("failed to attach to buffers"));
                        }

                        connected_ = true;
                        echo::info("ShmStream connected, conn_id=", conn_id_);
                        return dp::result::ok();
                    }
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

        /// Send message using bulk memcpy - FAST!
        dp::Res<void> send(const Message &msg) override {
            std::lock_guard<std::mutex> lock(send_mutex_);

            if (!connected_ || !send_shm_ptr_) {
                echo::trace("send called but not connected");
                return dp::result::err(dp::Error::not_found("not connected"));
            }

            auto *header = get_header(send_shm_ptr_);
            dp::u8 *data = get_data(send_shm_ptr_);

            if (msg.size() > header->capacity) {
                echo::error("message too large: ", msg.size(), " bytes (max ", header->capacity, ")");
                return dp::result::err(dp::Error::invalid_argument("message exceeds maximum size"));
            }

            echo::trace("shm send ", msg.size(), " bytes");

            // Wait for buffer to be free (msg_ready == 0)
            auto start_time = std::chrono::steady_clock::now();
            constexpr dp::u32 MAX_WAIT_MS = 30000;
            dp::u32 poll_us = POLL_INTERVAL_US;

            while (header->msg_ready.load(std::memory_order_acquire) != 0) {
                auto elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time)
                        .count();
                if (elapsed >= MAX_WAIT_MS) {
                    echo::error("send timeout waiting for buffer");
                    return dp::result::err(dp::Error::timeout("send buffer full"));
                }

                if (poll_us < MAX_POLL_INTERVAL_US) {
                    std::this_thread::sleep_for(std::chrono::microseconds(poll_us));
                    poll_us = std::min(poll_us * 2, MAX_POLL_INTERVAL_US);
                } else {
                    std::this_thread::yield();
                }
            }

            // Bulk copy message data
            std::memcpy(data, msg.data(), msg.size());

            // Set size and signal ready (with memory barriers)
            header->msg_size.store(msg.size(), std::memory_order_release);
            header->msg_ready.store(1, std::memory_order_release);

            echo::debug("sent ", msg.size(), " bytes");
            return dp::result::ok();
        }

        /// Receive message using bulk memcpy - FAST!
        dp::Res<Message> recv() override {
            if (!connected_ || !recv_shm_ptr_) {
                echo::trace("recv called but not connected");
                return dp::result::err(dp::Error::not_found("not connected"));
            }

            echo::trace("shm recv waiting");

            auto *header = get_header(recv_shm_ptr_);
            dp::u8 *data = get_data(recv_shm_ptr_);

            auto start_time = std::chrono::steady_clock::now();
            dp::u32 poll_us = POLL_INTERVAL_US;

            // Wait for message to be ready (msg_ready == 1)
            while (header->msg_ready.load(std::memory_order_acquire) == 0) {
                if (recv_timeout_ms_ > 0) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - start_time)
                                       .count();
                    if (elapsed >= recv_timeout_ms_) {
                        return dp::result::err(dp::Error::timeout("recv timeout"));
                    }
                }

                if (poll_us < MAX_POLL_INTERVAL_US) {
                    std::this_thread::sleep_for(std::chrono::microseconds(poll_us));
                    poll_us = std::min(poll_us * 2, MAX_POLL_INTERVAL_US);
                } else {
                    std::this_thread::yield();
                }
            }

            // Read size
            dp::u64 length = header->msg_size.load(std::memory_order_acquire);
            echo::trace("recv expecting ", length, " bytes");

            if (length > remote::MAX_MESSAGE_SIZE) {
                echo::error("message too large: ", length, " bytes");
                connected_ = false;
                return dp::result::err(dp::Error::invalid_argument("message exceeds maximum size"));
            }

            // Allocate and bulk copy
            Message msg;
            try {
                msg.resize(static_cast<dp::usize>(length));
            } catch (const std::bad_alloc &) {
                echo::error("allocation failed: ", length, " bytes");
                connected_ = false;
                return dp::result::err(dp::Error::io_error("memory allocation failed"));
            }

            std::memcpy(msg.data(), data, static_cast<dp::usize>(length));

            // Signal buffer is free
            header->msg_ready.store(0, std::memory_order_release);

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

                char s2c_name[256];
                char c2s_name[256];
                snprintf(s2c_name, sizeof(s2c_name), "/%s_%llu_s2c", channel_name_.c_str(),
                         static_cast<unsigned long long>(conn_id_));
                snprintf(c2s_name, sizeof(c2s_name), "/%s_%llu_c2s", channel_name_.c_str(),
                         static_cast<unsigned long long>(conn_id_));

                if (is_server_) {
                    // Server owns s2c (send), client owns c2s
                    close_msg_buffer(send_shm_ptr_, send_shm_size_, send_shm_fd_, owns_shm_ ? s2c_name : nullptr,
                                     owns_shm_);
                    close_msg_buffer(recv_shm_ptr_, recv_shm_size_, recv_shm_fd_, nullptr, false);
                } else {
                    // Client: just detach, don't unlink
                    close_msg_buffer(send_shm_ptr_, send_shm_size_, send_shm_fd_, nullptr, false);
                    close_msg_buffer(recv_shm_ptr_, recv_shm_size_, recv_shm_fd_, nullptr, false);
                }

                send_shm_ptr_ = nullptr;
                recv_shm_ptr_ = nullptr;
                send_shm_fd_ = -1;
                recv_shm_fd_ = -1;
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

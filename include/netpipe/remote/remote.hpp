#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <netpipe/remote/async.hpp>
#include <netpipe/remote/metrics.hpp>
#include <netpipe/remote/protocol.hpp>
#include <netpipe/remote/registry.hpp>
#include <netpipe/stream.hpp>
#include <queue>
#include <thread>
#include <vector>

namespace netpipe {
    namespace remote {

        /// Simple thread pool for handler execution
        /// Fixed-size pool with task queue
        class ThreadPool {
          private:
            std::vector<std::thread> workers_;
            std::queue<std::function<void()>> tasks_;
            mutable std::mutex queue_mutex_;
            std::condition_variable condition_;
            std::atomic<bool> stop_;
            std::atomic<dp::usize> active_tasks_;
            dp::usize max_queue_size_;

          public:
            explicit ThreadPool(dp::usize num_threads = 10, dp::usize max_queue_size = 1000)
                : stop_(false), active_tasks_(0), max_queue_size_(max_queue_size) {
                echo::trace("ThreadPool created with ", num_threads, " threads, max_queue=", max_queue_size);

                for (dp::usize i = 0; i < num_threads; ++i) {
                    workers_.emplace_back([this] {
                        while (true) {
                            std::function<void()> task;
                            {
                                std::unique_lock<std::mutex> lock(queue_mutex_);
                                condition_.wait(lock, [this] { return stop_ || !tasks_.empty(); });

                                if (stop_ && tasks_.empty()) {
                                    return;
                                }

                                if (!tasks_.empty()) {
                                    task = std::move(tasks_.front());
                                    tasks_.pop();
                                }
                            }

                            if (task) {
                                active_tasks_++;
                                task();
                                active_tasks_--;
                            }
                        }
                    });
                }
            }

            ~ThreadPool() { shutdown(); }

            /// Submit a task to the pool
            /// Returns false if queue is full
            bool submit(std::function<void()> task) {
                {
                    std::unique_lock<std::mutex> lock(queue_mutex_);
                    if (tasks_.size() >= max_queue_size_) {
                        echo::warn("ThreadPool queue full (", tasks_.size(), "/", max_queue_size_, ")");
                        return false;
                    }
                    tasks_.push(std::move(task));
                }
                condition_.notify_one();
                return true;
            }

            /// Get number of active tasks
            dp::usize active_count() const { return active_tasks_.load(); }

            /// Get number of queued tasks
            dp::usize queued_count() const {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                return tasks_.size();
            }

            /// Shutdown the pool and wait for all tasks to complete
            void shutdown() {
                if (stop_) {
                    return;
                }

                echo::trace("ThreadPool shutting down, active=", active_tasks_.load(), " queued=", tasks_.size());
                stop_ = true;
                condition_.notify_all();

                for (auto &worker : workers_) {
                    if (worker.joinable()) {
                        worker.join();
                    }
                }
                echo::trace("ThreadPool shutdown complete");
            }
        };

        /// Remote mode tags
        struct Unidirect {};
        struct Bidirect {};

        /// Remote RPC - template specializations for different modes
        template <typename Mode> class Remote;

        /// Remote RPC - Unidirectional mode (client-only or server-only)
        /// Simple synchronous request-response
        template <> class Remote<Unidirect> {
          private:
            Stream &stream_;
            dp::u32 next_request_id_;
            MethodRegistry registry_;

          public:
            explicit Remote(Stream &stream) : stream_(stream), next_request_id_(0) {
                echo::trace("Remote<Unidirect> constructed");
            }

            /// Client side: send request and wait for response
            dp::Res<Message> call(dp::u32 method_id, const Message &request, dp::u32 timeout_ms = 5000) {
                dp::u32 request_id = next_request_id_++;
                echo::trace("remote call id=", request_id, " method=", method_id, " len=", request.size());

                // Set receive timeout
                auto timeout_res = stream_.set_recv_timeout(timeout_ms);
                if (timeout_res.is_err()) {
                    echo::warn("failed to set recv timeout: ", timeout_res.error().message.c_str());
                }

                // Encode request
                Message remote_request = encode_remote_message_v2(request_id, method_id, request, MessageType::Request);

                // Send request
                auto send_res = stream_.send(remote_request);
                if (send_res.is_err()) {
                    echo::error("remote send failed");
                    return dp::result::err(send_res.error());
                }

                // Receive response
                auto recv_res = stream_.recv();
                if (recv_res.is_err()) {
                    echo::error("remote recv failed");
                    return dp::result::err(recv_res.error());
                }

                // Decode response
                auto decode_res = decode_remote_message_v2(recv_res.value());
                if (decode_res.is_err()) {
                    echo::error("remote decode failed");
                    return dp::result::err(decode_res.error());
                }

                auto decoded = decode_res.value();

                // Verify request_id matches
                if (decoded.request_id != request_id) {
                    echo::error("remote request_id mismatch: expected ", request_id, " got ", decoded.request_id);
                    return dp::result::err(dp::Error::invalid_argument("request_id mismatch"));
                }

                // Check if response is an error
                if (decoded.type == MessageType::Error) {
                    echo::error("remote call returned error");
                    dp::String error_msg(reinterpret_cast<const char *>(decoded.payload.data()),
                                         decoded.payload.size());
                    return dp::result::err(dp::Error::io_error(error_msg.c_str()));
                }

                echo::trace("remote response id=", decoded.request_id, " len=", decoded.payload.size());
                return dp::result::ok(std::move(decoded.payload));
            }

            /// Register a handler for a specific method_id
            dp::Res<void> register_method(dp::u32 method_id, Handler handler) {
                return registry_.register_method(method_id, handler);
            }

            /// Unregister a handler
            dp::Res<void> unregister_method(dp::u32 method_id) { return registry_.unregister_method(method_id); }

            /// Set default handler for unknown methods
            void set_default_handler(Handler handler) { registry_.set_default_handler(handler); }

            /// Clear default handler
            void clear_default_handler() { registry_.clear_default_handler(); }

            /// Server side: serve requests using registered handlers
            dp::Res<void> serve() {
                echo::info("remote serve started");

                while (true) {
                    // Receive request
                    auto recv_res = stream_.recv();
                    if (recv_res.is_err()) {
                        echo::error("remote serve recv failed");
                        return dp::result::err(recv_res.error());
                    }

                    // Decode request
                    auto decode_res = decode_remote_message_v2(recv_res.value());
                    if (decode_res.is_err()) {
                        echo::error("remote serve decode failed");
                        return dp::result::err(decode_res.error());
                    }

                    auto decoded = decode_res.value();
                    echo::trace("remote serve handling request id=", decoded.request_id, " method=", decoded.method_id);

                    // Get handler for method_id
                    auto handler_res = registry_.get_handler(decoded.method_id);
                    Message remote_response;

                    if (handler_res.is_err()) {
                        // No handler found - send error response
                        echo::warn("no handler for method_id: ", decoded.method_id);
                        dp::String error_msg = dp::String("No handler for method_id: ") +
                                               dp::String(std::to_string(decoded.method_id).c_str());
                        Message error_payload(error_msg.begin(), error_msg.end());
                        remote_response = encode_remote_message_v2(decoded.request_id, decoded.method_id, error_payload,
                                                                   MessageType::Error);
                    } else {
                        // Call handler
                        auto handler = handler_res.value();
                        auto result = handler(decoded.payload);

                        if (result.is_err()) {
                            // Handler returned error
                            echo::warn("handler returned error: ", result.error().message.c_str());
                            Message error_payload(result.error().message.begin(), result.error().message.end());
                            remote_response = encode_remote_message_v2(decoded.request_id, decoded.method_id,
                                                                       error_payload, MessageType::Error);
                        } else {
                            // Handler succeeded
                            remote_response = encode_remote_message_v2(decoded.request_id, decoded.method_id,
                                                                       result.value(), MessageType::Response);
                        }
                    }

                    // Send response
                    auto send_res = stream_.send(remote_response);
                    if (send_res.is_err()) {
                        echo::error("remote serve send failed");
                        return dp::result::err(send_res.error());
                    }

                    echo::trace("remote serve sent response id=", decoded.request_id);
                }

                return dp::result::ok();
            }

            /// Get number of registered methods
            dp::usize method_count() const { return registry_.method_count(); }
        };

        /// Handler execution info for lifecycle tracking
        struct HandlerInfo {
            dp::u32 request_id;
            dp::u32 method_id;
            std::chrono::steady_clock::time_point start_time;
            std::atomic<bool> completed;

            HandlerInfo(dp::u32 req_id, dp::u32 meth_id)
                : request_id(req_id), method_id(meth_id), start_time(std::chrono::steady_clock::now()),
                  completed(false) {}
        };

        /// Remote RPC - Bidirectional mode (peer-to-peer)
        template <> class Remote<Bidirect> {
          private:
            Stream &stream_;
            std::atomic<dp::u32> next_request_id_;
            MethodRegistry registry_;

            // Request tracking for outgoing calls
            std::map<dp::u32, std::shared_ptr<PendingRequest>> pending_requests_;
            mutable std::mutex pending_mutex_;

            // Mutex to protect stream send operations (recv is only in receiver thread)
            mutable std::mutex send_mutex_;

            std::thread receiver_thread_;
            std::atomic<bool> running_;
            dp::usize max_concurrent_requests_;
            RemoteMetrics client_metrics_; // Metrics for outgoing calls
            RemoteMetrics server_metrics_; // Metrics for incoming requests
            bool enable_metrics_;

            // Thread pool for handler execution
            std::unique_ptr<ThreadPool> handler_pool_;

            // Handler lifecycle tracking
            std::map<dp::u32, std::shared_ptr<HandlerInfo>> active_handlers_;
            mutable std::mutex handlers_mutex_;

            /// Receiver thread - handles both responses (for our calls) and requests (from peer)
            void receiver_loop() {
                echo::debug("remote bidirect receiver thread started");

                while (running_) {
                    // Receive message
                    auto recv_res = stream_.recv();
                    if (recv_res.is_err()) {
                        // Timeout is expected - just continue to check running_ flag
                        if (recv_res.error().code == dp::Error::TIMEOUT) {
                            continue;
                        }
                        // Connection closed or other error - exit gracefully
                        if (running_) {
                            echo::trace("remote bidirect recv failed: ", recv_res.error().message.c_str());
                        }
                        break;
                    }

                    // Decode message
                    auto decode_res = decode_remote_message_v2(recv_res.value());
                    if (decode_res.is_err()) {
                        echo::warn("remote bidirect decode failed");
                        continue;
                    }

                    auto decoded = decode_res.value();

                    // Determine message type
                    if (decoded.type == MessageType::Request) {
                        // Incoming request - submit to thread pool to avoid blocking receiver
                        bool submitted = handler_pool_->submit([this, decoded]() { handle_request(decoded); });
                        if (!submitted) {
                            echo::warn("handler pool queue full, rejecting request id=", decoded.request_id);
                            // Send error response - handler pool is overloaded
                            Message error_payload;
                            dp::String error_msg = "Handler pool overloaded";
                            error_payload.assign(error_msg.begin(), error_msg.end());
                            Message remote_response = encode_remote_message_v2(decoded.request_id, decoded.method_id,
                                                                               error_payload, MessageType::Error);
                            std::lock_guard<std::mutex> lock(send_mutex_);
                            stream_.send(remote_response);
                        }
                    } else if (decoded.type == MessageType::Cancel) {
                        // Cancellation request - handle it
                        handle_cancel(decoded);
                    } else {
                        // Response or error - match with pending request
                        handle_response(decoded);
                    }
                }

                echo::debug("remote bidirect receiver thread stopped");
            }

            /// Handle incoming request from peer
            void handle_request(const DecodedMessageV2 &decoded) {
                echo::trace("remote bidirect handling request id=", decoded.request_id, " method=", decoded.method_id);

                // Track handler lifecycle
                auto handler_info = std::make_shared<HandlerInfo>(decoded.request_id, decoded.method_id);
                {
                    std::lock_guard<std::mutex> lock(handlers_mutex_);
                    active_handlers_[decoded.request_id] = handler_info;
                }

                // Start metrics tracking if enabled
                std::unique_ptr<MetricsTracker> tracker;
                std::unique_ptr<HandlerMetricsTracker> handler_tracker;
                if (enable_metrics_) {
                    tracker = std::make_unique<MetricsTracker>(server_metrics_, decoded.payload.size());
                }

                // Get handler for method_id
                auto handler_res = registry_.get_handler(decoded.method_id);
                Message remote_response;

                if (handler_res.is_err()) {
                    // No handler found - send error response
                    echo::warn("no handler for method_id: ", decoded.method_id);
                    dp::String error_msg = dp::String("No handler for method_id: ") +
                                           dp::String(std::to_string(decoded.method_id).c_str());
                    Message error_payload(error_msg.begin(), error_msg.end());
                    remote_response = encode_remote_message_v2(decoded.request_id, decoded.method_id, error_payload,
                                                               MessageType::Error);
                    if (tracker)
                        tracker->failure();
                } else {
                    // Call handler
                    auto handler = handler_res.value();

                    // Track handler execution time
                    if (enable_metrics_) {
                        handler_tracker = std::make_unique<HandlerMetricsTracker>(server_metrics_);
                    }

                    auto result = handler(decoded.payload);

                    if (result.is_err()) {
                        // Handler returned error
                        echo::warn("handler returned error: ", result.error().message.c_str());
                        Message error_payload(result.error().message.begin(), result.error().message.end());
                        remote_response = encode_remote_message_v2(decoded.request_id, decoded.method_id, error_payload,
                                                                   MessageType::Error);
                        if (tracker)
                            tracker->failure();
                    } else {
                        // Handler succeeded
                        remote_response = encode_remote_message_v2(decoded.request_id, decoded.method_id,
                                                                   result.value(), MessageType::Response);
                        if (tracker)
                            tracker->success(result.value().size());
                    }
                }

                // Send response (only if still running)
                if (!running_) {
                    echo::trace("remote bidirect shutting down, skipping response");
                    return;
                }

                // Protect stream send with mutex
                {
                    std::lock_guard<std::mutex> lock(send_mutex_);
                    auto send_res = stream_.send(remote_response);
                    if (send_res.is_err()) {
                        echo::trace("remote bidirect send response failed: ", send_res.error().message.c_str());
                        return; // Exit if we can't send responses
                    }
                }

                echo::trace("remote bidirect sent response id=", decoded.request_id);

                // Mark handler as completed and remove from tracking
                handler_info->completed = true;
                {
                    std::lock_guard<std::mutex> lock(handlers_mutex_);
                    active_handlers_.erase(decoded.request_id);
                }

                // Log handler execution time
                auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                                      handler_info->start_time)
                                    .count();
                echo::trace("handler completed id=", decoded.request_id, " method=", decoded.method_id,
                            " duration=", duration, "ms");
            }

            /// Handle response to our outgoing call
            void handle_response(const DecodedMessageV2 &decoded) {
                dp::u32 request_id = decoded.request_id;
                echo::trace("remote bidirect received response id=", request_id);

                // Find pending request
                std::shared_ptr<PendingRequest> pending;
                {
                    std::lock_guard<std::mutex> lock(pending_mutex_);
                    auto it = pending_requests_.find(request_id);
                    if (it != pending_requests_.end()) {
                        pending = it->second;
                        pending_requests_.erase(it);
                    }
                }

                if (!pending) {
                    echo::warn("received response for unknown request_id: ", request_id);
                    return;
                }

                // Set result
                {
                    std::lock_guard<std::mutex> lock(pending->mutex);
                    if (decoded.type == MessageType::Error) {
                        dp::String error_msg(reinterpret_cast<const char *>(decoded.payload.data()),
                                             decoded.payload.size());
                        pending->result = dp::result::err(dp::Error::io_error(error_msg.c_str()));
                    } else {
                        pending->result = dp::result::ok(std::move(decoded.payload));
                    }
                    pending->completed = true;
                }
                pending->cv.notify_one();
            }

            /// Handle cancellation request from peer
            void handle_cancel(const DecodedMessageV2 &decoded) {
                dp::u32 request_id = decoded.request_id;
                echo::trace("remote bidirect received cancel request id=", request_id);
                echo::debug("cancel request received for id=", request_id, " (handler cancellation not implemented)");
            }

          public:
            /// Constructor for bidirectional RPC
            /// @param stream The underlying stream for communication
            /// @param max_concurrent Maximum number of concurrent pending requests (default: 100)
            /// @param enable_metrics Enable metrics collection (default: false)
            /// @param recv_timeout_ms Receiver thread timeout in milliseconds (default: 100)
            ///        Lower timeout = faster shutdown, higher CPU usage
            ///        Higher timeout = slower shutdown, lower CPU usage
            /// @param handler_threads Number of threads in handler pool (default: 10)
            /// @param max_handler_queue Maximum queued handlers (default: 1000)
            explicit Remote(Stream &stream, dp::usize max_concurrent = 100, bool enable_metrics = false,
                            dp::u32 recv_timeout_ms = 100, dp::usize handler_threads = 10,
                            dp::usize max_handler_queue = 1000)
                : stream_(stream), next_request_id_(0), running_(true), max_concurrent_requests_(max_concurrent),
                  enable_metrics_(enable_metrics) {
                echo::trace("Remote<Bidirect> constructed, max_concurrent=", max_concurrent,
                            " metrics=", enable_metrics, " recv_timeout=", recv_timeout_ms, "ms",
                            " handler_threads=", handler_threads, " max_queue=", max_handler_queue);

                // Create thread pool for handler execution
                handler_pool_ = std::make_unique<ThreadPool>(handler_threads, max_handler_queue);

                // Set receive timeout to allow receiver thread to check running_ flag
                // Configurable timeout allows tuning for different use cases
                stream_.set_recv_timeout(recv_timeout_ms);
                receiver_thread_ = std::thread(&Remote::receiver_loop, this);
            }

            ~Remote() {
                echo::trace("Remote<Bidirect> shutting down");
                running_ = false;

                // Wake up all pending requests
                {
                    std::lock_guard<std::mutex> lock(pending_mutex_);
                    for (auto &pair : pending_requests_) {
                        std::lock_guard<std::mutex> req_lock(pair.second->mutex);
                        pair.second->result = dp::result::err(dp::Error::io_error("Remote destroyed"));
                        pair.second->completed = true;
                        pair.second->cv.notify_one();
                    }
                    pending_requests_.clear();
                }

                // DON'T close stream - let the owner close it
                // The receiver thread will exit when running_ = false and recv() times out

                if (receiver_thread_.joinable()) {
                    receiver_thread_.join();
                }

                // Shutdown thread pool and wait for all handlers to complete
                if (handler_pool_) {
                    handler_pool_->shutdown();
                }

                echo::trace("Remote<Bidirect> destroyed");
            }

            /// Register a handler for incoming requests (server side)
            dp::Res<void> register_method(dp::u32 method_id, Handler handler) {
                return registry_.register_method(method_id, handler);
            }

            /// Unregister a handler
            dp::Res<void> unregister_method(dp::u32 method_id) { return registry_.unregister_method(method_id); }

            /// Set default handler for unknown methods
            void set_default_handler(Handler handler) { registry_.set_default_handler(handler); }

            /// Clear default handler
            void clear_default_handler() { registry_.clear_default_handler(); }

            /// Call a method on the peer (client side)
            /// Thread-safe - can be called from multiple threads
            dp::Res<Message> call(dp::u32 method_id, const Message &request, dp::u32 timeout_ms = 5000) {
                // Start metrics tracking if enabled
                std::unique_ptr<MetricsTracker> tracker;
                if (enable_metrics_) {
                    tracker = std::make_unique<MetricsTracker>(client_metrics_, request.size());
                }

                // Check concurrent request limit
                {
                    std::lock_guard<std::mutex> lock(pending_mutex_);
                    if (pending_requests_.size() >= max_concurrent_requests_) {
                        echo::error("max concurrent requests reached: ", max_concurrent_requests_);
                        if (tracker)
                            tracker->failure();
                        return dp::result::err(dp::Error::io_error("max concurrent requests reached"));
                    }
                }

                // Generate request ID (thread-safe)
                dp::u32 request_id = next_request_id_.fetch_add(1);
                echo::trace("remote bidirect call id=", request_id, " method=", method_id);

                // Create pending request
                auto pending = std::make_shared<PendingRequest>(request_id);

                // Register pending request
                {
                    std::lock_guard<std::mutex> lock(pending_mutex_);
                    pending_requests_[request_id] = pending;
                }

                // Encode and send request (protect with mutex)
                Message remote_request = encode_remote_message_v2(request_id, method_id, request, MessageType::Request);
                {
                    std::lock_guard<std::mutex> lock(send_mutex_);
                    auto send_res = stream_.send(remote_request);
                    if (send_res.is_err()) {
                        // Remove from pending
                        {
                            std::lock_guard<std::mutex> lock(pending_mutex_);
                            pending_requests_.erase(request_id);
                        }
                        echo::error("remote bidirect send failed");
                        if (tracker)
                            tracker->failure();
                        return dp::result::err(send_res.error());
                    }
                }

                // Wait for response with timeout
                {
                    std::unique_lock<std::mutex> lock(pending->mutex);
                    auto timeout = std::chrono::milliseconds(timeout_ms);
                    if (!pending->cv.wait_for(lock, timeout, [&] { return pending->completed; })) {
                        // Timeout - remove from pending
                        {
                            std::lock_guard<std::mutex> plock(pending_mutex_);
                            pending_requests_.erase(request_id);
                        }
                        echo::error("remote bidirect call timeout id=", request_id);
                        if (tracker)
                            tracker->timeout();
                        return dp::result::err(dp::Error::timeout("call timeout"));
                    }
                }

                echo::trace("remote bidirect call completed id=", request_id);

                // Track success/failure
                if (tracker) {
                    if (pending->result.is_ok()) {
                        tracker->success(pending->result.value().size());
                    } else {
                        tracker->failure();
                    }
                }

                return pending->result;
            }

            /// Get number of pending outgoing requests
            dp::usize pending_count() const {
                std::lock_guard<std::mutex> lock(pending_mutex_);
                return pending_requests_.size();
            }

            /// Get number of active handlers (incoming requests being processed)
            dp::usize active_handler_count() const {
                std::lock_guard<std::mutex> lock(handlers_mutex_);
                return active_handlers_.size();
            }

            /// Get number of active tasks in thread pool
            dp::usize active_pool_tasks() const { return handler_pool_ ? handler_pool_->active_count() : 0; }

            /// Get number of queued tasks in thread pool
            dp::usize queued_pool_tasks() const { return handler_pool_ ? handler_pool_->queued_count() : 0; }

            /// Get number of registered methods
            dp::usize method_count() const { return registry_.method_count(); }

            /// Get client metrics (outgoing calls)
            const RemoteMetrics &get_client_metrics() const { return client_metrics_; }

            /// Get server metrics (incoming requests)
            const RemoteMetrics &get_server_metrics() const { return server_metrics_; }

            /// Reset all metrics
            void reset_metrics() {
                client_metrics_.reset();
                server_metrics_.reset();
            }

            /// Check if metrics are enabled
            bool metrics_enabled() const { return enable_metrics_; }

            /// Cancel an in-flight request
            bool cancel(dp::u32 request_id) {
                echo::trace("remote bidirect cancel request id=", request_id);

                // Find and mark pending request as cancelled
                std::shared_ptr<PendingRequest> pending;
                {
                    std::lock_guard<std::mutex> lock(pending_mutex_);
                    auto it = pending_requests_.find(request_id);
                    if (it == pending_requests_.end()) {
                        echo::warn("cancel: request_id not found: ", request_id);
                        return false;
                    }
                    pending = it->second;
                }

                // Try to mark as cancelled
                {
                    std::lock_guard<std::mutex> lock(pending->mutex);
                    if (pending->completed) {
                        echo::trace("cancel: request already completed id=", request_id);
                        return false;
                    }
                    pending->cancelled = true;
                    pending->result = dp::result::err(dp::Error::io_error("request cancelled"));
                    pending->completed = true;
                }

                // Notify waiting thread
                pending->cv.notify_one();

                // Send cancellation message to peer (best effort)
                Message cancel_msg = encode_remote_message_v2(request_id, 0, Message(), MessageType::Cancel);
                {
                    std::lock_guard<std::mutex> lock(send_mutex_);
                    auto send_res = stream_.send(cancel_msg);
                    if (send_res.is_err()) {
                        echo::warn("cancel: failed to send cancel message id=", request_id);
                    }
                }

                // Remove from pending
                {
                    std::lock_guard<std::mutex> lock(pending_mutex_);
                    pending_requests_.erase(request_id);
                }

                echo::trace("remote bidirect cancelled request id=", request_id);
                return true;
            }
        };

    } // namespace remote

    // Export to netpipe namespace
    using Unidirect = remote::Unidirect;
    using Bidirect = remote::Bidirect;
    template <typename Mode> using Remote = remote::Remote<Mode>;

} // namespace netpipe

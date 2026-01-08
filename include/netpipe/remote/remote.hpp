#pragma once

#include <atomic>
#include <netpipe/remote/async.hpp>
#include <netpipe/remote/metrics.hpp>
#include <netpipe/remote/protocol.hpp>
#include <netpipe/remote/registry.hpp>
#include <netpipe/stream.hpp>
#include <thread>

namespace netpipe {
    namespace remote {

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

        /// Remote RPC - Bidirectional mode (peer-to-peer)
        /// Both sides can register handlers AND make calls
        /// Supports concurrent requests
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
                        // Incoming request - handle it in a separate thread to avoid blocking receiver
                        std::thread([this, decoded]() { handle_request(decoded); }).detach();
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
            explicit Remote(Stream &stream, dp::usize max_concurrent = 100, bool enable_metrics = false)
                : stream_(stream), next_request_id_(0), running_(true), max_concurrent_requests_(max_concurrent),
                  enable_metrics_(enable_metrics) {
                echo::trace("Remote<Bidirect> constructed, max_concurrent=", max_concurrent,
                            " metrics=", enable_metrics);
                // Set receive timeout to allow receiver thread to check running_ flag
                stream_.set_recv_timeout(100); // 100ms timeout
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

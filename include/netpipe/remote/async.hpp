#pragma once

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <netpipe/remote/metrics.hpp>
#include <netpipe/remote/protocol.hpp>
#include <netpipe/stream.hpp>
#include <thread>

namespace netpipe {
    namespace remote {

        /// Pending request state
        struct PendingRequest {
            dp::u32 request_id;
            std::mutex mutex;
            std::condition_variable cv;
            bool completed;
            bool cancelled;
            dp::Res<Message> result;

            PendingRequest(dp::u32 id)
                : request_id(id), completed(false), cancelled(false),
                  result(dp::result::err(dp::Error::timeout("timeout"))) {}
        };

        /// Remote with concurrent request support
        /// Allows multiple in-flight requests with out-of-order responses
        /// Thread-safe for concurrent calls from multiple threads
        class RemoteAsync {
          private:
            Stream &stream_;
            std::atomic<dp::u32> next_request_id_;
            std::map<dp::u32, std::shared_ptr<PendingRequest>> pending_requests_;
            mutable std::mutex pending_mutex_;
            std::thread receiver_thread_;
            std::atomic<bool> running_;
            dp::usize max_concurrent_requests_;
            RemoteMetrics metrics_;
            bool enable_metrics_;

            /// Receiver thread function - processes incoming responses
            void receiver_loop() {
                echo::debug("remote async receiver thread started");

                while (running_) {
                    // Receive response
                    auto recv_res = stream_.recv();
                    if (recv_res.is_err()) {
                        // Timeout is expected - just continue to check running_ flag
                        if (recv_res.error().code == dp::Error::TIMEOUT) {
                            continue;
                        }
                        if (running_) {
                            echo::error("remote async recv failed: ", recv_res.error().message.c_str());
                        }
                        break;
                    }

                    // Decode response
                    auto decode_res = decode_remote_message_v2(recv_res.value());
                    if (decode_res.is_err()) {
                        echo::error("remote async decode failed");
                        continue;
                    }

                    auto decoded = decode_res.value();
                    dp::u32 request_id = decoded.request_id;

                    echo::trace("remote async received response id=", request_id);

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
                        continue;
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

                echo::debug("remote async receiver thread stopped");
            }

          public:
            explicit RemoteAsync(Stream &stream, dp::usize max_concurrent = 100, bool enable_metrics = false)
                : stream_(stream), next_request_id_(0), running_(true), max_concurrent_requests_(max_concurrent),
                  enable_metrics_(enable_metrics) {
                echo::trace("RemoteAsync constructed, max_concurrent=", max_concurrent, " metrics=", enable_metrics);
                // Set receive timeout to allow receiver thread to check running_ flag
                stream_.set_recv_timeout(100); // 100ms timeout
                receiver_thread_ = std::thread(&RemoteAsync::receiver_loop, this);
            }

            ~RemoteAsync() {
                echo::trace("RemoteAsync shutting down");
                running_ = false;

                // Wake up all pending requests
                {
                    std::lock_guard<std::mutex> lock(pending_mutex_);
                    for (auto &pair : pending_requests_) {
                        std::lock_guard<std::mutex> req_lock(pair.second->mutex);
                        pair.second->result = dp::result::err(dp::Error::io_error("RemoteAsync destroyed"));
                        pair.second->completed = true;
                        pair.second->cv.notify_one();
                    }
                    pending_requests_.clear();
                }

                // Close stream to unblock receiver thread
                stream_.close();

                if (receiver_thread_.joinable()) {
                    receiver_thread_.join();
                }
                echo::trace("RemoteAsync destroyed");
            }

            /// Client side: call with concurrent request support
            /// Thread-safe - can be called from multiple threads
            dp::Res<Message> call(dp::u32 method_id, const Message &request, dp::u32 timeout_ms = 5000) {
                // Start metrics tracking if enabled
                std::unique_ptr<MetricsTracker> tracker;
                if (enable_metrics_) {
                    tracker = std::make_unique<MetricsTracker>(metrics_, request.size());
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
                echo::trace("remote async call id=", request_id, " method=", method_id);

                // Create pending request
                auto pending = std::make_shared<PendingRequest>(request_id);

                // Register pending request
                {
                    std::lock_guard<std::mutex> lock(pending_mutex_);
                    pending_requests_[request_id] = pending;
                }

                // Encode and send request
                Message remote_request = encode_remote_message_v2(request_id, method_id, request, MessageType::Request);
                auto send_res = stream_.send(remote_request);
                if (send_res.is_err()) {
                    // Remove from pending
                    {
                        std::lock_guard<std::mutex> lock(pending_mutex_);
                        pending_requests_.erase(request_id);
                    }
                    echo::error("remote async send failed");
                    if (tracker)
                        tracker->failure();
                    return dp::result::err(send_res.error());
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
                        echo::error("remote async call timeout id=", request_id);
                        if (tracker)
                            tracker->timeout();
                        return dp::result::err(dp::Error::timeout("call timeout"));
                    }
                }

                echo::trace("remote async call completed id=", request_id);

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

            /// Get number of pending requests
            dp::usize pending_count() const {
                std::lock_guard<std::mutex> lock(pending_mutex_);
                return pending_requests_.size();
            }

            /// Get maximum concurrent requests
            dp::usize max_concurrent() const { return max_concurrent_requests_; }

            /// Set maximum concurrent requests
            void set_max_concurrent(dp::usize max) { max_concurrent_requests_ = max; }

            /// Get metrics (if enabled)
            const RemoteMetrics &get_metrics() const { return metrics_; }

            /// Reset metrics
            void reset_metrics() { metrics_.reset(); }

            /// Check if metrics are enabled
            bool metrics_enabled() const { return enable_metrics_; }

            /// Cancel an in-flight request
            /// Returns true if the request was found and cancelled, false otherwise
            /// Note: If the response has already been received, cancellation will fail
            bool cancel(dp::u32 request_id) {
                echo::trace("remote async cancel request id=", request_id);

                // Find and mark pending request as cancelled
                std::shared_ptr<PendingRequest> pending;
                {
                    std::lock_guard<std::mutex> lock(pending_mutex_);
                    auto it = pending_requests_.find(request_id);
                    if (it == pending_requests_.end()) {
                        echo::warn("cancel: request_id not found: ", request_id);
                        return false; // Request not found (already completed or never existed)
                    }
                    pending = it->second;
                }

                // Try to mark as cancelled
                {
                    std::lock_guard<std::mutex> lock(pending->mutex);
                    if (pending->completed) {
                        echo::trace("cancel: request already completed id=", request_id);
                        return false; // Already completed
                    }
                    pending->cancelled = true;
                    pending->result = dp::result::err(dp::Error::io_error("request cancelled"));
                    pending->completed = true;
                }

                // Notify waiting thread
                pending->cv.notify_one();

                // Send cancellation message to server (best effort)
                Message cancel_msg = encode_remote_message_v2(request_id, 0, Message(), MessageType::Cancel);
                auto send_res = stream_.send(cancel_msg);
                if (send_res.is_err()) {
                    echo::warn("cancel: failed to send cancel message id=", request_id);
                }

                // Remove from pending
                {
                    std::lock_guard<std::mutex> lock(pending_mutex_);
                    pending_requests_.erase(request_id);
                }

                echo::trace("remote async cancelled request id=", request_id);
                return true;
            }
        };

    } // namespace remote

    using RemoteAsync = remote::RemoteAsync;

} // namespace netpipe

// Note: RemoteAsync is experimental and requires careful lifecycle management.
// The receiver thread blocks on recv() and needs the stream to be closed
// for proper shutdown. Consider using RemoteRouter for most use cases.

#pragma once

#include <atomic>
#include <chrono>
#include <datapod/datapod.hpp>
#include <mutex>

namespace netpipe {
    namespace remote {

        /// Metrics for Remote RPC operations
        struct RemoteMetrics {
            // Request counts
            std::atomic<dp::u64> total_requests{0};
            std::atomic<dp::u64> successful_requests{0};
            std::atomic<dp::u64> failed_requests{0};
            std::atomic<dp::u64> timeout_requests{0};

            // In-flight tracking
            std::atomic<dp::u64> in_flight_requests{0};
            std::atomic<dp::u64> peak_in_flight_requests{0};

            // Latency tracking (microseconds)
            std::atomic<dp::u64> total_latency_us{0};
            std::atomic<dp::u64> min_latency_us{UINT64_MAX};
            std::atomic<dp::u64> max_latency_us{0};

            // Message size tracking (bytes)
            std::atomic<dp::u64> total_request_bytes{0};
            std::atomic<dp::u64> total_response_bytes{0};

            // Handler execution time (microseconds)
            std::atomic<dp::u64> total_handler_time_us{0};
            std::atomic<dp::u64> handler_invocations{0};

            /// Reset all metrics to zero
            inline void reset() {
                total_requests = 0;
                successful_requests = 0;
                failed_requests = 0;
                timeout_requests = 0;
                in_flight_requests = 0;
                peak_in_flight_requests = 0;
                total_latency_us = 0;
                min_latency_us = UINT64_MAX;
                max_latency_us = 0;
                total_request_bytes = 0;
                total_response_bytes = 0;
                total_handler_time_us = 0;
                handler_invocations = 0;
            }

            /// Get average latency in microseconds
            inline dp::u64 avg_latency_us() const {
                dp::u64 total = total_requests.load();
                if (total == 0)
                    return 0;
                return total_latency_us.load() / total;
            }

            /// Get average handler execution time in microseconds
            inline dp::u64 avg_handler_time_us() const {
                dp::u64 invocations = handler_invocations.load();
                if (invocations == 0)
                    return 0;
                return total_handler_time_us.load() / invocations;
            }

            /// Get success rate (0.0 to 1.0)
            inline double success_rate() const {
                dp::u64 total = total_requests.load();
                if (total == 0)
                    return 0.0;
                return static_cast<double>(successful_requests.load()) / static_cast<double>(total);
            }

            /// Get failure rate (0.0 to 1.0)
            inline double failure_rate() const {
                dp::u64 total = total_requests.load();
                if (total == 0)
                    return 0.0;
                return static_cast<double>(failed_requests.load()) / static_cast<double>(total);
            }

            /// Get timeout rate (0.0 to 1.0)
            inline double timeout_rate() const {
                dp::u64 total = total_requests.load();
                if (total == 0)
                    return 0.0;
                return static_cast<double>(timeout_requests.load()) / static_cast<double>(total);
            }

            /// Get average request size in bytes
            inline dp::u64 avg_request_bytes() const {
                dp::u64 total = total_requests.load();
                if (total == 0)
                    return 0;
                return total_request_bytes.load() / total;
            }

            /// Get average response size in bytes
            inline dp::u64 avg_response_bytes() const {
                dp::u64 successful = successful_requests.load();
                if (successful == 0)
                    return 0;
                return total_response_bytes.load() / successful;
            }
        };

        /// RAII helper for tracking request metrics
        class MetricsTracker {
          private:
            RemoteMetrics &metrics_;
            std::chrono::steady_clock::time_point start_time_;
            dp::usize request_size_;
            bool completed_;

          public:
            explicit MetricsTracker(RemoteMetrics &metrics, dp::usize request_size)
                : metrics_(metrics), start_time_(std::chrono::steady_clock::now()), request_size_(request_size),
                  completed_(false) {
                metrics_.total_requests.fetch_add(1);
                metrics_.total_request_bytes.fetch_add(request_size);

                // Track in-flight
                dp::u64 in_flight = metrics_.in_flight_requests.fetch_add(1) + 1;
                dp::u64 peak = metrics_.peak_in_flight_requests.load();
                while (in_flight > peak && !metrics_.peak_in_flight_requests.compare_exchange_weak(peak, in_flight)) {
                    // Retry if another thread updated peak
                }
            }

            ~MetricsTracker() {
                if (!completed_) {
                    // Request was not completed (likely exception or early return)
                    metrics_.failed_requests.fetch_add(1);
                }
                metrics_.in_flight_requests.fetch_sub(1);
            }

            /// Mark request as successful
            inline void success(dp::usize response_size) {
                if (completed_)
                    return;

                auto end_time = std::chrono::steady_clock::now();
                auto latency_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time_).count();

                metrics_.successful_requests.fetch_add(1);
                metrics_.total_response_bytes.fetch_add(response_size);
                metrics_.total_latency_us.fetch_add(latency_us);

                // Update min latency
                dp::u64 min = metrics_.min_latency_us.load();
                while (static_cast<dp::u64>(latency_us) < min &&
                       !metrics_.min_latency_us.compare_exchange_weak(min, latency_us)) {
                    // Retry if another thread updated min
                }

                // Update max latency
                dp::u64 max = metrics_.max_latency_us.load();
                while (static_cast<dp::u64>(latency_us) > max &&
                       !metrics_.max_latency_us.compare_exchange_weak(max, latency_us)) {
                    // Retry if another thread updated max
                }

                completed_ = true;
            }

            /// Mark request as failed
            inline void failure() {
                if (completed_)
                    return;

                metrics_.failed_requests.fetch_add(1);
                completed_ = true;
            }

            /// Mark request as timed out
            inline void timeout() {
                if (completed_)
                    return;

                metrics_.timeout_requests.fetch_add(1);
                metrics_.failed_requests.fetch_add(1);
                completed_ = true;
            }
        };

        /// RAII helper for tracking handler execution time
        class HandlerMetricsTracker {
          private:
            RemoteMetrics &metrics_;
            std::chrono::steady_clock::time_point start_time_;

          public:
            explicit HandlerMetricsTracker(RemoteMetrics &metrics)
                : metrics_(metrics), start_time_(std::chrono::steady_clock::now()) {}

            ~HandlerMetricsTracker() {
                auto end_time = std::chrono::steady_clock::now();
                auto duration_us =
                    std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time_).count();

                metrics_.total_handler_time_us.fetch_add(duration_us);
                metrics_.handler_invocations.fetch_add(1);
            }
        };

    } // namespace remote

    using RemoteMetrics = remote::RemoteMetrics;
    using MetricsTracker = remote::MetricsTracker;
    using HandlerMetricsTracker = remote::HandlerMetricsTracker;

} // namespace netpipe

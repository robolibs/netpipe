#pragma once

#include <netpipe/protocol/http2/types.hpp>

namespace netpipe::http2 {

    class FlowController {
      public:
        static constexpr dp::i32 DEFAULT_WINDOW = 65535;
        static constexpr dp::i32 MAX_WINDOW = 2147483647;

        FlowController() = default;

        void ensure_stream(dp::u32 stream_id) {
            if (stream_windows_.find(stream_id) == stream_windows_.end()) {
                stream_windows_[stream_id] = DEFAULT_WINDOW;
            }
            if (priorities_.find(stream_id) == priorities_.end()) {
                priorities_[stream_id] = PriorityNode{};
            }
        }

        dp::Result<void> consume_outbound(dp::u32 stream_id, dp::i32 bytes) {
            if (bytes < 0) {
                return dp::result::err(dp::Error::invalid_argument("flow-control bytes must be non-negative"));
            }
            ensure_stream(stream_id);

            if (connection_send_window_ < bytes || stream_windows_[stream_id] < bytes) {
                return dp::result::err(dp::Error::invalid_argument("flow-control window exhausted"));
            }

            connection_send_window_ -= bytes;
            stream_windows_[stream_id] -= bytes;
            return dp::result::ok();
        }

        dp::Result<void> update_connection_send_window(dp::i32 increment) {
            return add_window(connection_send_window_, increment, "connection");
        }

        dp::Result<void> update_stream_send_window(dp::u32 stream_id, dp::i32 increment) {
            ensure_stream(stream_id);
            return add_window(stream_windows_[stream_id], increment, "stream");
        }

        dp::i32 connection_send_window() const { return connection_send_window_; }

        dp::i32 stream_send_window(dp::u32 stream_id) const {
            auto it = stream_windows_.find(stream_id);
            if (it == stream_windows_.end()) {
                return DEFAULT_WINDOW;
            }
            return it->second;
        }

        dp::Result<void> set_priority(dp::u32 stream_id, dp::u32 dependency_stream_id, dp::u8 weight, bool exclusive) {
            if (stream_id == 0) {
                return dp::result::err(dp::Error::invalid_argument("priority stream id must be non-zero"));
            }
            if (weight == 0) {
                return dp::result::err(dp::Error::invalid_argument("priority weight must be in [1,255]"));
            }

            ensure_stream(stream_id);
            priorities_[stream_id].parent = dependency_stream_id;
            priorities_[stream_id].weight = weight;
            priorities_[stream_id].exclusive = exclusive;
            return dp::result::ok();
        }

        void enqueue_stream_data(dp::u32 stream_id, dp::usize bytes) {
            ensure_stream(stream_id);
            priorities_[stream_id].pending_bytes += bytes;
        }

        dp::Optional<dp::u32> pick_next_stream() const {
            dp::Optional<dp::u32> best_stream;
            dp::usize best_score = 0;

            for (const auto &[stream_id, node] : priorities_) {
                if (node.pending_bytes == 0) {
                    continue;
                }
                if (stream_send_window(stream_id) <= 0) {
                    continue;
                }

                dp::usize score = node.pending_bytes * static_cast<dp::usize>(node.weight);
                if (!best_stream.has_value() || score > best_score) {
                    best_stream = stream_id;
                    best_score = score;
                }
            }

            return best_stream;
        }

        dp::Result<void> mark_stream_scheduled(dp::u32 stream_id, dp::usize bytes_sent) {
            ensure_stream(stream_id);
            if (priorities_[stream_id].pending_bytes < bytes_sent) {
                return dp::result::err(dp::Error::invalid_argument("scheduled bytes exceed pending bytes"));
            }
            priorities_[stream_id].pending_bytes -= bytes_sent;
            return dp::result::ok();
        }

      private:
        struct PriorityNode {
            dp::u32 parent = 0;
            dp::u8 weight = 16;
            bool exclusive = false;
            dp::usize pending_bytes = 0;
        };

        static dp::Result<void> add_window(dp::i32 &window, dp::i32 increment, const char *scope) {
            if (increment <= 0) {
                return dp::result::err(
                    dp::Error::invalid_argument(dp::String(scope) + " WINDOW_UPDATE increment must be positive"));
            }
            if (window > MAX_WINDOW - increment) {
                return dp::result::err(
                    dp::Error::invalid_argument(dp::String(scope) + " flow-control window overflow"));
            }
            window += increment;
            return dp::result::ok();
        }

        dp::i32 connection_send_window_ = DEFAULT_WINDOW;
        dp::Map<dp::u32, dp::i32> stream_windows_;
        dp::Map<dp::u32, PriorityNode> priorities_;
    };

} // namespace netpipe::http2

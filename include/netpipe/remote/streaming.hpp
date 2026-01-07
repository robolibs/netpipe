#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <netpipe/remote/protocol.hpp>
#include <netpipe/stream.hpp>
#include <queue>
#include <thread>

namespace netpipe {
    namespace remote {

        /// Stream callback - called for each chunk received
        using StreamCallback = std::function<void(const Message &chunk)>;

        /// Stream state
        struct StreamState {
            dp::u32 stream_id;
            std::mutex mutex;
            std::condition_variable cv;
            std::queue<Message> chunks;
            bool completed;
            bool error;
            dp::String error_message;
            StreamCallback callback;

            StreamState(dp::u32 id) : stream_id(id), completed(false), error(false) {}
        };

        /// Streaming Remote - supports client, server, and bidirectional streaming
        class StreamingRemote {
          private:
            Stream &stream_;
            std::atomic<dp::u32> next_stream_id_;
            std::map<dp::u32, std::shared_ptr<StreamState>> active_streams_;
            mutable std::mutex streams_mutex_;
            std::thread receiver_thread_;
            std::atomic<bool> running_;

            /// Receiver thread - processes incoming stream messages
            void receiver_loop() {
                echo::debug("streaming remote receiver thread started");

                while (running_) {
                    auto recv_res = stream_.recv();
                    if (recv_res.is_err()) {
                        // Timeout is expected
                        if (recv_res.error().code == dp::Error::TIMEOUT) {
                            continue;
                        }
                        if (running_) {
                            echo::error("streaming remote recv failed: ", recv_res.error().message.c_str());
                        }
                        break;
                    }

                    auto decode_res = decode_remote_message_v2(recv_res.value());
                    if (decode_res.is_err()) {
                        echo::error("streaming remote decode failed");
                        continue;
                    }

                    auto decoded = decode_res.value();
                    handle_stream_message(decoded);
                }

                echo::debug("streaming remote receiver thread stopped");
            }

            /// Handle incoming stream message
            void handle_stream_message(const DecodedMessageV2 &decoded) {
                dp::u32 stream_id = decoded.request_id; // Using request_id as stream_id

                // Find stream
                std::shared_ptr<StreamState> stream;
                {
                    std::lock_guard<std::mutex> lock(streams_mutex_);
                    auto it = active_streams_.find(stream_id);
                    if (it != active_streams_.end()) {
                        stream = it->second;
                    }
                }

                if (!stream) {
                    echo::warn("received stream message for unknown stream_id: ", stream_id);
                    return;
                }

                std::lock_guard<std::mutex> lock(stream->mutex);

                if (decoded.type == MessageType::StreamData) {
                    // Data chunk
                    if (stream->callback) {
                        stream->callback(decoded.payload);
                    } else {
                        stream->chunks.push(decoded.payload);
                    }
                    stream->cv.notify_one();
                } else if (decoded.type == MessageType::StreamEnd) {
                    // Stream completed
                    stream->completed = true;
                    stream->cv.notify_one();
                } else if (decoded.type == MessageType::StreamError) {
                    // Stream error
                    stream->error = true;
                    stream->error_message =
                        dp::String(reinterpret_cast<const char *>(decoded.payload.data()), decoded.payload.size());
                    stream->completed = true;
                    stream->cv.notify_one();
                }
            }

          public:
            explicit StreamingRemote(Stream &stream) : stream_(stream), next_stream_id_(0), running_(true) {
                echo::trace("StreamingRemote constructed");
                stream_.set_recv_timeout(100); // 100ms timeout
                receiver_thread_ = std::thread(&StreamingRemote::receiver_loop, this);
            }

            ~StreamingRemote() {
                echo::trace("StreamingRemote shutting down");
                running_ = false;

                // Complete all active streams
                {
                    std::lock_guard<std::mutex> lock(streams_mutex_);
                    for (auto &pair : active_streams_) {
                        std::lock_guard<std::mutex> stream_lock(pair.second->mutex);
                        pair.second->error = true;
                        pair.second->error_message = "StreamingRemote destroyed";
                        pair.second->completed = true;
                        pair.second->cv.notify_one();
                    }
                    active_streams_.clear();
                }

                stream_.close();

                if (receiver_thread_.joinable()) {
                    receiver_thread_.join();
                }
                echo::trace("StreamingRemote destroyed");
            }

            /// Client streaming: send multiple chunks, receive one response
            /// Returns the final response
            dp::Res<Message> client_stream(dp::u32 method_id, const dp::Vector<Message> &chunks,
                                           dp::u32 timeout_ms = 5000) {
                dp::u32 stream_id = next_stream_id_.fetch_add(1);
                echo::trace("client_stream id=", stream_id, " method=", method_id, " chunks=", chunks.size());

                // Create stream state
                auto stream_state = std::make_shared<StreamState>(stream_id);
                {
                    std::lock_guard<std::mutex> lock(streams_mutex_);
                    active_streams_[stream_id] = stream_state;
                }

                // Send all chunks
                for (dp::usize i = 0; i < chunks.size(); i++) {
                    bool is_final = (i == chunks.size() - 1);
                    dp::u16 flags = MessageFlags::Streaming;
                    if (is_final) {
                        flags |= MessageFlags::Final;
                    }

                    Message msg =
                        encode_remote_message_v2(stream_id, method_id, chunks[i], MessageType::StreamData, flags);
                    auto send_res = stream_.send(msg);
                    if (send_res.is_err()) {
                        std::lock_guard<std::mutex> lock(streams_mutex_);
                        active_streams_.erase(stream_id);
                        return dp::result::err(send_res.error());
                    }
                }

                // Wait for response (StreamEnd with final data)
                {
                    std::unique_lock<std::mutex> lock(stream_state->mutex);
                    auto timeout = std::chrono::milliseconds(timeout_ms);
                    if (!stream_state->cv.wait_for(lock, timeout, [&] { return stream_state->completed; })) {
                        std::lock_guard<std::mutex> slock(streams_mutex_);
                        active_streams_.erase(stream_id);
                        return dp::result::err(dp::Error::timeout("client stream timeout"));
                    }

                    if (stream_state->error) {
                        std::lock_guard<std::mutex> slock(streams_mutex_);
                        active_streams_.erase(stream_id);
                        return dp::result::err(dp::Error::io_error(stream_state->error_message.c_str()));
                    }

                    // Get final response
                    Message response;
                    if (!stream_state->chunks.empty()) {
                        response = stream_state->chunks.front();
                    }

                    std::lock_guard<std::mutex> slock(streams_mutex_);
                    active_streams_.erase(stream_id);
                    return dp::result::ok(response);
                }
            }

            /// Server streaming: send one request, receive multiple chunks
            /// Chunks are delivered via callback
            dp::Res<void> server_stream(dp::u32 method_id, const Message &request, StreamCallback callback,
                                        dp::u32 timeout_ms = 5000) {
                dp::u32 stream_id = next_stream_id_.fetch_add(1);
                echo::trace("server_stream id=", stream_id, " method=", method_id);

                // Create stream state with callback
                auto stream_state = std::make_shared<StreamState>(stream_id);
                stream_state->callback = callback;
                {
                    std::lock_guard<std::mutex> lock(streams_mutex_);
                    active_streams_[stream_id] = stream_state;
                }

                // Send request
                Message msg = encode_remote_message_v2(stream_id, method_id, request, MessageType::Request,
                                                       MessageFlags::Streaming | MessageFlags::RequiresAck);
                auto send_res = stream_.send(msg);
                if (send_res.is_err()) {
                    std::lock_guard<std::mutex> lock(streams_mutex_);
                    active_streams_.erase(stream_id);
                    return dp::result::err(send_res.error());
                }

                // Wait for stream to complete
                {
                    std::unique_lock<std::mutex> lock(stream_state->mutex);
                    auto timeout = std::chrono::milliseconds(timeout_ms);
                    if (!stream_state->cv.wait_for(lock, timeout, [&] { return stream_state->completed; })) {
                        std::lock_guard<std::mutex> slock(streams_mutex_);
                        active_streams_.erase(stream_id);
                        return dp::result::err(dp::Error::timeout("server stream timeout"));
                    }

                    if (stream_state->error) {
                        std::lock_guard<std::mutex> slock(streams_mutex_);
                        active_streams_.erase(stream_id);
                        return dp::result::err(dp::Error::io_error(stream_state->error_message.c_str()));
                    }
                }

                std::lock_guard<std::mutex> lock(streams_mutex_);
                active_streams_.erase(stream_id);
                return dp::result::ok();
            }

            /// Bidirectional streaming: send and receive multiple chunks
            /// Returns stream_id for sending chunks and receiving via callback
            dp::Res<dp::u32> bidirectional_stream(dp::u32 method_id, StreamCallback callback) {
                dp::u32 stream_id = next_stream_id_.fetch_add(1);
                echo::trace("bidirectional_stream id=", stream_id, " method=", method_id);

                // Create stream state with callback
                auto stream_state = std::make_shared<StreamState>(stream_id);
                stream_state->callback = callback;
                {
                    std::lock_guard<std::mutex> lock(streams_mutex_);
                    active_streams_[stream_id] = stream_state;
                }

                // Send initial request to start stream
                Message msg = encode_remote_message_v2(stream_id, method_id, Message(), MessageType::Request,
                                                       MessageFlags::Streaming);
                auto send_res = stream_.send(msg);
                if (send_res.is_err()) {
                    std::lock_guard<std::mutex> lock(streams_mutex_);
                    active_streams_.erase(stream_id);
                    return dp::result::err(send_res.error());
                }

                return dp::result::ok(stream_id);
            }

            /// Send a chunk on a bidirectional stream
            dp::Res<void> send_chunk(dp::u32 stream_id, const Message &chunk, bool is_final = false) {
                dp::u16 flags = MessageFlags::Streaming;
                if (is_final) {
                    flags |= MessageFlags::Final;
                }

                Message msg = encode_remote_message_v2(stream_id, 0, chunk, MessageType::StreamData, flags);
                return stream_.send(msg);
            }

            /// End a bidirectional stream
            dp::Res<void> end_stream(dp::u32 stream_id) {
                Message msg = encode_remote_message_v2(stream_id, 0, Message(), MessageType::StreamEnd);
                auto send_res = stream_.send(msg);

                // Remove from active streams
                {
                    std::lock_guard<std::mutex> lock(streams_mutex_);
                    active_streams_.erase(stream_id);
                }

                return send_res;
            }

            /// Get number of active streams
            dp::usize active_stream_count() const {
                std::lock_guard<std::mutex> lock(streams_mutex_);
                return active_streams_.size();
            }
        };

    } // namespace remote

    using StreamingRemote = remote::StreamingRemote;
    using StreamCallback = remote::StreamCallback;

} // namespace netpipe

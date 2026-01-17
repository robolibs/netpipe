#pragma once

#include <datapod/datapod.hpp>
#include <echo/echo.hpp>
#include <map>
#include <netpipe/transport/stream/quic/frame.hpp>
#include <netpipe/transport/stream/quic/types.hpp>

namespace netpipe::quic {

    // Stream states (RFC 9000 Section 3)
    //
    // Sending states:
    //   Ready -> Send -> DataSent -> DataRecvd
    //                              -> ResetSent -> ResetRecvd
    //
    // Receiving states:
    //   Recv -> SizeKnown -> DataRecvd -> DataRead
    //        -> ResetRecvd -> ResetRead

    enum class SendState {
        Ready,      // Stream created, no data sent
        Send,       // Sending data
        DataSent,   // All data sent, waiting for ACKs
        DataRecvd,  // All data acknowledged
        ResetSent,  // RESET_STREAM sent
        ResetRecvd, // RESET_STREAM acknowledged
    };

    enum class RecvState {
        Recv,       // Receiving data
        SizeKnown,  // Final size known (FIN received)
        DataRecvd,  // All data received
        DataRead,   // All data read by application
        ResetRecvd, // RESET_STREAM received
        ResetRead,  // Reset delivered to application
    };

    // Data chunk with offset (for reassembly)
    struct DataChunk {
        dp::u64 offset;
        dp::Vector<dp::u8> data;

        dp::u64 end_offset() const { return offset + data.size(); }
    };

    // Send buffer for a stream
    class SendBuffer {
      public:
        SendBuffer() = default;

        // Queue data for sending
        void write(const dp::Vector<dp::u8> &data) {
            buffer_.insert(buffer_.end(), data.begin(), data.end());
            unsent_offset_ = sent_offset_ + buffer_.size();
        }

        // Get data to send (up to max_len bytes)
        dp::Vector<dp::u8> get_data(dp::usize max_len) {
            dp::usize available = buffer_.size() - (sent_offset_ - acked_offset_);
            dp::usize send_len = std::min(available, max_len);

            if (send_len == 0) {
                return {};
            }

            dp::usize start = sent_offset_ - acked_offset_;
            return dp::Vector<dp::u8>(buffer_.begin() + start, buffer_.begin() + start + send_len);
        }

        // Mark data as sent
        void mark_sent(dp::usize len) { sent_offset_ += len; }

        // Mark data as acknowledged
        void mark_acked(dp::u64 offset, dp::usize len) {
            // Simple implementation: only track contiguous ACKs from the beginning
            if (offset == acked_offset_) {
                acked_offset_ += len;
                // Remove acknowledged data from buffer
                dp::usize remove = acked_offset_ - (sent_offset_ - buffer_.size());
                if (remove > 0 && remove <= buffer_.size()) {
                    buffer_.erase(buffer_.begin(), buffer_.begin() + remove);
                }
            }
        }

        // Mark stream as finished (FIN)
        void set_fin() { fin_ = true; }
        bool has_fin() const { return fin_; }
        bool fin_sent() const { return fin_sent_; }
        void mark_fin_sent() { fin_sent_ = true; }
        bool fin_acked() const { return fin_acked_; }
        void mark_fin_acked() { fin_acked_ = true; }

        dp::u64 sent_offset() const { return sent_offset_; }
        dp::u64 acked_offset() const { return acked_offset_; }
        dp::u64 unsent_offset() const { return unsent_offset_; }
        bool has_unsent_data() const { return sent_offset_ < unsent_offset_; }
        bool all_acked() const { return acked_offset_ >= unsent_offset_ && (!fin_ || fin_acked_); }

      private:
        dp::Vector<dp::u8> buffer_;
        dp::u64 acked_offset_ = 0;
        dp::u64 sent_offset_ = 0;
        dp::u64 unsent_offset_ = 0;
        bool fin_ = false;
        bool fin_sent_ = false;
        bool fin_acked_ = false;
    };

    // Receive buffer for a stream (handles out-of-order reassembly)
    class RecvBuffer {
      public:
        RecvBuffer() = default;

        // Insert received data at offset
        void insert(dp::u64 offset, const dp::Vector<dp::u8> &data) {
            if (data.empty()) {
                return;
            }

            // Check for duplicate/overlapping data
            dp::u64 end = offset + data.size();
            if (end <= read_offset_) {
                return; // Already read
            }

            // Adjust for partial overlap with already-read data
            dp::u64 actual_offset = offset;
            const dp::u8 *actual_data = data.data();
            dp::usize actual_len = data.size();

            if (offset < read_offset_) {
                dp::usize skip = read_offset_ - offset;
                actual_offset = read_offset_;
                actual_data += skip;
                actual_len -= skip;
            }

            // Insert into chunks (simplified - could be optimized with interval tree)
            chunks_[actual_offset] = dp::Vector<dp::u8>(actual_data, actual_data + actual_len);

            // Merge contiguous chunks
            merge_chunks();
        }

        // Read available contiguous data
        dp::Vector<dp::u8> read(dp::usize max_len = 0) {
            dp::Vector<dp::u8> result;

            while (!chunks_.empty()) {
                auto it = chunks_.begin();
                if (it->first != read_offset_) {
                    break; // Gap in data
                }

                auto &chunk = it->second;
                dp::usize take = chunk.size();
                if (max_len > 0 && result.size() + take > max_len) {
                    take = max_len - result.size();
                }

                result.insert(result.end(), chunk.begin(), chunk.begin() + take);
                read_offset_ += take;

                if (take < chunk.size()) {
                    // Partial read - update chunk
                    chunk.erase(chunk.begin(), chunk.begin() + take);
                    auto remaining = std::move(chunk);
                    chunks_.erase(it);
                    chunks_[read_offset_] = std::move(remaining);
                    break;
                } else {
                    chunks_.erase(it);
                }

                if (max_len > 0 && result.size() >= max_len) {
                    break;
                }
            }

            return result;
        }

        // Peek at available contiguous data without consuming
        dp::usize available() const {
            dp::usize total = 0;
            dp::u64 offset = read_offset_;

            for (const auto &[chunk_offset, chunk] : chunks_) {
                if (chunk_offset != offset) {
                    break;
                }
                total += chunk.size();
                offset += chunk.size();
            }

            return total;
        }

        // Mark FIN received at offset
        void set_fin(dp::u64 final_offset) {
            fin_received_ = true;
            final_size_ = final_offset;
        }

        bool fin_received() const { return fin_received_; }
        dp::u64 final_size() const { return final_size_; }
        dp::u64 read_offset() const { return read_offset_; }

        // Check if all data has been received
        bool all_received() const { return fin_received_ && read_offset_ >= final_size_; }

        // Check if all data has been read
        bool all_read() const { return fin_received_ && chunks_.empty() && read_offset_ >= final_size_; }

      private:
        void merge_chunks() {
            // Merge overlapping/adjacent chunks
            auto it = chunks_.begin();
            while (it != chunks_.end()) {
                auto next = std::next(it);
                if (next == chunks_.end()) {
                    break;
                }

                dp::u64 end = it->first + it->second.size();
                if (end >= next->first) {
                    // Overlap or adjacent
                    if (end < next->first + next->second.size()) {
                        // Extend with non-overlapping part
                        dp::usize overlap = end - next->first;
                        it->second.insert(it->second.end(), next->second.begin() + overlap, next->second.end());
                    }
                    chunks_.erase(next);
                } else {
                    ++it;
                }
            }
        }

        std::map<dp::u64, dp::Vector<dp::u8>> chunks_;
        dp::u64 read_offset_ = 0;
        bool fin_received_ = false;
        dp::u64 final_size_ = 0;
    };

    // Single QUIC stream
    class QuicStreamState {
      public:
        QuicStreamState(dp::u64 stream_id, bool is_local) : stream_id_(stream_id), is_local_(is_local) {
            // Determine stream type
            type_ = netpipe::quic::stream_type(stream_id);

            // Initialize states based on stream type
            if (netpipe::quic::is_bidirectional(stream_id)) {
                send_state_ = SendState::Ready;
                recv_state_ = RecvState::Recv;
            } else if (is_local) {
                // Local unidirectional - can only send
                send_state_ = SendState::Ready;
                recv_state_ = RecvState::DataRead; // No receive
            } else {
                // Remote unidirectional - can only receive
                send_state_ = SendState::DataRecvd; // No send
                recv_state_ = RecvState::Recv;
            }
        }

        dp::u64 stream_id() const { return stream_id_; }
        StreamType stream_type() const { return type_; }
        bool is_local() const { return is_local_; }
        bool is_bidirectional() const { return netpipe::quic::is_bidirectional(stream_id_); }

        // Send side
        SendState send_state() const { return send_state_; }
        SendBuffer &send_buffer() { return send_buffer_; }
        const SendBuffer &send_buffer() const { return send_buffer_; }

        bool can_send() const { return send_state_ == SendState::Ready || send_state_ == SendState::Send; }

        void write(const dp::Vector<dp::u8> &data) {
            if (!can_send()) {
                return;
            }
            send_buffer_.write(data);
            if (send_state_ == SendState::Ready) {
                send_state_ = SendState::Send;
            }
        }

        void finish() {
            if (can_send()) {
                send_buffer_.set_fin();
                send_state_ = SendState::DataSent;
            }
        }

        void mark_data_acked(dp::u64 offset, dp::usize len) {
            send_buffer_.mark_acked(offset, len);
            if (send_state_ == SendState::DataSent && send_buffer_.all_acked()) {
                send_state_ = SendState::DataRecvd;
            }
        }

        void reset(dp::u64 error_code) {
            reset_error_code_ = error_code;
            send_state_ = SendState::ResetSent;
        }

        void mark_reset_acked() {
            if (send_state_ == SendState::ResetSent) {
                send_state_ = SendState::ResetRecvd;
            }
        }

        // Receive side
        RecvState recv_state() const { return recv_state_; }
        RecvBuffer &recv_buffer() { return recv_buffer_; }
        const RecvBuffer &recv_buffer() const { return recv_buffer_; }

        bool can_recv() const { return recv_state_ == RecvState::Recv || recv_state_ == RecvState::SizeKnown; }

        void receive_data(dp::u64 offset, const dp::Vector<dp::u8> &data, bool fin) {
            if (!can_recv() && recv_state_ != RecvState::DataRecvd) {
                return;
            }

            recv_buffer_.insert(offset, data);

            if (fin) {
                recv_buffer_.set_fin(offset + data.size());
                if (recv_state_ == RecvState::Recv) {
                    recv_state_ = RecvState::SizeKnown;
                }
            }

            if (recv_buffer_.all_received() && recv_state_ == RecvState::SizeKnown) {
                recv_state_ = RecvState::DataRecvd;
            }
        }

        dp::Vector<dp::u8> read(dp::usize max_len = 0) {
            auto data = recv_buffer_.read(max_len);

            if (recv_buffer_.all_read() && recv_state_ == RecvState::DataRecvd) {
                recv_state_ = RecvState::DataRead;
            }

            return data;
        }

        void receive_reset(dp::u64 error_code, dp::u64 final_size) {
            if (recv_state_ == RecvState::DataRead || recv_state_ == RecvState::ResetRead) {
                return;
            }
            remote_reset_error_code_ = error_code;
            recv_buffer_.set_fin(final_size);
            recv_state_ = RecvState::ResetRecvd;
        }

        void mark_reset_read() {
            if (recv_state_ == RecvState::ResetRecvd) {
                recv_state_ = RecvState::ResetRead;
            }
        }

        // Flow control
        void set_max_stream_data_local(dp::u64 max) { max_stream_data_local_ = max; }
        void set_max_stream_data_remote(dp::u64 max) { max_stream_data_remote_ = max; }
        dp::u64 max_stream_data_local() const { return max_stream_data_local_; }
        dp::u64 max_stream_data_remote() const { return max_stream_data_remote_; }

        // Check if stream is fully closed
        bool is_closed() const {
            bool send_done = (send_state_ == SendState::DataRecvd || send_state_ == SendState::ResetRecvd ||
                              (!netpipe::quic::is_bidirectional(stream_id_) && !is_local_));
            bool recv_done = (recv_state_ == RecvState::DataRead || recv_state_ == RecvState::ResetRead ||
                              (!netpipe::quic::is_bidirectional(stream_id_) && is_local_));
            return send_done && recv_done;
        }

        // Error codes
        dp::u64 reset_error_code() const { return reset_error_code_; }
        dp::u64 remote_reset_error_code() const { return remote_reset_error_code_; }

      private:
        dp::u64 stream_id_;
        StreamType type_;
        bool is_local_;

        SendState send_state_ = SendState::Ready;
        RecvState recv_state_ = RecvState::Recv;

        SendBuffer send_buffer_;
        RecvBuffer recv_buffer_;

        dp::u64 max_stream_data_local_ = 0;
        dp::u64 max_stream_data_remote_ = 0;

        dp::u64 reset_error_code_ = 0;
        dp::u64 remote_reset_error_code_ = 0;
    };

    // Stream manager for a connection
    class StreamManager {
      public:
        StreamManager(bool is_client) : is_client_(is_client) {}

        // Create a new local stream
        dp::Res<dp::u64> create_stream(bool bidirectional) {
            dp::u64 stream_id;

            if (bidirectional) {
                if (next_bidi_stream_id_ / 4 >= max_bidi_streams_) {
                    return dp::result::err(dp::Error::io_error("stream limit reached"));
                }
                stream_id = next_bidi_stream_id_;
                next_bidi_stream_id_ += 4;
            } else {
                if (next_uni_stream_id_ / 4 >= max_uni_streams_) {
                    return dp::result::err(dp::Error::io_error("stream limit reached"));
                }
                stream_id = next_uni_stream_id_;
                next_uni_stream_id_ += 4;
            }

            streams_.emplace(stream_id, QuicStreamState(stream_id, true));
            return dp::result::ok(stream_id);
        }

        // Get or create a stream (for incoming frames)
        QuicStreamState *get_or_create(dp::u64 stream_id) {
            auto it = streams_.find(stream_id);
            if (it != streams_.end()) {
                return &it->second;
            }

            // Check if this is a valid remote stream
            bool is_client_initiated = netpipe::quic::is_client_initiated(stream_id);
            bool is_local = (is_client_ == is_client_initiated);

            if (is_local) {
                // We should have created this stream
                return nullptr;
            }

            // Create remote stream
            auto [new_it, inserted] = streams_.emplace(stream_id, QuicStreamState(stream_id, false));
            return &new_it->second;
        }

        // Get an existing stream
        QuicStreamState *get(dp::u64 stream_id) {
            auto it = streams_.find(stream_id);
            return it != streams_.end() ? &it->second : nullptr;
        }

        const QuicStreamState *get(dp::u64 stream_id) const {
            auto it = streams_.find(stream_id);
            return it != streams_.end() ? &it->second : nullptr;
        }

        // Remove closed streams
        void cleanup_closed() {
            for (auto it = streams_.begin(); it != streams_.end();) {
                if (it->second.is_closed()) {
                    it = streams_.erase(it);
                } else {
                    ++it;
                }
            }
        }

        // Set stream limits from transport parameters
        void set_max_bidi_streams(dp::u64 max) { max_bidi_streams_ = max; }
        void set_max_uni_streams(dp::u64 max) { max_uni_streams_ = max; }

        dp::u64 max_bidi_streams() const { return max_bidi_streams_; }
        dp::u64 max_uni_streams() const { return max_uni_streams_; }

        // Get all streams
        const std::map<dp::u64, QuicStreamState> &streams() const { return streams_; }
        std::map<dp::u64, QuicStreamState> &streams() { return streams_; }

        // Count active streams
        dp::usize count() const { return streams_.size(); }

        dp::usize count_bidi() const {
            dp::usize count = 0;
            for (const auto &[id, stream] : streams_) {
                if (stream.is_bidirectional()) {
                    count++;
                }
            }
            return count;
        }

        dp::usize count_uni() const {
            dp::usize count = 0;
            for (const auto &[id, stream] : streams_) {
                if (!stream.is_bidirectional()) {
                    count++;
                }
            }
            return count;
        }

      private:
        bool is_client_;
        std::map<dp::u64, QuicStreamState> streams_;

        // Next stream IDs
        dp::u64 next_bidi_stream_id_ = 0; // Will be set based on client/server
        dp::u64 next_uni_stream_id_ = 2;  // Will be set based on client/server

        // Stream limits from peer
        dp::u64 max_bidi_streams_ = 0;
        dp::u64 max_uni_streams_ = 0;
    };

} // namespace netpipe::quic

#pragma once

#include <datapod/datapod.hpp>
#include <echo/echo.hpp>
#include <netpipe/transport/stream/quic/types.hpp>

namespace netpipe::quic {

    // Flow control for a single stream
    class StreamFlowControl {
      public:
        StreamFlowControl(dp::u64 stream_id, dp::u64 initial_max_data, bool is_local)
            : stream_id_(stream_id), max_data_(initial_max_data), is_local_(is_local) {}

        // Get stream ID
        dp::u64 stream_id() const { return stream_id_; }

        // === Send side (data we're sending) ===

        // Get maximum data we're allowed to send
        dp::u64 send_limit() const { return max_data_; }

        // Get how much data we've sent
        dp::u64 data_sent() const { return data_sent_; }

        // Get available send window
        dp::u64 send_window() const {
            if (data_sent_ >= max_data_) {
                return 0;
            }
            return max_data_ - data_sent_;
        }

        // Check if we're blocked on send
        bool is_send_blocked() const { return data_sent_ >= max_data_; }

        // Record data being sent
        bool on_data_sent(dp::u64 bytes) {
            data_sent_ += bytes;
            if (data_sent_ > max_data_) {
                echo::warn("Stream ", stream_id_, " sent data exceeds limit: ", data_sent_, " > ", max_data_);
                return false;
            }
            return true;
        }

        // Update send limit (from MAX_STREAM_DATA frame)
        void update_send_limit(dp::u64 new_limit) {
            if (new_limit > max_data_) {
                echo::trace("Stream ", stream_id_, " send limit updated: ", max_data_, " -> ", new_limit);
                max_data_ = new_limit;
            }
        }

        // === Receive side (data we're receiving) ===

        // Get maximum data we'll accept
        dp::u64 recv_limit() const { return max_recv_data_; }

        // Get how much data we've received
        dp::u64 data_received() const { return data_received_; }

        // Get available receive window
        dp::u64 recv_window() const {
            if (data_received_ >= max_recv_data_) {
                return 0;
            }
            return max_recv_data_ - data_received_;
        }

        // Record data being received
        // Returns false if this would violate flow control
        bool on_data_received(dp::u64 offset, dp::u64 bytes) {
            dp::u64 end_offset = offset + bytes;

            // Update high water mark
            if (end_offset > highest_received_offset_) {
                highest_received_offset_ = end_offset;
            }

            // Check against limit
            if (highest_received_offset_ > max_recv_data_) {
                echo::error("Stream ", stream_id_, " flow control violation: ", highest_received_offset_, " > ",
                            max_recv_data_);
                return false;
            }

            data_received_ += bytes;
            return true;
        }

        // Update local receive limit (we're increasing our window)
        void update_recv_limit(dp::u64 new_limit) {
            if (new_limit > max_recv_data_) {
                max_recv_data_ = new_limit;
                echo::trace("Stream ", stream_id_, " recv limit updated to ", max_recv_data_);
            }
        }

        // Check if we should send MAX_STREAM_DATA
        bool should_send_max_stream_data() const {
            // Send update when window is less than half of maximum
            if (max_recv_data_ == 0) {
                return false;
            }
            return recv_window() < max_recv_data_ / 2;
        }

        // Get new limit to advertise (doubles current limit)
        dp::u64 get_new_recv_limit() const { return max_recv_data_ * 2; }

      private:
        dp::u64 stream_id_;
        bool is_local_; // True if we initiated this stream

        // Send side
        dp::u64 max_data_;      // Peer's advertised limit for this stream
        dp::u64 data_sent_ = 0; // Total data we've sent

        // Receive side
        dp::u64 max_recv_data_ = 0;           // Our limit for peer
        dp::u64 data_received_ = 0;           // Total data received
        dp::u64 highest_received_offset_ = 0; // Highest offset seen
    };

    // Connection-level flow control
    class ConnectionFlowControl {
      public:
        ConnectionFlowControl() = default;

        // Initialize with transport parameters
        void init(dp::u64 local_max_data, dp::u64 peer_max_data) {
            local_max_data_ = local_max_data;
            peer_max_data_ = peer_max_data;
            echo::debug("Flow control initialized: local_max=", local_max_data_, " peer_max=", peer_max_data_);
        }

        // === Send side (aggregate data we're sending) ===

        // Get maximum data we're allowed to send (peer's limit)
        dp::u64 send_limit() const { return peer_max_data_; }

        // Get how much data we've sent
        dp::u64 data_sent() const { return data_sent_; }

        // Get available send window
        dp::u64 send_window() const {
            if (data_sent_ >= peer_max_data_) {
                return 0;
            }
            return peer_max_data_ - data_sent_;
        }

        // Check if connection is blocked on send
        bool is_send_blocked() const { return data_sent_ >= peer_max_data_; }

        // Record data being sent
        bool on_data_sent(dp::u64 bytes) {
            data_sent_ += bytes;
            if (data_sent_ > peer_max_data_) {
                echo::warn("Connection send data exceeds limit: ", data_sent_, " > ", peer_max_data_);
                return false;
            }
            return true;
        }

        // Update send limit (from MAX_DATA frame)
        void update_send_limit(dp::u64 new_limit) {
            if (new_limit > peer_max_data_) {
                echo::trace("Connection send limit updated: ", peer_max_data_, " -> ", new_limit);
                peer_max_data_ = new_limit;
            }
        }

        // === Receive side (aggregate data we're receiving) ===

        // Get maximum data we'll accept
        dp::u64 recv_limit() const { return local_max_data_; }

        // Get how much data we've received
        dp::u64 data_received() const { return data_received_; }

        // Get available receive window
        dp::u64 recv_window() const {
            if (data_received_ >= local_max_data_) {
                return 0;
            }
            return local_max_data_ - data_received_;
        }

        // Record data being received
        bool on_data_received(dp::u64 bytes) {
            data_received_ += bytes;
            if (data_received_ > local_max_data_) {
                echo::error("Connection flow control violation: ", data_received_, " > ", local_max_data_);
                return false;
            }
            return true;
        }

        // Update local receive limit (we're increasing our window)
        void update_recv_limit(dp::u64 new_limit) {
            if (new_limit > local_max_data_) {
                local_max_data_ = new_limit;
                echo::trace("Connection recv limit updated to ", local_max_data_);
            }
        }

        // Check if we should send MAX_DATA
        bool should_send_max_data() const {
            if (local_max_data_ == 0) {
                return false;
            }
            return recv_window() < local_max_data_ / 2;
        }

        // Get new limit to advertise
        dp::u64 get_new_recv_limit() const { return local_max_data_ * 2; }

      private:
        // Peer's limit (how much we can send)
        dp::u64 peer_max_data_ = 0;
        dp::u64 data_sent_ = 0;

        // Our limit (how much peer can send)
        dp::u64 local_max_data_ = 0;
        dp::u64 data_received_ = 0;
    };

    // Stream limit manager (MAX_STREAMS)
    class StreamLimitManager {
      public:
        StreamLimitManager(bool is_client) : is_client_(is_client) {}

        // Initialize with transport parameters
        void init(dp::u64 local_max_bidi, dp::u64 local_max_uni, dp::u64 peer_max_bidi, dp::u64 peer_max_uni) {
            local_max_bidi_ = local_max_bidi;
            local_max_uni_ = local_max_uni;
            peer_max_bidi_ = peer_max_bidi;
            peer_max_uni_ = peer_max_uni;

            echo::debug("Stream limits initialized: local_bidi=", local_max_bidi_, " local_uni=", local_max_uni_,
                        " peer_bidi=", peer_max_bidi_, " peer_uni=", peer_max_uni_);
        }

        // Check if we can create a new stream
        bool can_create_stream(bool bidirectional) const {
            if (bidirectional) {
                return local_bidi_created_ < peer_max_bidi_;
            } else {
                return local_uni_created_ < peer_max_uni_;
            }
        }

        // Record stream creation
        void on_stream_created(bool bidirectional) {
            if (bidirectional) {
                local_bidi_created_++;
            } else {
                local_uni_created_++;
            }
        }

        // Check if peer can create a stream (for validation)
        bool peer_can_create_stream(bool bidirectional) const {
            if (bidirectional) {
                return remote_bidi_opened_ < local_max_bidi_;
            } else {
                return remote_uni_opened_ < local_max_uni_;
            }
        }

        // Record peer stream opening
        bool on_peer_stream_opened(dp::u64 stream_id) {
            bool is_bidi = netpipe::quic::is_bidirectional(stream_id);
            bool is_peer_initiated = is_client_ ? !is_client_initiated(stream_id) : is_client_initiated(stream_id);

            if (!is_peer_initiated) {
                return true; // Not a peer-initiated stream
            }

            if (is_bidi) {
                remote_bidi_opened_++;
                if (remote_bidi_opened_ > local_max_bidi_) {
                    echo::error("Peer exceeded bidirectional stream limit");
                    return false;
                }
            } else {
                remote_uni_opened_++;
                if (remote_uni_opened_ > local_max_uni_) {
                    echo::error("Peer exceeded unidirectional stream limit");
                    return false;
                }
            }

            return true;
        }

        // Update peer's stream limits (from MAX_STREAMS frame)
        void update_peer_limit(bool bidirectional, dp::u64 new_limit) {
            if (bidirectional) {
                if (new_limit > peer_max_bidi_) {
                    peer_max_bidi_ = new_limit;
                    echo::trace("Peer bidi stream limit updated to ", peer_max_bidi_);
                }
            } else {
                if (new_limit > peer_max_uni_) {
                    peer_max_uni_ = new_limit;
                    echo::trace("Peer uni stream limit updated to ", peer_max_uni_);
                }
            }
        }

        // Check if we should send MAX_STREAMS
        bool should_send_max_streams(bool bidirectional) const {
            if (bidirectional) {
                return local_max_bidi_ > 0 && remote_bidi_opened_ >= local_max_bidi_ / 2;
            } else {
                return local_max_uni_ > 0 && remote_uni_opened_ >= local_max_uni_ / 2;
            }
        }

        // Get new limit to advertise
        dp::u64 get_new_limit(bool bidirectional) const {
            if (bidirectional) {
                return local_max_bidi_ * 2;
            } else {
                return local_max_uni_ * 2;
            }
        }

        // Update local limits
        void update_local_limit(bool bidirectional, dp::u64 new_limit) {
            if (bidirectional) {
                local_max_bidi_ = new_limit;
            } else {
                local_max_uni_ = new_limit;
            }
        }

        // Getters
        dp::u64 local_max_bidi() const { return local_max_bidi_; }
        dp::u64 local_max_uni() const { return local_max_uni_; }
        dp::u64 peer_max_bidi() const { return peer_max_bidi_; }
        dp::u64 peer_max_uni() const { return peer_max_uni_; }

      private:
        bool is_client_;

        // Limits peer advertised to us (how many we can create)
        dp::u64 peer_max_bidi_ = 0;
        dp::u64 peer_max_uni_ = 0;
        dp::u64 local_bidi_created_ = 0;
        dp::u64 local_uni_created_ = 0;

        // Limits we advertised (how many peer can create)
        dp::u64 local_max_bidi_ = 0;
        dp::u64 local_max_uni_ = 0;
        dp::u64 remote_bidi_opened_ = 0;
        dp::u64 remote_uni_opened_ = 0;
    };

    // Combined flow control manager
    class FlowControlManager {
      public:
        FlowControlManager(bool is_client) : stream_limits_(is_client), is_client_(is_client) {}

        // Initialize from transport parameters
        void init_from_transport_params(dp::u64 local_max_data, dp::u64 peer_max_data,
                                        dp::u64 local_max_stream_data_bidi_local,
                                        dp::u64 local_max_stream_data_bidi_remote, dp::u64 local_max_stream_data_uni,
                                        dp::u64 peer_max_stream_data_bidi_local,
                                        dp::u64 peer_max_stream_data_bidi_remote, dp::u64 peer_max_stream_data_uni,
                                        dp::u64 local_max_streams_bidi, dp::u64 local_max_streams_uni,
                                        dp::u64 peer_max_streams_bidi, dp::u64 peer_max_streams_uni) {
            connection_fc_.init(local_max_data, peer_max_data);

            // Store stream data limits for creating new streams
            local_max_stream_data_bidi_local_ = local_max_stream_data_bidi_local;
            local_max_stream_data_bidi_remote_ = local_max_stream_data_bidi_remote;
            local_max_stream_data_uni_ = local_max_stream_data_uni;
            peer_max_stream_data_bidi_local_ = peer_max_stream_data_bidi_local;
            peer_max_stream_data_bidi_remote_ = peer_max_stream_data_bidi_remote;
            peer_max_stream_data_uni_ = peer_max_stream_data_uni;

            stream_limits_.init(local_max_streams_bidi, local_max_streams_uni, peer_max_streams_bidi,
                                peer_max_streams_uni);
        }

        // Get connection flow control
        ConnectionFlowControl &connection() { return connection_fc_; }
        const ConnectionFlowControl &connection() const { return connection_fc_; }

        // Get stream limits
        StreamLimitManager &stream_limits() { return stream_limits_; }
        const StreamLimitManager &stream_limits() const { return stream_limits_; }

        // Get initial send limit for a new stream
        dp::u64 initial_stream_send_limit(dp::u64 stream_id) const {
            bool is_local = is_client_ ? is_client_initiated(stream_id) : !is_client_initiated(stream_id);
            bool is_bidi = netpipe::quic::is_bidirectional(stream_id);

            if (is_bidi) {
                // Bidirectional stream
                if (is_local) {
                    // We initiated: peer's "bidi_remote" limit applies
                    return peer_max_stream_data_bidi_remote_;
                } else {
                    // Peer initiated: peer's "bidi_local" limit applies
                    return peer_max_stream_data_bidi_local_;
                }
            } else {
                // Unidirectional stream
                if (is_local) {
                    // We initiated: peer's uni limit applies
                    return peer_max_stream_data_uni_;
                } else {
                    // Peer initiated unidirectional to us: we can't send on it
                    return 0;
                }
            }
        }

        // Get initial receive limit for a new stream
        dp::u64 initial_stream_recv_limit(dp::u64 stream_id) const {
            bool is_local = is_client_ ? is_client_initiated(stream_id) : !is_client_initiated(stream_id);
            bool is_bidi = netpipe::quic::is_bidirectional(stream_id);

            if (is_bidi) {
                if (is_local) {
                    // We initiated: our "bidi_local" limit
                    return local_max_stream_data_bidi_local_;
                } else {
                    // Peer initiated: our "bidi_remote" limit
                    return local_max_stream_data_bidi_remote_;
                }
            } else {
                if (is_local) {
                    // We initiated unidirectional: peer can't send on it
                    return 0;
                } else {
                    // Peer initiated: our uni limit
                    return local_max_stream_data_uni_;
                }
            }
        }

        // Validate that sending bytes won't violate flow control
        // Returns error string if violation would occur, empty string if OK
        dp::String validate_send(dp::u64 bytes) const {
            if (connection_fc_.data_sent() + bytes > connection_fc_.send_limit()) {
                return "connection flow control limit exceeded";
            }
            return "";
        }

        // Validate that receiving bytes won't violate flow control
        dp::String validate_recv(dp::u64 bytes) const {
            if (connection_fc_.data_received() + bytes > connection_fc_.recv_limit()) {
                return "connection flow control limit exceeded";
            }
            return "";
        }

      private:
        ConnectionFlowControl connection_fc_;
        StreamLimitManager stream_limits_;
        bool is_client_;

        // Stream data limits (from transport parameters)
        dp::u64 local_max_stream_data_bidi_local_ = 0;
        dp::u64 local_max_stream_data_bidi_remote_ = 0;
        dp::u64 local_max_stream_data_uni_ = 0;
        dp::u64 peer_max_stream_data_bidi_local_ = 0;
        dp::u64 peer_max_stream_data_bidi_remote_ = 0;
        dp::u64 peer_max_stream_data_uni_ = 0;
    };

} // namespace netpipe::quic

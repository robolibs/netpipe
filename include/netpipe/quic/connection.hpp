#pragma once

#include <array>
#include <chrono>
#include <datapod/datapod.hpp>
#include <deque>
#include <echo/echo.hpp>
#include <keylock/utils/common.hpp>
#include <netpipe/quic/ack_manager.hpp>
#include <netpipe/quic/congestion_control.hpp>
#include <netpipe/quic/crypto.hpp>
#include <netpipe/quic/flow_control.hpp>
#include <netpipe/quic/frame.hpp>
#include <netpipe/quic/loss_detection.hpp>
#include <netpipe/quic/packet.hpp>
#include <netpipe/quic/stream.hpp>
#include <netpipe/quic/transport_params.hpp>
#include <netpipe/quic/types.hpp>

namespace netpipe::quic {

    // Connection states
    enum class ConnectionState {
        Idle,        // Not yet started
        Handshaking, // Performing TLS handshake
        Connected,   // Handshake complete, application data can flow
        Draining,    // Connection close initiated, draining period
        Closed,      // Connection fully closed
    };

    // Packet number space state
    struct PacketNumberSpaceState {
        dp::u64 next_packet_number = 0;
        dp::u64 largest_acked_packet = 0;
        dp::u64 largest_received_packet = 0;

        // Crypto state for this space
        CryptoState send_crypto;
        CryptoState recv_crypto;

        bool has_keys() const { return send_crypto.has_keys() && recv_crypto.has_keys(); }
    };

    // Sent packet info (for loss detection)
    struct SentPacket {
        dp::u64 packet_number;
        PacketNumberSpace space;
        dp::Vector<dp::u8> data;
        std::chrono::steady_clock::time_point sent_time;
        dp::usize sent_bytes;
        bool ack_eliciting = false;
        bool in_flight = false;

        // Frames in this packet (for retransmission)
        dp::Vector<dp::u64> stream_frames; // stream_id for each STREAM frame
    };

    // Connection configuration
    struct ConnectionConfig {
        // Local transport parameters
        TransportParameters local_params;

        // Maximum packet size
        dp::usize max_packet_size = MIN_INITIAL_PACKET_SIZE;

        // Idle timeout
        std::chrono::milliseconds idle_timeout{30000};

        // Enable 0-RTT
        bool enable_0rtt = false;
    };

    // QUIC Connection
    class Connection {
      public:
        Connection(bool is_client, const ConnectionConfig &config = {})
            : is_client_(is_client), config_(config), stream_manager_(is_client), flow_control_(is_client) {
            // Initialize packet number spaces
            spaces_[static_cast<int>(PacketNumberSpace::Initial)] = PacketNumberSpaceState{};
            spaces_[static_cast<int>(PacketNumberSpace::Handshake)] = PacketNumberSpaceState{};
            spaces_[static_cast<int>(PacketNumberSpace::ApplicationData)] = PacketNumberSpaceState{};

            // Initialize loss detection
            loss_detection_.reset();

            // Set initial stream IDs based on role
            if (is_client) {
                // Client: bidi=0, uni=2
            } else {
                // Server: bidi=1, uni=3
            }

            echo::trace("QUIC connection created (", is_client ? "client" : "server", ")");
        }

        // Get connection state
        ConnectionState state() const { return state_; }
        bool is_client() const { return is_client_; }
        bool is_connected() const { return state_ == ConnectionState::Connected; }
        bool is_closed() const { return state_ == ConnectionState::Closed || state_ == ConnectionState::Draining; }

        // Connection IDs
        void set_local_cid(const ConnectionId &cid) {
            local_cid_ = cid;
            // Add to issued CIDs with sequence 0
            IssuedConnectionId issued;
            issued.cid = cid;
            issued.sequence = 0;
            issued.stateless_reset_token = generate_stateless_reset_token();
            issued_cids_.push_back(issued);
            next_cid_sequence_ = 1;
        }
        void set_remote_cid(const ConnectionId &cid) {
            remote_cid_ = cid;
            // Add to peer CIDs with sequence 0
            PeerConnectionId peer;
            peer.cid = cid;
            peer.sequence = 0;
            peer_cids_.push_back(peer);
            active_peer_cid_sequence_ = 0;
        }
        void set_original_dcid(const ConnectionId &cid) { original_dcid_ = cid; }

        const ConnectionId &local_cid() const { return local_cid_; }
        const ConnectionId &remote_cid() const { return remote_cid_; }
        const ConnectionId &original_dcid() const { return original_dcid_; }

        // === Connection ID Management ===

        // Issue a new connection ID to peer
        // Returns the NEW_CONNECTION_ID frame to send
        NewConnectionIdFrame issue_new_connection_id(dp::u64 retire_prior_to = 0) {
            IssuedConnectionId issued;
            issued.cid = ConnectionId::generate();
            issued.sequence = next_cid_sequence_++;
            issued.stateless_reset_token = generate_stateless_reset_token();
            issued_cids_.push_back(issued);

            NewConnectionIdFrame frame;
            frame.sequence_number = issued.sequence;
            frame.retire_prior_to = retire_prior_to;
            frame.connection_id = issued.cid;
            frame.stateless_reset_token = issued.stateless_reset_token;

            echo::debug("Issued new connection ID: seq=", issued.sequence);
            return frame;
        }

        // Retire a connection ID
        RetireConnectionIdFrame retire_connection_id(dp::u64 sequence) {
            RetireConnectionIdFrame frame;
            frame.sequence_number = sequence;

            // Remove from peer CIDs
            peer_cids_.erase(std::remove_if(peer_cids_.begin(), peer_cids_.end(),
                                            [sequence](const PeerConnectionId &p) { return p.sequence == sequence; }),
                             peer_cids_.end());

            echo::debug("Retired connection ID: seq=", sequence);
            return frame;
        }

        // Process NEW_CONNECTION_ID from peer
        void process_new_connection_id(const NewConnectionIdFrame &frame) {
            // Retire old CIDs as requested
            if (frame.retire_prior_to > 0) {
                for (auto it = peer_cids_.begin(); it != peer_cids_.end();) {
                    if (it->sequence < frame.retire_prior_to) {
                        pending_retire_cids_.push_back(it->sequence);
                        it = peer_cids_.erase(it);
                    } else {
                        ++it;
                    }
                }
            }

            // Add the new CID
            PeerConnectionId peer;
            peer.cid = frame.connection_id;
            peer.sequence = frame.sequence_number;
            peer.stateless_reset_token = frame.stateless_reset_token;
            peer_cids_.push_back(peer);

            echo::debug("Received new connection ID from peer: seq=", frame.sequence_number);
        }

        // Process RETIRE_CONNECTION_ID from peer
        void process_retire_connection_id(const RetireConnectionIdFrame &frame) {
            // Remove from issued CIDs
            issued_cids_.erase(
                std::remove_if(issued_cids_.begin(), issued_cids_.end(),
                               [&frame](const IssuedConnectionId &i) { return i.sequence == frame.sequence_number; }),
                issued_cids_.end());

            echo::debug("Peer retired connection ID: seq=", frame.sequence_number);
        }

        // Switch to a different peer connection ID (for migration)
        bool switch_to_peer_cid(dp::u64 sequence) {
            for (const auto &peer : peer_cids_) {
                if (peer.sequence == sequence) {
                    remote_cid_ = peer.cid;
                    active_peer_cid_sequence_ = sequence;
                    echo::debug("Switched to peer CID: seq=", sequence);
                    return true;
                }
            }
            return false;
        }

        // Get next available peer CID (for migration)
        dp::Optional<dp::u64> next_available_peer_cid() const {
            for (const auto &peer : peer_cids_) {
                if (peer.sequence > active_peer_cid_sequence_) {
                    return peer.sequence;
                }
            }
            return {};
        }

        // Check if there are CIDs to retire
        bool has_pending_retire_cids() const { return !pending_retire_cids_.empty(); }

        // Get CIDs that need RETIRE_CONNECTION_ID frames
        dp::Vector<dp::u64> take_pending_retire_cids() {
            auto result = std::move(pending_retire_cids_);
            pending_retire_cids_.clear();
            return result;
        }

        // Get number of available peer CIDs
        dp::usize available_peer_cids() const { return peer_cids_.size(); }

        // Get number of issued CIDs
        dp::usize issued_cids_count() const { return issued_cids_.size(); }

        // Transport parameters
        void set_local_params(const TransportParameters &params) {
            config_.local_params = params;
            config_.local_params.initial_source_cid = local_cid_;
        }

        void set_remote_params(const TransportParameters &params) {
            remote_params_ = params;

            // Apply stream limits
            stream_manager_.set_max_bidi_streams(params.initial_max_streams_bidi);
            stream_manager_.set_max_uni_streams(params.initial_max_streams_uni);

            // Apply flow control limits
            max_data_remote_ = params.initial_max_data;
        }

        const TransportParameters &local_params() const { return config_.local_params; }
        const TransportParameters &remote_params() const { return remote_params_; }

        // Set keys for a packet number space
        void set_initial_keys(const ConnectionId &dcid) {
            auto [client_secret, server_secret] = derive_initial_secrets(dcid, version_);

            auto client_keys = derive_keys(client_secret);
            auto server_keys = derive_keys(server_secret);

            auto &initial_space = spaces_[static_cast<int>(PacketNumberSpace::Initial)];

            if (is_client_) {
                initial_space.send_crypto.set_keys(client_keys);
                initial_space.recv_crypto.set_keys(server_keys);
            } else {
                initial_space.send_crypto.set_keys(server_keys);
                initial_space.recv_crypto.set_keys(client_keys);
            }

            echo::debug("Initial keys installed");
        }

        void set_handshake_keys(const QuicKeys &send_keys, const QuicKeys &recv_keys) {
            auto &space = spaces_[static_cast<int>(PacketNumberSpace::Handshake)];
            space.send_crypto.set_keys(send_keys);
            space.recv_crypto.set_keys(recv_keys);
            echo::debug("Handshake keys installed");
        }

        void set_application_keys(const QuicKeys &send_keys, const QuicKeys &recv_keys) {
            auto &space = spaces_[static_cast<int>(PacketNumberSpace::ApplicationData)];
            space.send_crypto.set_keys(send_keys);
            space.recv_crypto.set_keys(recv_keys);
            echo::debug("Application keys installed");
        }

        bool has_keys(PacketNumberSpace space) const { return spaces_[static_cast<int>(space)].has_keys(); }

        // Start handshake (client)
        dp::Res<dp::Vector<dp::u8>> start_handshake() {
            if (!is_client_ || state_ != ConnectionState::Idle) {
                return dp::result::err(dp::Error::invalid_argument("cannot start handshake"));
            }

            state_ = ConnectionState::Handshaking;

            // Generate connection IDs
            if (local_cid_.empty()) {
                local_cid_ = ConnectionId::generate();
            }
            if (remote_cid_.empty()) {
                remote_cid_ = ConnectionId::generate(); // Will be replaced by server's choice
            }
            original_dcid_ = remote_cid_;

            // Install initial keys based on destination CID
            set_initial_keys(original_dcid_);

            // The actual TLS ClientHello will be provided by the TLS layer
            // For now, return empty - caller should add CRYPTO frames
            return dp::result::ok(dp::Vector<dp::u8>{});
        }

        // Accept handshake (server) - called when Initial packet received
        dp::Res<void> accept_handshake(const ConnectionId &dcid, const ConnectionId &scid) {
            if (is_client_ || state_ != ConnectionState::Idle) {
                return dp::result::err(dp::Error::invalid_argument("cannot accept handshake"));
            }

            state_ = ConnectionState::Handshaking;

            // Set connection IDs
            original_dcid_ = dcid;
            remote_cid_ = scid;
            if (local_cid_.empty()) {
                local_cid_ = ConnectionId::generate();
            }

            // Install initial keys based on client's destination CID
            set_initial_keys(original_dcid_);

            return dp::result::ok();
        }

        // Mark handshake as complete
        void handshake_complete() {
            if (state_ == ConnectionState::Handshaking) {
                state_ = ConnectionState::Connected;
                echo::info("QUIC handshake complete");

                // Discard Initial and Handshake keys
                spaces_[static_cast<int>(PacketNumberSpace::Initial)] = PacketNumberSpaceState{};
                spaces_[static_cast<int>(PacketNumberSpace::Handshake)] = PacketNumberSpaceState{};
            }
        }

        // Stream management
        StreamManager &streams() { return stream_manager_; }
        const StreamManager &streams() const { return stream_manager_; }

        dp::Res<dp::u64> create_stream(bool bidirectional = true) {
            if (!is_connected()) {
                return dp::result::err(dp::Error::invalid_argument("connection not established"));
            }
            return stream_manager_.create_stream(bidirectional);
        }

        // Write data to a stream
        dp::Res<void> stream_write(dp::u64 stream_id, const dp::Vector<dp::u8> &data) {
            auto *stream = stream_manager_.get(stream_id);
            if (!stream) {
                return dp::result::err(dp::Error::not_found("stream not found"));
            }

            if (!stream->can_send()) {
                return dp::result::err(dp::Error::invalid_argument("stream cannot send"));
            }

            stream->write(data);
            return dp::result::ok();
        }

        // Finish a stream (send FIN)
        dp::Res<void> stream_finish(dp::u64 stream_id) {
            auto *stream = stream_manager_.get(stream_id);
            if (!stream) {
                return dp::result::err(dp::Error::not_found("stream not found"));
            }

            stream->finish();
            return dp::result::ok();
        }

        // Read data from a stream
        dp::Res<dp::Vector<dp::u8>> stream_read(dp::u64 stream_id, dp::usize max_len = 0) {
            auto *stream = stream_manager_.get(stream_id);
            if (!stream) {
                return dp::result::err(dp::Error::not_found("stream not found"));
            }

            return dp::result::ok(stream->read(max_len));
        }

        // Build an Initial packet
        dp::Res<dp::Vector<dp::u8>> build_initial_packet(const dp::Vector<dp::u8> &crypto_data,
                                                         const dp::Vector<dp::u8> &token = {}) {
            auto &space = spaces_[static_cast<int>(PacketNumberSpace::Initial)];
            if (!space.has_keys()) {
                return dp::result::err(dp::Error::invalid_argument("no initial keys"));
            }

            // Build Initial header
            InitialPacket packet;
            packet.header.packet_type = LongPacketType::Initial;
            packet.header.version = version_;
            packet.header.dest_cid = remote_cid_;
            packet.header.src_cid = local_cid_;
            packet.token = token;
            packet.packet_number = space.next_packet_number++;

            // Determine packet number length
            dp::u8 pn_length = packet_number_length(packet.packet_number, space.largest_acked_packet);
            packet.header.pn_length = pn_length;

            // Build payload (CRYPTO frame)
            CryptoFrame crypto_frame;
            crypto_frame.offset = 0; // Simplified - should track offset
            crypto_frame.data = crypto_data;
            auto payload = crypto_frame.serialize();

            // Build header
            auto header = packet.serialize_header_and_token();

            // Add length field (varint)
            dp::usize payload_len = (pn_length + 1) + payload.size() + AEAD_TAG_LENGTH;
            auto length_bytes = varint_encode(payload_len);
            header.insert(header.end(), length_bytes.begin(), length_bytes.end());

            // Add packet number
            auto pn_bytes = encode_packet_number(packet.packet_number, pn_length);
            dp::usize pn_offset = header.size();
            header.insert(header.end(), pn_bytes.begin(), pn_bytes.end());

            // Encrypt and protect
            auto result = space.send_crypto.encrypt_packet(std::move(header), payload, packet.packet_number, pn_offset,
                                                           pn_length + 1);
            if (result.is_err()) {
                return dp::result::err(result.error());
            }

            auto final_packet = result.value();

            // Pad to minimum size
            if (final_packet.size() < MIN_INITIAL_PACKET_SIZE) {
                final_packet.resize(MIN_INITIAL_PACKET_SIZE, 0);
            }

            // Track sent packet for loss detection
            loss_detection_.on_packet_sent(PacketNumberSpace::Initial, packet.packet_number, final_packet.size(),
                                           true /* ack_eliciting */, true /* crypto_data */, false /* stream_data */);

            // Update congestion control
            congestion_control_.on_packet_sent(final_packet.size());

            return dp::result::ok(std::move(final_packet));
        }

        // Build a Handshake packet
        dp::Res<dp::Vector<dp::u8>> build_handshake_packet(const dp::Vector<dp::u8> &crypto_data) {
            auto &space = spaces_[static_cast<int>(PacketNumberSpace::Handshake)];
            if (!space.has_keys()) {
                return dp::result::err(dp::Error::invalid_argument("no handshake keys"));
            }

            // Build Handshake header
            LongHeader header;
            header.packet_type = LongPacketType::Handshake;
            header.version = version_;
            header.dest_cid = remote_cid_;
            header.src_cid = local_cid_;

            dp::u64 pn = space.next_packet_number++;
            dp::u8 pn_length = packet_number_length(pn, space.largest_acked_packet);
            header.pn_length = pn_length;

            // Build payload
            CryptoFrame crypto_frame;
            crypto_frame.offset = 0;
            crypto_frame.data = crypto_data;
            auto payload = crypto_frame.serialize();

            // Serialize header
            auto header_bytes = header.serialize();

            // Add length field
            dp::usize payload_len = (pn_length + 1) + payload.size() + AEAD_TAG_LENGTH;
            auto length_bytes = varint_encode(payload_len);
            header_bytes.insert(header_bytes.end(), length_bytes.begin(), length_bytes.end());

            // Add packet number
            auto pn_bytes = encode_packet_number(pn, pn_length);
            dp::usize pn_offset = header_bytes.size();
            header_bytes.insert(header_bytes.end(), pn_bytes.begin(), pn_bytes.end());

            // Encrypt
            auto result =
                space.send_crypto.encrypt_packet(std::move(header_bytes), payload, pn, pn_offset, pn_length + 1);
            if (result.is_err()) {
                return dp::result::err(result.error());
            }

            auto final_packet = result.value();

            // Track sent packet for loss detection
            loss_detection_.on_packet_sent(PacketNumberSpace::Handshake, pn, final_packet.size(),
                                           true /* ack_eliciting */, true /* crypto_data */, false /* stream_data */);

            // Update congestion control
            congestion_control_.on_packet_sent(final_packet.size());

            return dp::result::ok(std::move(final_packet));
        }

        // Build a 0-RTT packet (early data)
        // 0-RTT packets use long header with ApplicationData packet number space
        dp::Res<dp::Vector<dp::u8>> build_0rtt_packet(const dp::Vector<dp::u8> &stream_frames) {
            if (!has_0rtt_keys_) {
                return dp::result::err(dp::Error::invalid_argument("no 0-RTT keys"));
            }

            // 0-RTT uses ApplicationData packet number space
            auto &space = spaces_[static_cast<int>(PacketNumberSpace::ApplicationData)];

            // Build 0-RTT header (long header)
            LongHeader header;
            header.packet_type = LongPacketType::ZeroRTT;
            header.version = version_;
            header.dest_cid = remote_cid_;
            header.src_cid = local_cid_;

            dp::u64 pn = space.next_packet_number++;
            dp::u8 pn_length = packet_number_length(pn, space.largest_acked_packet);
            header.pn_length = pn_length;

            // Serialize header
            auto header_bytes = header.serialize();

            // Add length field (varint)
            dp::usize payload_len = (pn_length + 1) + stream_frames.size() + AEAD_TAG_LENGTH;
            auto length_bytes = varint_encode(payload_len);
            header_bytes.insert(header_bytes.end(), length_bytes.begin(), length_bytes.end());

            // Add packet number
            auto pn_bytes = encode_packet_number(pn, pn_length);
            dp::usize pn_offset = header_bytes.size();
            header_bytes.insert(header_bytes.end(), pn_bytes.begin(), pn_bytes.end());

            // Encrypt using 0-RTT keys
            auto result =
                zero_rtt_crypto_.encrypt_packet(std::move(header_bytes), stream_frames, pn, pn_offset, pn_length + 1);
            if (result.is_err()) {
                return dp::result::err(result.error());
            }

            auto final_packet = result.value();

            // Track sent packet for loss detection
            loss_detection_.on_packet_sent(PacketNumberSpace::ApplicationData, pn, final_packet.size(),
                                           true /* ack_eliciting */, false /* crypto_data */, true /* stream_data */);

            // Update congestion control
            congestion_control_.on_packet_sent(final_packet.size());

            echo::debug("Built 0-RTT packet (", final_packet.size(), " bytes)");
            return dp::result::ok(std::move(final_packet));
        }

        // Set 0-RTT keys (for early data)
        void set_0rtt_keys(const QuicKeys &keys) {
            zero_rtt_crypto_.set_keys(keys);
            has_0rtt_keys_ = true;
            echo::debug("0-RTT keys installed");
        }

        // Check if 0-RTT is available
        bool has_0rtt_keys() const { return has_0rtt_keys_; }

        // === Path Validation (Connection Migration) ===

        // Initiate path validation by sending PATH_CHALLENGE
        // Returns the challenge data to be sent
        std::array<dp::u8, 8> initiate_path_challenge() {
            std::array<dp::u8, 8> challenge;
            // Generate random challenge data
            auto random = keylock::utils::Common::generate_random_bytes(8);
            std::copy(random.begin(), random.end(), challenge.begin());
            pending_path_challenges_.push_back(challenge);
            path_validated_ = false;
            echo::debug("Initiated path challenge");
            return challenge;
        }

        // Check if path is validated
        bool is_path_validated() const { return path_validated_; }

        // Get pending PATH_RESPONSE frames to send
        dp::Vector<std::array<dp::u8, 8>> take_pending_path_responses() {
            dp::Vector<std::array<dp::u8, 8>> responses;
            while (!pending_path_responses_.empty()) {
                responses.push_back(pending_path_responses_.front());
                pending_path_responses_.pop_front();
            }
            return responses;
        }

        // Check if there are pending path responses
        bool has_pending_path_responses() const { return !pending_path_responses_.empty(); }

        // Build a packet containing PATH_CHALLENGE
        dp::Res<dp::Vector<dp::u8>> build_path_challenge_packet() {
            auto challenge = initiate_path_challenge();
            PathChallengeFrame frame;
            frame.data = dp::Vector<dp::u8>(challenge.begin(), challenge.end());
            return build_short_packet(frame.serialize());
        }

        // Build a packet containing PATH_RESPONSE
        dp::Res<dp::Vector<dp::u8>> build_path_response_packet(const std::array<dp::u8, 8> &data) {
            PathResponseFrame frame;
            frame.data = dp::Vector<dp::u8>(data.begin(), data.end());
            return build_short_packet(frame.serialize());
        }

        // Build a 1-RTT (short header) packet with stream data
        dp::Res<dp::Vector<dp::u8>> build_short_packet(const dp::Vector<dp::u8> &payload_frames) {
            auto &space = spaces_[static_cast<int>(PacketNumberSpace::ApplicationData)];
            if (!space.has_keys()) {
                return dp::result::err(dp::Error::invalid_argument("no application keys"));
            }

            ShortHeader header;
            header.dest_cid = remote_cid_;

            dp::u64 pn = space.next_packet_number++;
            dp::u8 pn_length = packet_number_length(pn, space.largest_acked_packet);
            header.pn_length = pn_length;
            header.key_phase = key_phase_;

            // Serialize header
            auto header_bytes = header.serialize();

            // Add packet number
            auto pn_bytes = encode_packet_number(pn, pn_length);
            dp::usize pn_offset = header_bytes.size();
            header_bytes.insert(header_bytes.end(), pn_bytes.begin(), pn_bytes.end());

            // Encrypt
            auto result =
                space.send_crypto.encrypt_packet(std::move(header_bytes), payload_frames, pn, pn_offset, pn_length + 1);
            if (result.is_err()) {
                return dp::result::err(result.error());
            }

            auto final_packet = result.value();

            // Track sent packet for loss detection (assume stream data for short packets)
            bool has_stream = !payload_frames.empty();
            loss_detection_.on_packet_sent(PacketNumberSpace::ApplicationData, pn, final_packet.size(),
                                           has_stream /* ack_eliciting */, false /* crypto_data */,
                                           has_stream /* stream_data */);

            // Update congestion control
            congestion_control_.on_packet_sent(final_packet.size());

            return dp::result::ok(std::move(final_packet));
        }

        // Process a received packet
        dp::Res<void> process_packet(const dp::Vector<dp::u8> &packet) {
            if (packet.empty()) {
                return dp::result::err(dp::Error::invalid_argument("empty packet"));
            }

            // Make a mutable copy for decryption/processing
            dp::Vector<dp::u8> packet_copy = packet;

            if (is_long_header(packet_copy[0])) {
                return process_long_header_packet(packet_copy);
            } else {
                return process_short_header_packet(packet_copy);
            }
        }

        // Close connection
        void close(dp::u64 error_code = 0, const dp::String &reason = "") {
            if (state_ == ConnectionState::Closed || state_ == ConnectionState::Draining) {
                return;
            }

            close_error_code_ = error_code;
            close_reason_ = reason;
            state_ = ConnectionState::Draining;

            echo::info("QUIC connection closing: ", reason.c_str());
        }

        // Flow control
        dp::u64 max_data_local() const { return config_.local_params.initial_max_data; }
        dp::u64 max_data_remote() const { return max_data_remote_; }
        dp::u64 data_sent() const { return data_sent_; }
        dp::u64 data_received() const { return data_received_; }

        // Get pending frames to send (for building packets)
        dp::Vector<dp::u8> get_pending_stream_frames(dp::usize max_size) {
            dp::Vector<dp::u8> frames;

            for (auto &[stream_id, stream] : stream_manager_.streams()) {
                if (!stream.send_buffer().has_unsent_data()) {
                    continue;
                }

                // Check flow control
                dp::usize available = max_size - frames.size();
                if (available < 20) { // Minimum frame overhead
                    break;
                }

                // Build STREAM frame
                StreamFrame frame;
                frame.stream_id = stream_id;
                frame.offset = stream.send_buffer().sent_offset();
                frame.data = stream.send_buffer().get_data(available - 20);
                frame.fin = stream.send_buffer().has_fin() && !stream.send_buffer().fin_sent();

                if (frame.data.empty() && !frame.fin) {
                    continue;
                }

                auto frame_bytes = frame.serialize();
                frames.insert(frames.end(), frame_bytes.begin(), frame_bytes.end());

                stream.send_buffer().mark_sent(frame.data.size());
                if (frame.fin) {
                    stream.send_buffer().mark_fin_sent();
                }
            }

            return frames;
        }

      private:
        dp::Res<void> process_long_header_packet(dp::Vector<dp::u8> &packet) {
            auto header_result = LongHeader::parse(packet.data(), packet.size());
            if (header_result.is_err()) {
                return dp::result::err(header_result.error());
            }

            auto [header, header_len] = header_result.value();

            // Check version
            if (header.version != version_ && header.version != 0) {
                echo::warn("Version mismatch: ", header.version);
                // Could trigger version negotiation
            }

            PacketNumberSpace space = PacketNumberSpace::Initial;
            switch (header.packet_type) {
            case LongPacketType::Initial:
                space = PacketNumberSpace::Initial;
                break;
            case LongPacketType::Handshake:
                space = PacketNumberSpace::Handshake;
                break;
            case LongPacketType::ZeroRTT:
                space = PacketNumberSpace::ApplicationData;
                break;
            case LongPacketType::Retry:
                return process_retry_packet(packet);
            default:
                return dp::result::err(dp::Error::invalid_argument("unknown packet type"));
            }

            auto &pn_space = spaces_[static_cast<int>(space)];
            if (!pn_space.has_keys()) {
                return dp::result::err(dp::Error::invalid_argument("no keys for packet space"));
            }

            // For Initial packets, skip token
            dp::usize pn_offset = header_len;
            if (header.packet_type == LongPacketType::Initial) {
                auto token_len_result = varint_decode(packet.data() + pn_offset, packet.size() - pn_offset);
                if (token_len_result.is_err()) {
                    return dp::result::err(token_len_result.error());
                }
                auto [token_len, token_len_bytes] = token_len_result.value();
                pn_offset += token_len_bytes + token_len;
            }

            // Parse length
            auto length_result = varint_decode(packet.data() + pn_offset, packet.size() - pn_offset);
            if (length_result.is_err()) {
                return dp::result::err(length_result.error());
            }
            pn_offset += length_result.value().second;

            // Decrypt packet
            auto decrypt_result =
                pn_space.recv_crypto.decrypt_packet(packet, pn_offset, pn_space.largest_received_packet);
            if (decrypt_result.is_err()) {
                return dp::result::err(decrypt_result.error());
            }

            auto [packet_number, payload] = decrypt_result.value();
            pn_space.largest_received_packet = std::max(pn_space.largest_received_packet, packet_number);

            // Track received packet for ACK generation
            // Determine if ack-eliciting (contains non-ACK, non-PADDING frames)
            bool ack_eliciting =
                !payload.empty() && !(payload.size() == 1 && (payload[0] == 0x00 || payload[0] == 0x02));
            ack_manager_.on_packet_received(space, packet_number, ack_eliciting);

            // Process frames
            return process_frames(payload, space);
        }

        dp::Res<void> process_short_header_packet(dp::Vector<dp::u8> &packet) {
            auto &space = spaces_[static_cast<int>(PacketNumberSpace::ApplicationData)];
            if (!space.has_keys()) {
                return dp::result::err(dp::Error::invalid_argument("no application keys"));
            }

            dp::usize pn_offset = 1 + remote_cid_.size();

            auto decrypt_result = space.recv_crypto.decrypt_packet(packet, pn_offset, space.largest_received_packet);
            if (decrypt_result.is_err()) {
                return dp::result::err(decrypt_result.error());
            }

            auto [packet_number, payload] = decrypt_result.value();
            space.largest_received_packet = std::max(space.largest_received_packet, packet_number);

            // Track received packet for ACK generation
            bool ack_eliciting =
                !payload.empty() && !(payload.size() == 1 && (payload[0] == 0x00 || payload[0] == 0x02));
            ack_manager_.on_packet_received(PacketNumberSpace::ApplicationData, packet_number, ack_eliciting);

            return process_frames(payload, PacketNumberSpace::ApplicationData);
        }

        dp::Res<void> process_retry_packet(dp::Vector<dp::u8> &packet) {
            // Parse retry packet
            auto retry_result = RetryPacket::parse(packet.data(), packet.size());
            if (retry_result.is_err()) {
                return dp::result::err(retry_result.error());
            }

            auto &retry = retry_result.value();

            // Verify integrity tag (simplified)
            // In a full implementation, we'd verify the tag

            // Update connection ID and retry token
            remote_cid_ = retry.header.src_cid;
            retry_token_ = retry.retry_token;

            // Reset initial keys with new DCID
            set_initial_keys(remote_cid_);

            echo::debug("Processed Retry packet, new SCID set");
            return dp::result::ok();
        }

        dp::Res<void> process_frames(const dp::Vector<dp::u8> &payload, PacketNumberSpace space) {
            dp::usize offset = 0;

            while (offset < payload.size()) {
                dp::u8 frame_type = payload[offset];

                if (frame_type == 0x00) {
                    // PADDING - skip
                    offset++;
                    continue;
                }

                if (frame_type == 0x01) {
                    // PING - nothing to do
                    offset++;
                    continue;
                }

                if (frame_type == 0x02 || frame_type == 0x03) {
                    // ACK
                    auto ack_result = AckFrame::parse(payload.data() + offset, payload.size() - offset);
                    if (ack_result.is_err()) {
                        return dp::result::err(ack_result.error());
                    }
                    auto [ack, consumed] = ack_result.value();
                    process_ack(ack, space);
                    offset += consumed;
                    continue;
                }

                if (frame_type == 0x06) {
                    // CRYPTO
                    auto crypto_result = CryptoFrame::parse(payload.data() + offset, payload.size() - offset);
                    if (crypto_result.is_err()) {
                        return dp::result::err(crypto_result.error());
                    }
                    auto [crypto, consumed] = crypto_result.value();
                    // Store crypto data for TLS processing
                    pending_crypto_data_.insert(pending_crypto_data_.end(), crypto.data.begin(), crypto.data.end());
                    offset += consumed;
                    continue;
                }

                if (is_stream_frame(frame_type)) {
                    // STREAM
                    auto stream_result = StreamFrame::parse(payload.data() + offset, payload.size() - offset);
                    if (stream_result.is_err()) {
                        return dp::result::err(stream_result.error());
                    }
                    auto [stream_frame, consumed] = stream_result.value();
                    process_stream_frame(stream_frame);
                    offset += consumed;
                    continue;
                }

                if (frame_type == 0x1c || frame_type == 0x1d) {
                    // CONNECTION_CLOSE
                    auto close_result = ConnectionCloseFrame::parse(payload.data() + offset, payload.size() - offset);
                    if (close_result.is_err()) {
                        return dp::result::err(close_result.error());
                    }
                    auto [close_frame, consumed] = close_result.value();
                    close(close_frame.error_code, close_frame.reason_phrase);
                    offset += consumed;
                    continue;
                }

                if (frame_type == 0x1e) {
                    // HANDSHAKE_DONE
                    handshake_complete();
                    offset++;
                    continue;
                }

                if (frame_type == 0x1a) {
                    // PATH_CHALLENGE
                    auto pc_result = PathChallengeFrame::parse(payload.data() + offset, payload.size() - offset);
                    if (pc_result.is_err()) {
                        return dp::result::err(pc_result.error());
                    }
                    auto [pc_frame, consumed] = pc_result.value();
                    // Queue PATH_RESPONSE with same data (convert to array)
                    if (pc_frame.data.size() == 8) {
                        std::array<dp::u8, 8> resp_data;
                        std::copy(pc_frame.data.begin(), pc_frame.data.end(), resp_data.begin());
                        pending_path_responses_.push_back(resp_data);
                        echo::debug("Received PATH_CHALLENGE, queueing response");
                    }
                    offset += consumed;
                    continue;
                }

                if (frame_type == 0x1b) {
                    // PATH_RESPONSE
                    auto pr_result = PathResponseFrame::parse(payload.data() + offset, payload.size() - offset);
                    if (pr_result.is_err()) {
                        return dp::result::err(pr_result.error());
                    }
                    auto [pr_frame, consumed] = pr_result.value();
                    // Check if this matches our pending challenge
                    bool matched = false;
                    if (!pending_path_challenges_.empty() && pr_frame.data.size() == 8) {
                        auto &expected = pending_path_challenges_.front();
                        matched = std::equal(expected.begin(), expected.end(), pr_frame.data.begin());
                    }
                    if (matched) {
                        pending_path_challenges_.pop_front();
                        path_validated_ = true;
                        echo::debug("PATH_RESPONSE validated, path confirmed");
                    } else {
                        echo::warn("Unexpected PATH_RESPONSE data");
                    }
                    offset += consumed;
                    continue;
                }

                if (frame_type == 0x18) {
                    // NEW_CONNECTION_ID
                    auto ncid_result = NewConnectionIdFrame::parse(payload.data() + offset, payload.size() - offset);
                    if (ncid_result.is_err()) {
                        return dp::result::err(ncid_result.error());
                    }
                    auto [ncid_frame, consumed] = ncid_result.value();
                    process_new_connection_id(ncid_frame);
                    offset += consumed;
                    continue;
                }

                if (frame_type == 0x19) {
                    // RETIRE_CONNECTION_ID
                    auto rcid_result = RetireConnectionIdFrame::parse(payload.data() + offset, payload.size() - offset);
                    if (rcid_result.is_err()) {
                        return dp::result::err(rcid_result.error());
                    }
                    auto [rcid_frame, consumed] = rcid_result.value();
                    process_retire_connection_id(rcid_frame);
                    offset += consumed;
                    continue;
                }

                // Unknown frame type - skip (would need length parsing)
                echo::warn("Unknown frame type: ", static_cast<int>(frame_type));
                break;
            }

            return dp::result::ok();
        }

        void process_ack(const AckFrame &ack, PacketNumberSpace space) {
            auto &pn_space = spaces_[static_cast<int>(space)];
            pn_space.largest_acked_packet = std::max(pn_space.largest_acked_packet, ack.largest_ack);

            // Decode ACK ranges to (start, end) pairs
            auto ack_ranges = AckState::decode_ack_ranges(ack);

            // Process with loss detection - get newly acknowledged packets
            auto newly_acked = loss_detection_.on_ack_received(space, ack.largest_ack, ack.ack_delay, ack_ranges);

            // Update congestion control with acked packets
            congestion_control_.on_packets_acked(newly_acked);

            // Detect lost packets
            auto loss_result = loss_detection_.detect_lost_packets(space);

            // Handle lost packets
            if (!loss_result.lost_packets.empty()) {
                congestion_control_.on_packets_lost(loss_result.lost_packets);

                // Check for persistent congestion
                if (congestion_control_.check_persistent_congestion(loss_result.lost_packets, loss_detection_.rtt())) {
                    congestion_control_.on_persistent_congestion();
                }

                // Mark frames for retransmission (simplified - would need to track frames per packet)
                for (const auto &lost_pkt : loss_result.lost_packets) {
                    echo::debug("Lost packet ", lost_pkt.packet_number, " in space ", static_cast<int>(space));
                }
            }

            // Sync bytes in flight
            congestion_control_.set_bytes_in_flight(loss_detection_.bytes_in_flight());

            echo::trace("ACK processed: largest=", ack.largest_ack, " newly_acked=", newly_acked.size(),
                        " lost=", loss_result.lost_packets.size());
        }

        void process_stream_frame(const StreamFrame &frame) {
            // Check connection-level flow control
            auto fc_error = flow_control_.validate_recv(frame.data.size());
            if (!fc_error.empty()) {
                echo::error("Flow control violation: ", fc_error.c_str());
                close(static_cast<dp::u64>(TransportError::FlowControlError), fc_error);
                return;
            }

            auto *stream = stream_manager_.get_or_create(frame.stream_id);
            if (!stream) {
                echo::warn("Failed to get/create stream ", frame.stream_id);
                return;
            }

            stream->receive_data(frame.offset, frame.data, frame.fin);

            // Update flow control
            flow_control_.connection().on_data_received(frame.data.size());
            data_received_ += frame.data.size();

            echo::trace("Received ", frame.data.size(), " bytes on stream ", frame.stream_id);
        }

        bool is_client_;
        ConnectionConfig config_;
        ConnectionState state_ = ConnectionState::Idle;
        dp::u32 version_ = QUIC_VERSION_1;

        // Connection IDs
        ConnectionId local_cid_;
        ConnectionId remote_cid_;
        ConnectionId original_dcid_;
        dp::Vector<dp::u8> retry_token_;

        // Transport parameters
        TransportParameters remote_params_;

        // Packet number spaces
        PacketNumberSpaceState spaces_[3];

        // Stream management
        StreamManager stream_manager_;

        // Loss detection and recovery (RFC 9002)
        LossDetection loss_detection_;

        // Congestion control (NewReno)
        CongestionController congestion_control_;

        // ACK management
        AckManager ack_manager_;

        // Flow control (replaces simple counters)
        FlowControlManager flow_control_;

        // Legacy flow control (kept for compatibility, will migrate to flow_control_)
        dp::u64 max_data_remote_ = 0;
        dp::u64 data_sent_ = 0;
        dp::u64 data_received_ = 0;

        // Key phase (for key updates)
        bool key_phase_ = false;

        // 0-RTT support
        CryptoState zero_rtt_crypto_;
        bool has_0rtt_keys_ = false;

        // Path validation (for connection migration)
        std::deque<std::array<dp::u8, 8>> pending_path_challenges_; // Challenges we've sent
        std::deque<std::array<dp::u8, 8>> pending_path_responses_;  // Responses to send
        bool path_validated_ = true;                                // Initially validated (original path)

        // Connection ID management
        struct IssuedConnectionId {
            ConnectionId cid;
            dp::u64 sequence = 0;
            dp::Vector<dp::u8> stateless_reset_token;
        };
        struct PeerConnectionId {
            ConnectionId cid;
            dp::u64 sequence = 0;
            dp::Vector<dp::u8> stateless_reset_token;
        };
        dp::Vector<IssuedConnectionId> issued_cids_; // CIDs we've issued to peer
        dp::Vector<PeerConnectionId> peer_cids_;     // CIDs peer has given us
        dp::Vector<dp::u64> pending_retire_cids_;    // CIDs we need to retire
        dp::u64 next_cid_sequence_ = 0;              // Next sequence for issued CIDs
        dp::u64 active_peer_cid_sequence_ = 0;       // Currently active peer CID

        // Generate stateless reset token
        dp::Vector<dp::u8> generate_stateless_reset_token() {
            auto bytes = keylock::utils::Common::generate_random_bytes(16);
            return dp::Vector<dp::u8>(bytes.begin(), bytes.end());
        }

        // Pending crypto data from CRYPTO frames
        dp::Vector<dp::u8> pending_crypto_data_;

        // Close state
        dp::u64 close_error_code_ = 0;
        dp::String close_reason_;

      public:
        // Get pending crypto data (for TLS processing)
        dp::Vector<dp::u8> take_crypto_data() {
            auto data = std::move(pending_crypto_data_);
            pending_crypto_data_.clear();
            return data;
        }

        bool has_pending_crypto() const { return !pending_crypto_data_.empty(); }

        // Access to reliability/congestion/flow control modules
        LossDetection &loss_detection() { return loss_detection_; }
        const LossDetection &loss_detection() const { return loss_detection_; }

        CongestionController &congestion_control() { return congestion_control_; }
        const CongestionController &congestion_control() const { return congestion_control_; }

        AckManager &ack_manager() { return ack_manager_; }
        const AckManager &ack_manager() const { return ack_manager_; }

        FlowControlManager &flow_control() { return flow_control_; }
        const FlowControlManager &flow_control() const { return flow_control_; }

        // Check if we should send ACKs
        bool should_send_ack() const { return ack_manager_.any_space_needs_ack(); }

        // Get ACK frame to send for a space
        dp::Res<AckFrame> generate_ack_frame(PacketNumberSpace space) { return ack_manager_.generate_ack_frame(space); }

        // Check if congestion window allows sending
        bool can_send(dp::usize bytes = 1200) const { return congestion_control_.can_send(bytes); }

        // Get RTT info
        const RttEstimator &rtt() const { return loss_detection_.rtt(); }

        // Get datagrams to send (for transport layer)
        dp::Vector<dp::Vector<dp::u8>> get_datagrams_to_send() {
            dp::Vector<dp::Vector<dp::u8>> datagrams;

            // Check if we have stream data to send
            if (state_ == ConnectionState::Connected) {
                auto stream_frames = get_pending_stream_frames(1200 - 50); // Leave room for header
                if (!stream_frames.empty()) {
                    auto pkt_result = build_short_packet(stream_frames);
                    if (pkt_result.is_ok()) {
                        datagrams.push_back(std::move(pkt_result.value()));
                    }
                }

                // Send ACKs if needed
                if (should_send_ack()) {
                    auto ack_result = generate_ack_frame(PacketNumberSpace::ApplicationData);
                    if (ack_result.is_ok()) {
                        auto ack_bytes = ack_result.value().serialize();
                        auto pkt_result = build_short_packet(ack_bytes);
                        if (pkt_result.is_ok()) {
                            datagrams.push_back(std::move(pkt_result.value()));
                        }
                    }
                }
            } else if (state_ == ConnectionState::Handshaking) {
                // During handshake, we might need to send crypto data
                // This is handled separately by the TLS adapter
            }

            return datagrams;
        }
    };

} // namespace netpipe::quic

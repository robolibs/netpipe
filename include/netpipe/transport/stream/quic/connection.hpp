#pragma once

#include <array>
#include <chrono>
#include <datapod/datapod.hpp>
#include <deque>
#include <echo/echo.hpp>
#include <keylock/crypto/common.hpp>
#include <keylock/hash/hmac/hmac_sha256.hpp>
#include <netpipe/transport/stream/quic/ack_manager.hpp>
#include <netpipe/transport/stream/quic/congestion_control.hpp>
#include <netpipe/transport/stream/quic/crypto.hpp>
#include <netpipe/transport/stream/quic/flow_control.hpp>
#include <netpipe/transport/stream/quic/frame.hpp>
#include <netpipe/transport/stream/quic/loss_detection.hpp>
#include <netpipe/transport/stream/quic/packet.hpp>
#include <netpipe/transport/stream/quic/stream.hpp>
#include <netpipe/transport/stream/quic/transport_params.hpp>
#include <netpipe/transport/stream/quic/types.hpp>

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

        // === Version Negotiation (RFC 9000 Section 6) ===

        // Get list of supported versions
        static dp::Vector<dp::u32> supported_versions() { return {QUIC_VERSION_1, QUIC_VERSION_2}; }

        // Check if a version is supported
        static bool is_version_supported(dp::u32 version) {
            return version == QUIC_VERSION_1 || version == QUIC_VERSION_2;
        }

        // Get current negotiated version
        dp::u32 version() const { return version_; }

        // Set version (call before handshake starts)
        void set_version(dp::u32 version) {
            version_ = version;
            echo::debug("QUIC version set to 0x", std::hex, version, std::dec);
        }

        // Build a version negotiation packet (server sends when client version unsupported)
        static dp::Vector<dp::u8> build_version_negotiation(const ConnectionId &dest_cid, const ConnectionId &src_cid) {
            VersionNegotiationPacket packet;
            packet.dest_cid = dest_cid;
            packet.src_cid = src_cid;
            packet.supported_versions = supported_versions();
            return packet.serialize();
        }

        // Process a received version negotiation packet
        // Returns the selected version, or error if no compatible version
        dp::Res<dp::u32> process_version_negotiation(const VersionNegotiationPacket &vn_packet) {
            if (!is_client_) {
                return dp::result::err(dp::Error::invalid_argument("server should not receive version negotiation"));
            }

            // Check that the DCID matches our SCID (to prevent reflection attacks)
            if (vn_packet.dest_cid != local_cid_) {
                return dp::result::err(dp::Error::invalid_argument("version negotiation DCID mismatch"));
            }

            // Find a compatible version
            for (auto offered_version : vn_packet.supported_versions) {
                if (is_version_supported(offered_version)) {
                    version_ = offered_version;
                    version_negotiation_received_ = true;
                    echo::info("Version negotiation selected version 0x", std::hex, offered_version, std::dec);
                    return dp::result::ok(offered_version);
                }
            }

            return dp::result::err(dp::Error::invalid_argument("no compatible version found"));
        }

        // Check if version negotiation was received
        bool version_negotiation_received() const { return version_negotiation_received_; }

        // === Path Validation (Connection Migration) ===

        // Initiate path validation by sending PATH_CHALLENGE
        // Returns the challenge data to be sent
        std::array<dp::u8, 8> initiate_path_challenge() {
            std::array<dp::u8, 8> challenge;
            // Generate random challenge data
            auto random = keylock::crypto::Common::generate_random_bytes(8);
            std::copy(random.begin(), random.end(), challenge.begin());
            pending_path_challenges_.push_back(challenge);
            path_validated_ = false;
            path_challenge_retries_ = 0;
            path_challenge_sent_time_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now().time_since_epoch())
                                            .count();
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

        // Record bytes received (for anti-amplification)
        void record_bytes_received(dp::u64 bytes) {
            if (!path_validated_) {
                bytes_received_before_validation_ += bytes;
            }
        }

        // Check if we can send more data (anti-amplification limit)
        // Before path is validated, server can only send 3x what it has received
        bool can_send_before_validation(dp::u64 bytes) const {
            if (path_validated_) {
                return true;
            }
            // Anti-amplification: can only send 3x received
            dp::u64 limit = bytes_received_before_validation_ * ANTI_AMPLIFICATION_FACTOR;
            return bytes_sent_before_validation_ + bytes <= limit;
        }

        // Get remaining bytes we can send before validation
        dp::u64 bytes_until_amplification_limit() const {
            if (path_validated_) {
                return dp::u64(-1); // No limit
            }
            dp::u64 limit = bytes_received_before_validation_ * ANTI_AMPLIFICATION_FACTOR;
            if (bytes_sent_before_validation_ >= limit) {
                return 0;
            }
            return limit - bytes_sent_before_validation_;
        }

        // Record bytes sent (for anti-amplification tracking)
        void record_bytes_sent(dp::u64 bytes) {
            if (!path_validated_) {
                bytes_sent_before_validation_ += bytes;
            }
        }

        // Check if path challenge has timed out and should be retried
        bool should_retry_path_challenge(dp::u64 current_time_ms) const {
            if (path_validated_ || pending_path_challenges_.empty()) {
                return false;
            }
            if (path_challenge_retries_ >= MAX_PATH_CHALLENGE_RETRIES) {
                return false; // Exceeded retry limit
            }
            return (current_time_ms - path_challenge_sent_time_) >= PATH_CHALLENGE_TIMEOUT_MS;
        }

        // Retry path challenge (call after should_retry_path_challenge returns true)
        std::array<dp::u8, 8> retry_path_challenge() {
            path_challenge_retries_++;
            // Use the same challenge data
            auto challenge = pending_path_challenges_.front();
            // Update sent time
            path_challenge_sent_time_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now().time_since_epoch())
                                            .count();
            echo::debug("Retrying path challenge (attempt ", path_challenge_retries_, ")");
            return challenge;
        }

        // Check if path validation has failed (exceeded retries)
        bool has_path_validation_failed() const {
            return !path_validated_ && !pending_path_challenges_.empty() &&
                   path_challenge_retries_ >= MAX_PATH_CHALLENGE_RETRIES;
        }

        // Get number of path challenge retries
        dp::u32 path_challenge_retry_count() const { return path_challenge_retries_; }

        // Reset path validation state (for starting fresh on a new path)
        void reset_path_validation() {
            pending_path_challenges_.clear();
            pending_path_responses_.clear();
            path_validated_ = false;
            path_challenge_sent_time_ = 0;
            path_challenge_retries_ = 0;
            bytes_received_before_validation_ = 0;
            bytes_sent_before_validation_ = 0;
        }

        // =================================================================
        // Connection Migration (RFC 9000 Section 9)
        // =================================================================

        // Migration state
        enum class MigrationState {
            Idle,         // Not migrating
            Probing,      // Sending PATH_CHALLENGE on new path
            WaitingProbe, // Waiting for PATH_RESPONSE on new path
            Migrating,    // Migration in progress
            Complete      // Migration complete
        };

        // Get current migration state
        MigrationState migration_state() const { return migration_state_; }

        // Check if connection migration is supported
        bool supports_migration() const {
            // Migration is disabled if disable_active_migration transport param is set
            return !remote_params_.disable_active_migration;
        }

        // Get current local address
        const dp::String &local_address() const { return local_address_; }

        // Get current remote address
        const dp::String &remote_address() const { return remote_address_; }

        // Set addresses (used during initial connection)
        void set_addresses(const dp::String &local, const dp::String &remote) {
            local_address_ = local;
            remote_address_ = remote;
        }

        // Initiate active connection migration to a new local address
        // This is "active migration" initiated by this endpoint
        dp::Res<void> initiate_migration(const dp::String &new_local_addr) {
            if (!supports_migration()) {
                return dp::result::err(dp::Error::invalid_argument("peer disabled active migration"));
            }
            if (state_ != ConnectionState::Connected) {
                return dp::result::err(dp::Error::invalid_argument("connection not established"));
            }
            if (migration_state_ != MigrationState::Idle) {
                return dp::result::err(dp::Error::invalid_argument("migration already in progress"));
            }

            pending_local_address_ = new_local_addr;
            migration_state_ = MigrationState::Probing;

            // Reset path validation for the new path
            reset_path_validation();

            // Initiate path challenge on the new path
            initiate_path_challenge();

            echo::info("Initiating connection migration to ", new_local_addr.c_str());
            return dp::result::ok();
        }

        // Handle detection of peer address change (passive migration)
        // Called when we receive a packet from a different address
        dp::Res<void> handle_peer_address_change(const dp::String &new_remote_addr) {
            if (!supports_migration()) {
                // If peer disabled migration, this might indicate an attack
                echo::warn("Received packet from new address but migration is disabled");
                return dp::result::err(dp::Error::invalid_argument("migration disabled"));
            }

            if (new_remote_addr == remote_address_) {
                return dp::result::ok(); // No change
            }

            // Store the new address as pending
            pending_remote_address_ = new_remote_addr;

            // We need to validate this new path before using it
            migration_state_ = MigrationState::Probing;
            reset_path_validation();
            initiate_path_challenge();

            echo::info("Detected peer address change to ", new_remote_addr.c_str());
            return dp::result::ok();
        }

        // Complete the migration after path validation succeeds
        void complete_migration() {
            if (migration_state_ == MigrationState::Idle) {
                return;
            }

            if (!pending_local_address_.empty()) {
                local_address_ = pending_local_address_;
                pending_local_address_.clear();
            }
            if (!pending_remote_address_.empty()) {
                remote_address_ = pending_remote_address_;
                pending_remote_address_.clear();
            }

            migration_state_ = MigrationState::Complete;
            echo::info("Connection migration complete");

            // Reset back to idle after completion
            migration_state_ = MigrationState::Idle;
        }

        // Cancel an in-progress migration
        void cancel_migration() {
            if (migration_state_ == MigrationState::Idle) {
                return;
            }

            pending_local_address_.clear();
            pending_remote_address_.clear();
            reset_path_validation();
            path_validated_ = true; // Keep using current path
            migration_state_ = MigrationState::Idle;

            echo::info("Connection migration cancelled");
        }

        // Check if migration should be completed (called after path validation)
        bool should_complete_migration() const {
            return migration_state_ != MigrationState::Idle && migration_state_ != MigrationState::Complete &&
                   path_validated_;
        }

        // Handle path validation result
        void on_path_validation_result(bool success) {
            if (migration_state_ == MigrationState::Idle) {
                return;
            }

            if (success) {
                complete_migration();
            } else {
                cancel_migration();
                echo::warn("Path validation failed, migration cancelled");
            }
        }

        // Check if we're in the middle of a migration
        bool is_migrating() const { return migration_state_ != MigrationState::Idle; }

        // Get data for a PATH_CHALLENGE frame to send on new path
        dp::Res<dp::Vector<dp::u8>> get_migration_probe_data() {
            if (migration_state_ != MigrationState::Probing && migration_state_ != MigrationState::WaitingProbe) {
                return dp::result::err(dp::Error::invalid_argument("not probing new path"));
            }

            if (pending_path_challenges_.empty()) {
                return dp::result::err(dp::Error::invalid_argument("no pending path challenge"));
            }

            migration_state_ = MigrationState::WaitingProbe;
            return build_path_challenge_packet();
        }

        // =================================================================
        // Packet Coalescing (RFC 9000 Section 12.2)
        // =================================================================

        // Maximum UDP datagram size (default path MTU)
        static constexpr dp::usize MAX_DATAGRAM_SIZE = 1200;

        // Coalesce multiple packets into a single UDP datagram
        // Packets must be ordered by encryption level (Initial, Handshake, then 1-RTT)
        // Returns the coalesced datagram or error if packets don't fit
        dp::Res<dp::Vector<dp::u8>> coalesce_packets(const dp::Vector<dp::Vector<dp::u8>> &packets,
                                                     dp::usize max_size = MAX_DATAGRAM_SIZE) {
            dp::Vector<dp::u8> datagram;

            for (const auto &packet : packets) {
                if (datagram.size() + packet.size() > max_size) {
                    if (datagram.empty()) {
                        // First packet doesn't fit - error
                        return dp::result::err(dp::Error::invalid_argument("packet too large for MTU"));
                    }
                    // Stop coalescing, return what we have
                    break;
                }
                datagram.insert(datagram.end(), packet.begin(), packet.end());
            }

            if (datagram.empty()) {
                return dp::result::err(dp::Error::invalid_argument("no packets to coalesce"));
            }

            echo::trace("Coalesced ", packets.size(), " packets into ", datagram.size(), " byte datagram");
            return dp::result::ok(std::move(datagram));
        }

        // Build a coalesced datagram with Initial and optionally Handshake packets
        // This is commonly used during the handshake phase
        dp::Res<dp::Vector<dp::u8>>
        build_coalesced_handshake_datagram(const dp::Vector<dp::u8> &initial_crypto_data,
                                           const dp::Vector<dp::u8> &handshake_crypto_data = {},
                                           const dp::Vector<dp::u8> &token = {}) {

            dp::Vector<dp::Vector<dp::u8>> packets;

            // Build Initial packet (required during handshake)
            if (!initial_crypto_data.empty()) {
                auto &initial_space = spaces_[static_cast<int>(PacketNumberSpace::Initial)];
                if (initial_space.has_keys()) {
                    auto initial_result = build_initial_packet(initial_crypto_data, token);
                    if (initial_result.is_ok()) {
                        packets.push_back(std::move(initial_result.value()));
                    }
                }
            }

            // Build Handshake packet if we have handshake keys
            if (!handshake_crypto_data.empty()) {
                auto &handshake_space = spaces_[static_cast<int>(PacketNumberSpace::Handshake)];
                if (handshake_space.has_keys()) {
                    auto handshake_result = build_handshake_packet(handshake_crypto_data);
                    if (handshake_result.is_ok()) {
                        packets.push_back(std::move(handshake_result.value()));
                    }
                }
            }

            if (packets.empty()) {
                return dp::result::err(dp::Error::invalid_argument("no packets to send"));
            }

            return coalesce_packets(packets);
        }

        // Build a coalesced datagram from pre-built packets
        // Use this when you have already built packets and want to coalesce them
        dp::Res<dp::Vector<dp::u8>> build_coalesced_from_packets(dp::Vector<dp::Vector<dp::u8>> &&packets) {
            if (packets.empty()) {
                return dp::result::err(dp::Error::invalid_argument("no packets to coalesce"));
            }
            return coalesce_packets(packets);
        }

        // Check if coalescing is beneficial (more than one packet type pending)
        bool can_coalesce() const {
            int levels_with_data = 0;

            auto &initial_space = spaces_[static_cast<int>(PacketNumberSpace::Initial)];
            if (initial_space.has_keys()) {
                levels_with_data++;
            }

            auto &handshake_space = spaces_[static_cast<int>(PacketNumberSpace::Handshake)];
            if (handshake_space.has_keys()) {
                levels_with_data++;
            }

            auto &app_space = spaces_[static_cast<int>(PacketNumberSpace::ApplicationData)];
            if (app_space.has_keys()) {
                levels_with_data++;
            }

            return levels_with_data > 1;
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

            // Apply pending key update if any
            key_phase_state_.apply_pending_key_update(pn);
            header.key_phase = key_phase_state_.get_write_phase();
            key_phase_ = header.key_phase; // Keep legacy tracking in sync

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

        // =================================================================
        // Key Phase / Key Updates (RFC 9001 Section 6)
        // =================================================================

        // Initialize key phase state after handshake completes
        void initialize_key_phase(const dp::Vector<dp::u8> &client_app_secret,
                                  const dp::Vector<dp::u8> &server_app_secret) {
            key_phase_state_.initialize(client_app_secret, server_app_secret, is_client_);
            echo::debug("Key phase initialized for ", is_client_ ? "client" : "server");
        }

        // Initiate a key update (local request)
        // Returns true if key update was successfully initiated
        bool initiate_key_update() {
            if (state_ != ConnectionState::Connected) {
                echo::debug("Cannot initiate key update: connection not in Connected state");
                return false;
            }

            auto &space = spaces_[static_cast<dp::usize>(PacketNumberSpace::ApplicationData)];
            return key_phase_state_.initiate_key_update(space.next_packet_number);
        }

        // Get current key phase
        bool current_key_phase() const { return key_phase_state_.get_write_phase(); }

        // Get number of key updates performed
        dp::u64 key_update_count() const { return key_phase_state_.key_update_count; }

        // Check if peer initiated a key update we need to respond to
        bool pending_key_update_response() const { return key_phase_state_.should_send_key_update(); }

        // Get key phase state for decryption (to handle out-of-order packets)
        const KeyPhaseState &key_phase_state() const { return key_phase_state_; }

        // =================================================================
        // Stateless Reset (RFC 9000 Section 10.3)
        // =================================================================

        // Check if a received packet is a stateless reset
        // Stateless resets have the reset token as the last 16 bytes
        bool is_stateless_reset(const dp::Vector<dp::u8> &packet) const {
            if (packet.size() < STATELESS_RESET_MIN_SIZE) {
                return false; // Too short to be a stateless reset
            }

            // Extract the last 16 bytes as the potential reset token
            dp::Vector<dp::u8> received_token(packet.end() - STATELESS_RESET_TOKEN_SIZE, packet.end());

            // Check against all peer connection IDs we have tokens for
            for (const auto &peer_cid : peer_cids_) {
                if (peer_cid.stateless_reset_token.size() == STATELESS_RESET_TOKEN_SIZE &&
                    peer_cid.stateless_reset_token == received_token) {
                    return true;
                }
            }

            // Check against the original remote connection ID's token (from transport params)
            if (peer_stateless_reset_token_.size() == STATELESS_RESET_TOKEN_SIZE &&
                peer_stateless_reset_token_ == received_token) {
                return true;
            }

            return false;
        }

        // Handle a received stateless reset
        void handle_stateless_reset() {
            echo::info("Received stateless reset, closing connection");
            state_ = ConnectionState::Closed;
            close_error_code_ = 0;
            close_reason_ = "stateless reset received";
        }

        // Build a stateless reset packet
        // This is used when we receive a packet for a connection we don't have state for
        dp::Vector<dp::u8> build_stateless_reset(const dp::Vector<dp::u8> &reset_token) {
            // Stateless reset format (RFC 9000 Section 10.3):
            // - Fixed bit = 1, Form bit = 0 (looks like short header)
            // - At least 21 bytes total (to be indistinguishable from short header packets)
            // - Unpredictable bytes
            // - Stateless Reset Token (16 bytes) at the end

            dp::Vector<dp::u8> packet;

            // Generate random unpredictable bytes (at least 5 to make total >= 21)
            auto random_bytes = keylock::crypto::Common::generate_random_bytes(21 - STATELESS_RESET_TOKEN_SIZE);

            // First byte: Fixed bit = 1, Form bit = 0
            // Other bits should look random
            random_bytes[0] = (random_bytes[0] & 0x3F) | 0x40; // Set fixed bit, clear form bit

            packet.insert(packet.end(), random_bytes.begin(), random_bytes.end());

            // Append the stateless reset token
            packet.insert(packet.end(), reset_token.begin(), reset_token.end());

            echo::debug("Built stateless reset packet, size: ", packet.size());
            return packet;
        }

        // Get our stateless reset token for the local connection ID
        dp::Vector<dp::u8> get_local_stateless_reset_token() const {
            if (!issued_cids_.empty()) {
                return issued_cids_[0].stateless_reset_token;
            }
            return {};
        }

        // Set the peer's stateless reset token from transport parameters
        void set_peer_stateless_reset_token(const dp::Vector<dp::u8> &token) { peer_stateless_reset_token_ = token; }

        // Constants for stateless reset
        static constexpr dp::usize STATELESS_RESET_TOKEN_SIZE = 16;
        static constexpr dp::usize STATELESS_RESET_MIN_SIZE = 21; // Minimum packet size

        // ==========================================================================
        // Address Validation Tokens (RFC 9000 Section 8.1)
        // ==========================================================================

        // Server: Initialize the token secret (should be done once at server start)
        void init_token_secret() {
            if (token_secret_.empty()) {
                auto secret = keylock::crypto::Common::generate_random_bytes(32);
                token_secret_ = dp::Vector<dp::u8>(secret.begin(), secret.end());
            }
        }

        // Server: Set a specific token secret (for key sharing across server instances)
        void set_token_secret(const dp::Vector<dp::u8> &secret) { token_secret_ = secret; }

        // Server: Generate a NEW_TOKEN for address validation
        // The token encodes: timestamp + client address info + HMAC
        dp::Res<NewTokenFrame> generate_new_token(const dp::String &client_addr) {
            if (is_client_) {
                return dp::result::err(dp::Error::invalid_argument("only servers generate tokens"));
            }
            if (token_secret_.empty()) {
                init_token_secret();
            }

            // Token format: [timestamp:8][addr_len:1][addr:var][hmac:32]
            dp::Vector<dp::u8> token_data;

            // Timestamp (8 bytes, milliseconds since epoch)
            auto now = std::chrono::steady_clock::now();
            dp::u64 timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
            for (int i = 7; i >= 0; i--) {
                token_data.push_back(static_cast<dp::u8>((timestamp >> (i * 8)) & 0xFF));
            }

            // Address length and data
            token_data.push_back(static_cast<dp::u8>(client_addr.size()));
            token_data.insert(token_data.end(), client_addr.begin(), client_addr.end());

            // Compute HMAC-SHA256 over the token data
            auto hmac = compute_token_hmac(token_data);
            token_data.insert(token_data.end(), hmac.begin(), hmac.end());

            NewTokenFrame frame;
            frame.token = std::move(token_data);
            return dp::result::ok(std::move(frame));
        }

        // Server: Validate a token received in an Initial packet
        // Returns the client address if valid, error otherwise
        dp::Res<dp::String> validate_token(const dp::Vector<dp::u8> &token) {
            if (is_client_) {
                return dp::result::err(dp::Error::invalid_argument("only servers validate tokens"));
            }
            if (token_secret_.empty()) {
                return dp::result::err(dp::Error::invalid_argument("token secret not set"));
            }

            // Minimum token size: 8 (timestamp) + 1 (addr_len) + 0 (addr) + 32 (hmac) = 41
            if (token.size() < 41) {
                return dp::result::err(dp::Error::invalid_argument("token too short"));
            }

            // Extract HMAC (last 32 bytes)
            dp::Vector<dp::u8> received_hmac(token.end() - 32, token.end());
            dp::Vector<dp::u8> token_data(token.begin(), token.end() - 32);

            // Verify HMAC
            auto expected_hmac = compute_token_hmac(token_data);
            if (received_hmac != expected_hmac) {
                return dp::result::err(dp::Error::invalid_argument("invalid token HMAC"));
            }

            // Extract timestamp
            dp::u64 timestamp = 0;
            for (int i = 0; i < 8; i++) {
                timestamp = (timestamp << 8) | token_data[i];
            }

            // Check token age
            auto now = std::chrono::steady_clock::now();
            dp::u64 current_time =
                std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
            if (current_time - timestamp > TOKEN_LIFETIME_MS) {
                return dp::result::err(dp::Error::invalid_argument("token expired"));
            }

            // Extract address
            dp::u8 addr_len = token_data[8];
            if (static_cast<dp::usize>(9 + addr_len) > token_data.size()) {
                return dp::result::err(dp::Error::invalid_argument("invalid token format"));
            }
            dp::String addr(reinterpret_cast<const char *>(token_data.data() + 9), addr_len);

            return dp::result::ok(std::move(addr));
        }

        // Client: Store a received token for a server
        void store_token(const dp::String &server_name, const dp::Vector<dp::u8> &token) {
            if (!is_client_)
                return;
            stored_tokens_[server_name] = token;
            echo::debug("Stored address validation token for ", server_name.c_str());
        }

        // Client: Get a stored token for a server (for use in Initial packet)
        dp::Optional<dp::Vector<dp::u8>> get_stored_token(const dp::String &server_name) const {
            if (!is_client_)
                return dp::nullopt;
            auto it = stored_tokens_.find(server_name);
            if (it == stored_tokens_.end()) {
                return dp::nullopt;
            }
            return it->second;
        }

        // Client: Check if we have a stored token for a server
        bool has_stored_token(const dp::String &server_name) const {
            if (!is_client_)
                return false;
            return stored_tokens_.find(server_name) != stored_tokens_.end();
        }

        // Client: Clear stored token for a server
        void clear_stored_token(const dp::String &server_name) {
            if (!is_client_)
                return;
            stored_tokens_.erase(server_name);
        }

        // Client: Handle received NEW_TOKEN frame
        dp::Res<void> handle_new_token(const NewTokenFrame &frame, const dp::String &server_name) {
            if (!is_client_) {
                return dp::result::err(dp::Error::invalid_argument("servers don't receive NEW_TOKEN"));
            }
            if (frame.token.empty()) {
                return dp::result::err(dp::Error::invalid_argument("empty token"));
            }
            store_token(server_name, frame.token);
            return dp::result::ok();
        }

      private:
        // Compute HMAC-SHA256 for token validation
        dp::Vector<dp::u8> compute_token_hmac(const dp::Vector<dp::u8> &data) const {
            dp::Vector<dp::u8> hmac(keylock::hash::hmac_sha256::BYTES);
            keylock::hash::hmac_sha256::hmac(hmac.data(), token_secret_.data(), token_secret_.size(), data.data(),
                                             data.size());
            return hmac;
        }

      public:
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
        // Helper to calculate total size of packets
        static dp::usize packets_total_size(const dp::Vector<dp::Vector<dp::u8>> &packets) {
            dp::usize total = 0;
            for (const auto &p : packets) {
                total += p.size();
            }
            return total;
        }

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

            // Verify integrity tag (RFC 9001 Section 5.8)
            // Build the Retry pseudo-packet: ODCID length || ODCID || Retry packet without tag
            dp::usize retry_without_tag_len = packet.size() - RetryPacket::RETRY_INTEGRITY_TAG_LENGTH;
            dp::Vector<dp::u8> retry_without_tag(packet.begin(), packet.begin() + retry_without_tag_len);

            auto pseudo_packet = build_retry_pseudo_packet(original_dcid_, retry_without_tag);

            if (!verify_retry_integrity_tag(pseudo_packet, retry.retry_integrity_tag, version_)) {
                echo::warn("Retry integrity tag verification failed");
                return dp::result::err(dp::Error::invalid_argument("Retry integrity tag verification failed"));
            }

            echo::debug("Retry integrity tag verified");

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
        bool version_negotiation_received_ = false;

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

        // Key phase tracking (for key updates per RFC 9001 Section 6)
        KeyPhaseState key_phase_state_;
        bool key_phase_ = false; // Legacy simple tracking (deprecated, use key_phase_state_)

        // 0-RTT support
        CryptoState zero_rtt_crypto_;
        bool has_0rtt_keys_ = false;

        // Path validation (for connection migration)
        std::deque<std::array<dp::u8, 8>> pending_path_challenges_; // Challenges we've sent
        std::deque<std::array<dp::u8, 8>> pending_path_responses_;  // Responses to send
        bool path_validated_ = true;                                // Initially validated (original path)
        dp::u64 path_challenge_sent_time_ = 0;                      // When we sent the last PATH_CHALLENGE
        dp::u32 path_challenge_retries_ = 0;                        // Number of PATH_CHALLENGE retries
        static constexpr dp::u32 MAX_PATH_CHALLENGE_RETRIES = 3;    // RFC recommends 3 retries
        static constexpr dp::u64 PATH_CHALLENGE_TIMEOUT_MS = 1000;  // 1 second timeout

        // Connection migration state
        MigrationState migration_state_ = MigrationState::Idle;
        dp::String local_address_;
        dp::String remote_address_;
        dp::String pending_local_address_;
        dp::String pending_remote_address_;

        // Anti-amplification (RFC 9000 Section 8.1)
        dp::u64 bytes_received_before_validation_ = 0;
        dp::u64 bytes_sent_before_validation_ = 0;
        static constexpr dp::u64 ANTI_AMPLIFICATION_FACTOR = 3;

        // Address validation tokens (RFC 9000 Section 8.1)
        // Client: tokens received via NEW_TOKEN frames, keyed by server name
        dp::Map<dp::String, dp::Vector<dp::u8>> stored_tokens_;
        // Server: secret for generating/validating tokens
        dp::Vector<dp::u8> token_secret_;
        static constexpr dp::u64 TOKEN_LIFETIME_MS = 24 * 60 * 60 * 1000; // 24 hours

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
        dp::Vector<IssuedConnectionId> issued_cids_;    // CIDs we've issued to peer
        dp::Vector<PeerConnectionId> peer_cids_;        // CIDs peer has given us
        dp::Vector<dp::u64> pending_retire_cids_;       // CIDs we need to retire
        dp::u64 next_cid_sequence_ = 0;                 // Next sequence for issued CIDs
        dp::u64 active_peer_cid_sequence_ = 0;          // Currently active peer CID
        dp::Vector<dp::u8> peer_stateless_reset_token_; // Peer's reset token from transport params

        // Generate stateless reset token
        dp::Vector<dp::u8> generate_stateless_reset_token() {
            auto bytes = keylock::crypto::Common::generate_random_bytes(16);
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

        // Check and handle loss detection timeout (PTO)
        // Returns packets to send as probes
        dp::Vector<dp::Vector<dp::u8>> handle_loss_detection_timeout() {
            dp::Vector<dp::Vector<dp::u8>> probe_packets;

            auto [has_timer, deadline] = loss_detection_.get_loss_detection_timer();
            if (!has_timer) {
                return probe_packets;
            }

            auto now = std::chrono::steady_clock::now();
            if (now < deadline) {
                // Timer hasn't fired yet
                return probe_packets;
            }

            // Timer fired - handle the timeout
            loss_detection_.on_loss_detection_timeout();

            // Check each space for packets to probe
            // Send 1-2 probe packets as per RFC 9002

            // Priority order: Initial > Handshake > ApplicationData
            if (state_ == ConnectionState::Handshaking) {
                // During handshake, probe Initial or Handshake space
                if (loss_detection_.has_unacked_data(PacketNumberSpace::Initial)) {
                    auto probe = send_pto_probe(PacketNumberSpace::Initial);
                    if (!probe.empty())
                        probe_packets.push_back(std::move(probe));
                }
                if (loss_detection_.has_unacked_data(PacketNumberSpace::Handshake)) {
                    auto probe = send_pto_probe(PacketNumberSpace::Handshake);
                    if (!probe.empty())
                        probe_packets.push_back(std::move(probe));
                }
            } else if (state_ == ConnectionState::Connected) {
                // After handshake, probe ApplicationData space
                if (loss_detection_.has_unacked_data(PacketNumberSpace::ApplicationData)) {
                    auto probe = send_pto_probe(PacketNumberSpace::ApplicationData);
                    if (!probe.empty())
                        probe_packets.push_back(std::move(probe));
                    // Optionally send a second probe
                    if (loss_detection_.pto_count() >= 2) {
                        auto probe2 = send_pto_probe(PacketNumberSpace::ApplicationData);
                        if (!probe2.empty())
                            probe_packets.push_back(std::move(probe2));
                    }
                }
            }

            echo::debug("PTO timeout handled, sent ", probe_packets.size(), " probe packets");
            return probe_packets;
        }

        // Send a PTO probe packet for the given packet number space
        // Returns the serialized probe packet, or empty vector on failure
        dp::Vector<dp::u8> send_pto_probe(PacketNumberSpace space) {
            echo::debug("Sending PTO probe for space ", static_cast<int>(space));

            if (space == PacketNumberSpace::Initial) {
                // For Initial space, resend pending crypto data or send PING
                if (has_pending_crypto()) {
                    auto crypto_data = take_crypto_data();
                    // Re-queue it since we're just probing
                    pending_crypto_data_.insert(pending_crypto_data_.end(), crypto_data.begin(), crypto_data.end());

                    // Build Initial packet with crypto data
                    auto pkt_result = build_initial_packet(crypto_data, {});
                    if (pkt_result.is_ok()) {
                        return std::move(pkt_result.value());
                    }
                } else {
                    // Send PING frame
                    dp::Vector<dp::u8> ping_frame = {0x01}; // PING frame type
                    auto pkt_result = build_initial_packet({}, ping_frame);
                    if (pkt_result.is_ok()) {
                        return std::move(pkt_result.value());
                    }
                }
            } else if (space == PacketNumberSpace::Handshake) {
                // For Handshake space, send PING frame
                dp::Vector<dp::u8> ping_frame = {0x01};
                auto pkt_result = build_handshake_packet(ping_frame);
                if (pkt_result.is_ok()) {
                    return std::move(pkt_result.value());
                }
            } else if (space == PacketNumberSpace::ApplicationData) {
                // For ApplicationData, check for pending stream data first
                auto stream_frames = get_pending_stream_frames(100);
                if (!stream_frames.empty()) {
                    auto pkt_result = build_short_packet(stream_frames);
                    if (pkt_result.is_ok()) {
                        return std::move(pkt_result.value());
                    }
                }

                // Otherwise send PING
                dp::Vector<dp::u8> ping_frame = {0x01};
                auto pkt_result = build_short_packet(ping_frame);
                if (pkt_result.is_ok()) {
                    return std::move(pkt_result.value());
                }
            }

            return {};
        }

        // Get the deadline for the next loss detection timer
        std::pair<bool, std::chrono::steady_clock::time_point> get_loss_detection_timer_deadline() const {
            return loss_detection_.get_loss_detection_timer();
        }
    };

} // namespace netpipe::quic

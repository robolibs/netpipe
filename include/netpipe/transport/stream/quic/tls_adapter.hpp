#pragma once

#include <chrono>
#include <datapod/datapod.hpp>
#include <echo/echo.hpp>
#include <keylock/cert/certificate.hpp>
#include <keylock/crypto/common.hpp>
#include <keylock/crypto/context.hpp>
#include <keylock/hash/context.hpp>
#include <mutex>
#include <netpipe/security/tls/extensions.hpp>
#include <netpipe/security/tls/key_schedule.hpp>
#include <netpipe/security/tls/messages.hpp>
#include <netpipe/transport/stream/quic/crypto.hpp>
#include <netpipe/transport/stream/quic/transport_params.hpp>
#include <netpipe/transport/stream/quic/types.hpp>
#include <netpipe/transport/stream/quic/varint.hpp>
#include <unordered_set>

#include <sodium.h>

namespace netpipe::quic {

    // Anti-replay store for 0-RTT early data (RFC 8446 Section 8)
    // Uses a time-based window with a set of seen ClientHello hashes
    class AntiReplayStore {
      public:
        AntiReplayStore(dp::u64 window_ms = 10000) : window_ms_(window_ms) {}

        // Check if this ClientHello has been seen before (replay attack)
        // Returns true if this is a fresh request (not a replay)
        bool check(const dp::Vector<dp::u8> &client_hello_hash) {
            std::lock_guard<std::mutex> lock(mutex_);

            cleanup_expired();

            // Convert hash to string for storage
            std::string hash_str(client_hello_hash.begin(), client_hello_hash.end());

            // Check if we've seen this hash before
            if (seen_hashes_.find(hash_str) != seen_hashes_.end()) {
                echo::warn("AntiReplay: Potential replay detected");
                return false;
            }

            // Record this hash
            auto now = current_time_ms();
            seen_hashes_.insert(hash_str);
            timestamps_[hash_str] = now;

            return true;
        }

        // Clear expired entries
        void cleanup_expired() {
            auto now = current_time_ms();
            auto cutoff = now > window_ms_ ? now - window_ms_ : 0;

            std::vector<std::string> expired;
            for (const auto &[hash, ts] : timestamps_) {
                if (ts < cutoff) {
                    expired.push_back(hash);
                }
            }

            for (const auto &hash : expired) {
                seen_hashes_.erase(hash);
                timestamps_.erase(hash);
            }
        }

        // Get current stats
        dp::usize size() const {
            std::lock_guard<std::mutex> lock(mutex_);
            return seen_hashes_.size();
        }

      private:
        dp::u64 current_time_ms() const {
            return static_cast<dp::u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now().time_since_epoch())
                                            .count());
        }

        dp::u64 window_ms_;
        std::unordered_set<std::string> seen_hashes_;
        std::unordered_map<std::string, dp::u64> timestamps_;
        mutable std::mutex mutex_;
    };

    // QUIC TLS configuration
    struct QuicTlsConfig {
        // Certificate (DER-encoded) - required for server
        dp::Vector<dp::u8> certificate;

        // Private key (Ed25519) - required for server
        dp::Vector<dp::u8> private_key;

        // Server name (SNI) - for client
        dp::String server_name;

        // Skip certificate verification (testing only!)
        bool skip_cert_verification = false;

        // QUIC transport parameters to send
        TransportParameters transport_params;

        // Session ticket for 0-RTT (client only)
        dp::Optional<tls::SessionTicket> session_ticket;

        // Enable 0-RTT early data (client only)
        bool enable_early_data = false;

        // Max early data size (server only, advertised via NewSessionTicket)
        dp::u32 max_early_data_size = 16384;
    };

    // TLS handshake states for QUIC
    enum class QuicTlsState {
        Start,
        WaitServerHello,
        WaitEncryptedExtensions,
        WaitCertificate,
        WaitCertificateVerify,
        WaitFinished,
        WaitClientFinished, // Server only
        Connected,
        Error,
        // 0-RTT states
        EarlyData, // Client: sent early data, waiting for server response
    };

    // QUIC TLS Adapter - handles TLS 1.3 handshake for QUIC
    // Unlike regular TLS, QUIC doesn't use the TLS record layer.
    // TLS handshake messages are sent in CRYPTO frames.
    class QuicTlsAdapter {
      public:
        explicit QuicTlsAdapter(bool is_client, const QuicTlsConfig &config = {})
            : is_client_(is_client), config_(config), state_(QuicTlsState::Start) {
            echo::trace("QuicTlsAdapter created: ", is_client ? "client" : "server");

            // Initialize key schedule
            key_schedule_.init();

            // Generate X25519 keypair for key exchange
            generate_x25519_keypair();
        }

        // Get current state
        QuicTlsState state() const { return state_; }

        // Check if handshake is complete
        bool is_complete() const { return state_ == QuicTlsState::Connected; }

        // Check if in error state
        bool is_error() const { return state_ == QuicTlsState::Error; }

        // Get encryption level for current state
        EncryptionLevel current_level() const {
            switch (state_) {
            case QuicTlsState::Start:
            case QuicTlsState::WaitServerHello:
                return EncryptionLevel::Initial;
            case QuicTlsState::WaitEncryptedExtensions:
            case QuicTlsState::WaitCertificate:
            case QuicTlsState::WaitCertificateVerify:
            case QuicTlsState::WaitFinished:
            case QuicTlsState::WaitClientFinished:
                return EncryptionLevel::Handshake;
            case QuicTlsState::Connected:
                return EncryptionLevel::OneRTT;
            default:
                return EncryptionLevel::Initial;
            }
        }

        // CLIENT: Create ClientHello for CRYPTO frame
        // Returns raw TLS handshake message (no record layer!)
        dp::Res<dp::Vector<dp::u8>> create_client_hello() {
            if (!is_client_ || state_ != QuicTlsState::Start) {
                return dp::result::err(dp::Error::invalid_argument("invalid state for create_client_hello"));
            }

            echo::trace("Creating QUIC ClientHello");

            tls::ClientHello ch;
            ch.generate_random();
            client_random_ = dp::Vector<dp::u8>(ch.random.begin(), ch.random.end());

            // Generate a random session ID for compatibility
            auto session_id = keylock::crypto::Common::generate_random_bytes(32);
            ch.legacy_session_id = dp::Vector<dp::u8>(session_id.begin(), session_id.end());

            // Cipher suites - QUIC prefers ChaCha20-Poly1305 for software implementations
            if (crypto_aead_aes256gcm_is_available()) {
                ch.cipher_suites = {tls::TLS_AES_256_GCM_SHA384, tls::TLS_CHACHA20_POLY1305_SHA256};
            } else {
                ch.cipher_suites = {tls::TLS_CHACHA20_POLY1305_SHA256};
            }

            // Build extensions
            // 1. supported_versions (TLS 1.3)
            tls::SupportedVersionsExtension sv_ext;
            sv_ext.versions = {tls::TLS_1_3};
            ch.extensions.push_back(sv_ext.serialize_client());

            // 2. supported_groups (X25519)
            tls::SupportedGroupsExtension sg_ext;
            sg_ext.groups = {tls::NamedGroup::X25519};
            ch.extensions.push_back(sg_ext.serialize());

            // 3. signature_algorithms (Ed25519)
            tls::SignatureAlgorithmsExtension sa_ext;
            sa_ext.algorithms = {tls::SignatureScheme::Ed25519};
            ch.extensions.push_back(sa_ext.serialize());

            // 4. key_share (our X25519 public key)
            tls::KeyShareClientHello ks_ext;
            ks_ext.client_shares.push_back({tls::NamedGroup::X25519, our_x25519_public_});
            ch.extensions.push_back(ks_ext.serialize());

            // 5. QUIC transport parameters extension (0x39)
            auto tp_ext = serialize_quic_transport_params();
            ch.extensions.push_back(tp_ext);

            // Serialize handshake message (NOT wrapped in record layer)
            auto ch_bytes = ch.serialize();
            update_transcript(ch_bytes);

            state_ = QuicTlsState::WaitServerHello;
            echo::debug("QUIC ClientHello created (", ch_bytes.size(), " bytes)");

            return dp::result::ok(std::move(ch_bytes));
        }

        // CLIENT: Process ServerHello from CRYPTO frame
        dp::Res<void> process_server_hello(const dp::Vector<dp::u8> &data) {
            if (!is_client_ || state_ != QuicTlsState::WaitServerHello) {
                return dp::result::err(dp::Error::invalid_argument("invalid state for process_server_hello"));
            }

            echo::trace("Processing QUIC ServerHello");

            // Parse handshake header
            auto header_result = tls::parse_handshake_header(data.data(), data.size());
            if (header_result.is_err()) {
                state_ = QuicTlsState::Error;
                return dp::result::err(header_result.error());
            }

            auto [msg_type, msg_length] = header_result.value();
            if (msg_type != tls::HandshakeType::ServerHello) {
                state_ = QuicTlsState::Error;
                return dp::result::err(dp::Error::invalid_argument("expected ServerHello"));
            }

            // Parse ServerHello
            auto sh_result = tls::ServerHello::parse(data.data() + tls::HANDSHAKE_HEADER_SIZE, msg_length);
            if (sh_result.is_err()) {
                state_ = QuicTlsState::Error;
                return dp::result::err(sh_result.error());
            }

            auto &sh = sh_result.value();
            server_random_ = dp::Vector<dp::u8>(sh.random.begin(), sh.random.end());
            selected_cipher_suite_ = sh.cipher_suite;

            // Find key_share extension
            bool found_key_share = false;
            for (const auto &ext : sh.extensions) {
                if (ext.type == tls::ExtensionType::KeyShare) {
                    auto ks_result = tls::KeyShareServerHello::parse(ext.data);
                    if (ks_result.is_err()) {
                        state_ = QuicTlsState::Error;
                        return dp::result::err(ks_result.error());
                    }
                    peer_x25519_public_ = ks_result.value().server_share.key_exchange;
                    found_key_share = true;
                }
            }

            if (!found_key_share) {
                state_ = QuicTlsState::Error;
                return dp::result::err(dp::Error::invalid_argument("ServerHello missing key_share"));
            }

            // Update transcript
            update_transcript(data);

            // Compute shared secret
            auto shared_secret = compute_x25519_shared_secret();
            if (shared_secret.empty()) {
                state_ = QuicTlsState::Error;
                return dp::result::err(dp::Error::io_error("X25519 key exchange failed"));
            }

            // Derive handshake secrets
            auto hello_hash = tls::transcript_hash(transcript_);
            key_schedule_.derive_handshake_secrets(shared_secret, hello_hash);

            // Derive QUIC handshake keys
            derive_handshake_keys();

            state_ = QuicTlsState::WaitEncryptedExtensions;
            echo::debug("QUIC ServerHello processed, handshake keys derived");

            return dp::result::ok();
        }

        // CLIENT: Process encrypted handshake message (EncryptedExtensions, Certificate, etc.)
        dp::Res<void> process_handshake_message(const dp::Vector<dp::u8> &data) {
            if (!is_client_) {
                return dp::result::err(dp::Error::invalid_argument("client-only method"));
            }

            // Parse handshake header
            auto header_result = tls::parse_handshake_header(data.data(), data.size());
            if (header_result.is_err()) {
                state_ = QuicTlsState::Error;
                return dp::result::err(header_result.error());
            }

            auto [msg_type, msg_length] = header_result.value();
            const dp::u8 *msg_data = data.data() + tls::HANDSHAKE_HEADER_SIZE;

            switch (state_) {
            case QuicTlsState::WaitEncryptedExtensions:
                if (msg_type != tls::HandshakeType::EncryptedExtensions) {
                    state_ = QuicTlsState::Error;
                    return dp::result::err(dp::Error::invalid_argument("expected EncryptedExtensions"));
                }
                // Parse to extract transport params if present
                update_transcript(data);
                state_ = QuicTlsState::WaitCertificate;
                echo::debug("EncryptedExtensions received");
                break;

            case QuicTlsState::WaitCertificate:
                if (msg_type != tls::HandshakeType::Certificate) {
                    state_ = QuicTlsState::Error;
                    return dp::result::err(dp::Error::invalid_argument("expected Certificate"));
                }
                {
                    auto cert_result = tls::Certificate::parse(msg_data, msg_length);
                    if (cert_result.is_err()) {
                        state_ = QuicTlsState::Error;
                        return dp::result::err(cert_result.error());
                    }
                    peer_certificate_ = cert_result.value();
                }
                update_transcript(data);
                state_ = QuicTlsState::WaitCertificateVerify;
                echo::debug("Certificate received");
                break;

            case QuicTlsState::WaitCertificateVerify:
                if (msg_type != tls::HandshakeType::CertificateVerify) {
                    state_ = QuicTlsState::Error;
                    return dp::result::err(dp::Error::invalid_argument("expected CertificateVerify"));
                }
                {
                    auto cv_result = tls::CertificateVerify::parse(msg_data, msg_length);
                    if (cv_result.is_err()) {
                        state_ = QuicTlsState::Error;
                        return dp::result::err(cv_result.error());
                    }

                    // Verify signature (if not skipping verification)
                    if (!config_.skip_cert_verification) {
                        auto verify_result = verify_certificate_verify(cv_result.value());
                        if (verify_result.is_err()) {
                            state_ = QuicTlsState::Error;
                            return dp::result::err(verify_result.error());
                        }
                    }
                }
                update_transcript(data);
                state_ = QuicTlsState::WaitFinished;
                echo::debug("CertificateVerify received and verified");
                break;

            case QuicTlsState::WaitFinished:
                if (msg_type != tls::HandshakeType::Finished) {
                    state_ = QuicTlsState::Error;
                    return dp::result::err(dp::Error::invalid_argument("expected Finished"));
                }
                {
                    auto fin_result = tls::Finished::parse(msg_data, msg_length);
                    if (fin_result.is_err()) {
                        state_ = QuicTlsState::Error;
                        return dp::result::err(fin_result.error());
                    }

                    // Verify Finished
                    auto verify_result = verify_server_finished(fin_result.value());
                    if (verify_result.is_err()) {
                        state_ = QuicTlsState::Error;
                        return dp::result::err(verify_result.error());
                    }

                    update_transcript(data);

                    // Derive application secrets
                    auto full_hash = tls::transcript_hash(transcript_);
                    key_schedule_.derive_application_secrets(full_hash);

                    // Derive QUIC application keys
                    derive_application_keys();

                    server_finished_received_ = true;
                    echo::debug("Server Finished received and verified");
                }
                break;

            default:
                state_ = QuicTlsState::Error;
                return dp::result::err(dp::Error::invalid_argument("unexpected handshake state"));
            }

            return dp::result::ok();
        }

        // CLIENT: Create client Finished message
        dp::Res<dp::Vector<dp::u8>> create_client_finished() {
            if (!is_client_ || !server_finished_received_) {
                return dp::result::err(dp::Error::invalid_argument("invalid state for create_client_finished"));
            }

            echo::trace("Creating client Finished");

            // Compute verify_data
            auto finished_key = tls::KeySchedule::derive_finished_key(key_schedule_.client_handshake_traffic_secret);
            auto hash = tls::transcript_hash(transcript_);
            auto verify_data = tls::Finished::compute_verify_data(finished_key, hash);

            tls::Finished fin;
            fin.verify_data = std::move(verify_data);

            auto fin_bytes = fin.serialize();
            update_transcript(fin_bytes);

            state_ = QuicTlsState::Connected;
            echo::debug("Client Finished created, handshake complete");

            return dp::result::ok(std::move(fin_bytes));
        }

        // SERVER: Process ClientHello and generate response messages
        // Returns: ServerHello, EncryptedExtensions, Certificate, CertificateVerify, Finished
        // as separate messages (caller wraps each in appropriate CRYPTO frames)
        dp::Res<dp::Vector<dp::Vector<dp::u8>>> process_client_hello_and_respond(const dp::Vector<dp::u8> &data) {
            if (is_client_ || state_ != QuicTlsState::Start) {
                return dp::result::err(dp::Error::invalid_argument("invalid state for process_client_hello"));
            }

            echo::trace("Processing QUIC ClientHello");

            // Parse handshake header
            auto header_result = tls::parse_handshake_header(data.data(), data.size());
            if (header_result.is_err()) {
                state_ = QuicTlsState::Error;
                return dp::result::err(header_result.error());
            }

            auto [msg_type, msg_length] = header_result.value();
            if (msg_type != tls::HandshakeType::ClientHello) {
                state_ = QuicTlsState::Error;
                return dp::result::err(dp::Error::invalid_argument("expected ClientHello"));
            }

            // Parse ClientHello
            auto ch_result = tls::ClientHello::parse(data.data() + tls::HANDSHAKE_HEADER_SIZE, msg_length);
            if (ch_result.is_err()) {
                state_ = QuicTlsState::Error;
                return dp::result::err(ch_result.error());
            }

            auto &ch = ch_result.value();
            client_random_ = dp::Vector<dp::u8>(ch.random.begin(), ch.random.end());

            // Find supported cipher suite
            bool found_cipher = false;
            bool aes_available = crypto_aead_aes256gcm_is_available();

            if (aes_available) {
                for (auto cs : ch.cipher_suites) {
                    if (cs == tls::TLS_AES_256_GCM_SHA384 || cs == tls::TLS_AES_128_GCM_SHA256) {
                        selected_cipher_suite_ = cs;
                        found_cipher = true;
                        break;
                    }
                }
            }

            if (!found_cipher) {
                for (auto cs : ch.cipher_suites) {
                    if (cs == tls::TLS_CHACHA20_POLY1305_SHA256) {
                        selected_cipher_suite_ = cs;
                        found_cipher = true;
                        break;
                    }
                }
            }

            if (!found_cipher) {
                state_ = QuicTlsState::Error;
                return dp::result::err(dp::Error::invalid_argument("no supported cipher suite"));
            }

            // Find key_share extension with X25519
            bool found_key_share = false;
            bool found_psk = false;
            for (const auto &ext : ch.extensions) {
                if (ext.type == tls::ExtensionType::KeyShare) {
                    auto ks_result = tls::KeyShareClientHello::parse(ext.data);
                    if (ks_result.is_err()) {
                        continue;
                    }

                    for (const auto &share : ks_result.value().client_shares) {
                        if (share.group == tls::NamedGroup::X25519) {
                            peer_x25519_public_ = share.key_exchange;
                            found_key_share = true;
                            break;
                        }
                    }
                }

                // Extract QUIC transport parameters if present
                if (ext.type == static_cast<tls::ExtensionType>(0x39)) {
                    // Parse peer transport params
                    parse_quic_transport_params(ext.data);
                }

                // Detect early_data extension
                if (ext.type == tls::ExtensionType::EarlyData) {
                    client_offered_early_data_ = true;
                    echo::debug("Client offered early data");
                }

                // Detect pre_shared_key extension (indicates PSK-based resumption)
                if (ext.type == tls::ExtensionType::PreSharedKey) {
                    found_psk = true;
                    // TODO: Parse PSK identities and validate ticket
                    // For now, just note that PSK was offered
                    echo::debug("Client offered PSK");
                }
            }

            if (!found_key_share) {
                state_ = QuicTlsState::Error;
                return dp::result::err(dp::Error::invalid_argument("ClientHello missing X25519 key_share"));
            }

            // Update transcript with ClientHello
            update_transcript(data);

            dp::Vector<dp::Vector<dp::u8>> messages;

            // Create ServerHello
            tls::ServerHello sh;
            sh.generate_random();
            server_random_ = dp::Vector<dp::u8>(sh.random.begin(), sh.random.end());
            sh.legacy_session_id_echo = ch.legacy_session_id;
            sh.cipher_suite = selected_cipher_suite_;

            // Add extensions
            sh.extensions.push_back(tls::SupportedVersionsExtension::serialize_server(tls::TLS_1_3));

            tls::KeyShareServerHello ks_ext;
            ks_ext.server_share = {tls::NamedGroup::X25519, our_x25519_public_};
            sh.extensions.push_back(ks_ext.serialize());

            auto sh_bytes = sh.serialize();
            update_transcript(sh_bytes);
            messages.push_back(std::move(sh_bytes));

            // Compute shared secret and derive handshake keys
            auto shared_secret = compute_x25519_shared_secret();
            if (shared_secret.empty()) {
                state_ = QuicTlsState::Error;
                return dp::result::err(dp::Error::io_error("X25519 key exchange failed"));
            }

            auto hello_hash = tls::transcript_hash(transcript_);
            key_schedule_.derive_handshake_secrets(shared_secret, hello_hash);

            // Derive QUIC handshake keys
            derive_handshake_keys();

            // EncryptedExtensions with QUIC transport params
            tls::EncryptedExtensions ee;
            ee.extensions.push_back(serialize_quic_transport_params());

            // Add early_data extension if we're accepting early data
            if (early_data_accepted_) {
                tls::Extension ed_ext;
                ed_ext.type = tls::ExtensionType::EarlyData;
                ed_ext.data = {}; // Empty for EncryptedExtensions
                ee.extensions.push_back(ed_ext);
                echo::debug("Server accepting early data (added to EncryptedExtensions)");
            }

            auto ee_bytes = ee.serialize();
            update_transcript(ee_bytes);
            messages.push_back(std::move(ee_bytes));

            // Certificate
            tls::Certificate cert;
            tls::CertificateEntry cert_entry;
            cert_entry.cert_data = config_.certificate;
            cert.certificate_list.push_back(std::move(cert_entry));

            auto cert_bytes = cert.serialize();
            update_transcript(cert_bytes);
            messages.push_back(std::move(cert_bytes));

            // CertificateVerify
            auto cv_result = create_certificate_verify();
            if (cv_result.is_err()) {
                state_ = QuicTlsState::Error;
                return dp::result::err(cv_result.error());
            }
            auto cv_bytes = cv_result.value();
            update_transcript(cv_bytes);
            messages.push_back(std::move(cv_bytes));

            // Derive application secrets before Finished
            auto full_hash = tls::transcript_hash(transcript_);
            key_schedule_.derive_application_secrets(full_hash);
            derive_application_keys();

            // Finished
            auto finished_key = tls::KeySchedule::derive_finished_key(key_schedule_.server_handshake_traffic_secret);
            auto hash = tls::transcript_hash(transcript_);
            auto verify_data = tls::Finished::compute_verify_data(finished_key, hash);

            tls::Finished fin;
            fin.verify_data = std::move(verify_data);

            auto fin_bytes = fin.serialize();
            update_transcript(fin_bytes);
            messages.push_back(std::move(fin_bytes));

            state_ = QuicTlsState::WaitClientFinished;
            echo::debug("Server handshake messages created, waiting for client Finished");

            return dp::result::ok(std::move(messages));
        }

        // SERVER: Process client Finished
        dp::Res<void> process_client_finished(const dp::Vector<dp::u8> &data) {
            if (is_client_ || state_ != QuicTlsState::WaitClientFinished) {
                return dp::result::err(dp::Error::invalid_argument("invalid state for process_client_finished"));
            }

            echo::trace("Processing client Finished");

            // Parse handshake header
            auto header_result = tls::parse_handshake_header(data.data(), data.size());
            if (header_result.is_err()) {
                state_ = QuicTlsState::Error;
                return dp::result::err(header_result.error());
            }

            auto [msg_type, msg_length] = header_result.value();
            if (msg_type != tls::HandshakeType::Finished) {
                state_ = QuicTlsState::Error;
                return dp::result::err(dp::Error::invalid_argument("expected Finished"));
            }

            // Parse Finished
            auto fin_result = tls::Finished::parse(data.data() + tls::HANDSHAKE_HEADER_SIZE, msg_length);
            if (fin_result.is_err()) {
                state_ = QuicTlsState::Error;
                return dp::result::err(fin_result.error());
            }

            // Verify client Finished
            auto finished_key = tls::KeySchedule::derive_finished_key(key_schedule_.client_handshake_traffic_secret);
            auto hash = tls::transcript_hash(transcript_);
            auto expected_verify_data = tls::Finished::compute_verify_data(finished_key, hash);

            if (fin_result.value().verify_data != expected_verify_data) {
                state_ = QuicTlsState::Error;
                return dp::result::err(dp::Error::invalid_argument("client Finished verify_data mismatch"));
            }

            state_ = QuicTlsState::Connected;
            echo::debug("Client Finished verified, handshake complete");

            return dp::result::ok();
        }

        // Get derived keys for each encryption level
        QuicKeys handshake_send_keys() const { return handshake_send_keys_; }
        QuicKeys handshake_recv_keys() const { return handshake_recv_keys_; }
        QuicKeys application_send_keys() const { return application_send_keys_; }
        QuicKeys application_recv_keys() const { return application_recv_keys_; }
        QuicKeys early_data_keys() const { return early_data_keys_; }

        // Check if keys are available
        bool has_handshake_keys() const { return handshake_send_keys_.is_valid(); }
        bool has_application_keys() const { return application_send_keys_.is_valid(); }
        bool has_early_data_keys() const { return early_data_keys_.is_valid(); }

        // Get peer transport parameters (after ClientHello/ServerHello processed)
        const TransportParameters &peer_transport_params() const { return peer_transport_params_; }

        // Check if 0-RTT is available (client only)
        bool can_send_early_data() const {
            return is_client_ && config_.enable_early_data && config_.session_ticket.has_value() &&
                   config_.session_ticket->is_valid();
        }

        // CLIENT: Create ClientHello with PSK for 0-RTT
        // Returns ClientHello message and also derives early data keys if available
        dp::Res<dp::Vector<dp::u8>> create_client_hello_with_psk() {
            if (!is_client_ || state_ != QuicTlsState::Start) {
                return dp::result::err(dp::Error::invalid_argument("invalid state for create_client_hello_with_psk"));
            }

            if (!config_.session_ticket.has_value() || !config_.session_ticket->is_valid()) {
                // No valid session ticket, fall back to regular ClientHello
                return create_client_hello();
            }

            echo::trace("Creating QUIC ClientHello with PSK (0-RTT)");

            // Compute PSK from session ticket
            auto psk = config_.session_ticket->compute_psk();

            // Initialize key schedule with PSK
            key_schedule_.init_with_psk(psk);

            tls::ClientHello ch;
            ch.generate_random();
            client_random_ = dp::Vector<dp::u8>(ch.random.begin(), ch.random.end());

            // Generate a random session ID for compatibility
            auto session_id = keylock::crypto::Common::generate_random_bytes(32);
            ch.legacy_session_id = dp::Vector<dp::u8>(session_id.begin(), session_id.end());

            // Cipher suites - must include the one from the session ticket
            ch.cipher_suites = {config_.session_ticket->cipher_suite};

            // Add other cipher suites as fallback
            if (crypto_aead_aes256gcm_is_available()) {
                if (config_.session_ticket->cipher_suite != tls::TLS_AES_256_GCM_SHA384) {
                    ch.cipher_suites.push_back(tls::TLS_AES_256_GCM_SHA384);
                }
            }
            if (config_.session_ticket->cipher_suite != tls::TLS_CHACHA20_POLY1305_SHA256) {
                ch.cipher_suites.push_back(tls::TLS_CHACHA20_POLY1305_SHA256);
            }

            // Build extensions
            // 1. supported_versions (TLS 1.3)
            tls::SupportedVersionsExtension sv_ext;
            sv_ext.versions = {tls::TLS_1_3};
            ch.extensions.push_back(sv_ext.serialize_client());

            // 2. supported_groups (X25519)
            tls::SupportedGroupsExtension sg_ext;
            sg_ext.groups = {tls::NamedGroup::X25519};
            ch.extensions.push_back(sg_ext.serialize());

            // 3. signature_algorithms (Ed25519)
            tls::SignatureAlgorithmsExtension sa_ext;
            sa_ext.algorithms = {tls::SignatureScheme::Ed25519};
            ch.extensions.push_back(sa_ext.serialize());

            // 4. key_share (our X25519 public key)
            tls::KeyShareClientHello ks_ext;
            ks_ext.client_shares.push_back({tls::NamedGroup::X25519, our_x25519_public_});
            ch.extensions.push_back(ks_ext.serialize());

            // 5. QUIC transport parameters extension
            auto tp_ext = serialize_quic_transport_params();
            ch.extensions.push_back(tp_ext);

            // 6. pre_shared_key extension (must be last!)
            auto psk_ext = serialize_psk_extension();
            ch.extensions.push_back(psk_ext);

            // 7. early_data extension (indicates we want to send early data)
            if (config_.enable_early_data) {
                tls::Extension ed_ext;
                ed_ext.type = tls::ExtensionType::EarlyData;
                ed_ext.data = {}; // Empty for ClientHello
                ch.extensions.push_back(ed_ext);
            }

            // Serialize handshake message
            auto ch_bytes = ch.serialize();

            // Compute binder (HMAC over partial transcript)
            // Note: PSK binder goes over ClientHello up to (not including) the binder itself
            auto binder = compute_psk_binder(ch_bytes, psk);

            // Insert binder into the serialized ClientHello
            // The binder is at the end of the pre_shared_key extension
            insert_psk_binder(ch_bytes, binder);

            update_transcript(ch_bytes);

            // Derive early data keys if we can send early data
            if (config_.enable_early_data) {
                auto ch_hash = tls::transcript_hash(ch_bytes);
                key_schedule_.derive_early_secrets(ch_hash);
                derive_early_data_keys();
                early_data_enabled_ = true;
            }

            state_ = QuicTlsState::WaitServerHello;
            echo::debug("QUIC ClientHello with PSK created (", ch_bytes.size(), " bytes)");

            return dp::result::ok(std::move(ch_bytes));
        }

        // SERVER: Create NewSessionTicket message
        // Call after handshake is complete to enable session resumption
        dp::Res<dp::Vector<dp::u8>> create_new_session_ticket() {
            if (is_client_ || state_ != QuicTlsState::Connected) {
                return dp::result::err(dp::Error::invalid_argument("invalid state for create_new_session_ticket"));
            }

            echo::trace("Creating NewSessionTicket");

            // First, derive resumption master secret if not already done
            if (key_schedule_.resumption_master_secret.empty()) {
                auto full_hash = tls::transcript_hash(transcript_);
                key_schedule_.derive_resumption_master_secret(full_hash);
            }

            tls::NewSessionTicket nst;

            // Ticket lifetime: 1 day (in seconds)
            nst.ticket_lifetime = 86400;

            // Random ticket_age_add for obfuscation
            auto random_bytes = keylock::crypto::Common::generate_random_bytes(4);
            nst.ticket_age_add = (static_cast<dp::u32>(random_bytes[0]) << 24) |
                                 (static_cast<dp::u32>(random_bytes[1]) << 16) |
                                 (static_cast<dp::u32>(random_bytes[2]) << 8) | static_cast<dp::u32>(random_bytes[3]);

            // Generate ticket nonce (used to derive PSK)
            auto nonce_bytes = keylock::crypto::Common::generate_random_bytes(8);
            nst.ticket_nonce = dp::Vector<dp::u8>(nonce_bytes.begin(), nonce_bytes.end());

            // The ticket itself contains encrypted session state
            // For simplicity, we'll include: resumption_master_secret + cipher_suite
            // In production, this should be encrypted with a server-side key
            nst.ticket = create_ticket_data(nst.ticket_nonce);

            // Add early_data extension if we accept early data
            if (config_.max_early_data_size > 0) {
                tls::Extension ed_ext;
                ed_ext.type = tls::ExtensionType::EarlyData;
                // max_early_data_size as 4-byte big-endian
                ed_ext.data.push_back(static_cast<dp::u8>((config_.max_early_data_size >> 24) & 0xFF));
                ed_ext.data.push_back(static_cast<dp::u8>((config_.max_early_data_size >> 16) & 0xFF));
                ed_ext.data.push_back(static_cast<dp::u8>((config_.max_early_data_size >> 8) & 0xFF));
                ed_ext.data.push_back(static_cast<dp::u8>(config_.max_early_data_size & 0xFF));
                nst.extensions.push_back(ed_ext);
            }

            auto nst_bytes = nst.serialize();
            echo::debug("NewSessionTicket created (", nst_bytes.size(), " bytes)");

            return dp::result::ok(std::move(nst_bytes));
        }

        // CLIENT: Process NewSessionTicket and store for future connections
        dp::Res<tls::SessionTicket> process_new_session_ticket(const dp::Vector<dp::u8> &data) {
            if (!is_client_ || state_ != QuicTlsState::Connected) {
                return dp::result::err(dp::Error::invalid_argument("invalid state for process_new_session_ticket"));
            }

            echo::trace("Processing NewSessionTicket");

            // Parse handshake header
            auto header_result = tls::parse_handshake_header(data.data(), data.size());
            if (header_result.is_err()) {
                return dp::result::err(header_result.error());
            }

            auto [msg_type, msg_length] = header_result.value();
            if (msg_type != tls::HandshakeType::NewSessionTicket) {
                return dp::result::err(dp::Error::invalid_argument("expected NewSessionTicket"));
            }

            // Parse NewSessionTicket
            auto nst_result = tls::NewSessionTicket::parse(data.data() + tls::HANDSHAKE_HEADER_SIZE, msg_length);
            if (nst_result.is_err()) {
                return dp::result::err(nst_result.error());
            }

            auto &nst = nst_result.value();

            // Derive resumption master secret if not already done
            if (key_schedule_.resumption_master_secret.empty()) {
                auto full_hash = tls::transcript_hash(transcript_);
                key_schedule_.derive_resumption_master_secret(full_hash);
            }

            // Create stored session ticket
            tls::SessionTicket stored;
            stored.ticket = nst.ticket;
            stored.resumption_master_secret = key_schedule_.resumption_master_secret;
            stored.ticket_lifetime = nst.ticket_lifetime;
            stored.ticket_age_add = nst.ticket_age_add;
            stored.ticket_nonce = nst.ticket_nonce;
            stored.cipher_suite = selected_cipher_suite_;
            stored.timestamp_ms = static_cast<dp::u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                           std::chrono::steady_clock::now().time_since_epoch())
                                                           .count());

            echo::debug("NewSessionTicket processed and stored");

            return dp::result::ok(std::move(stored));
        }

        // Check if early data was accepted by server
        bool early_data_accepted() const { return early_data_accepted_; }

        // Mark early data as accepted (called when server EncryptedExtensions contains early_data)
        void set_early_data_accepted(bool accepted) { early_data_accepted_ = accepted; }

        // Check if client offered early data
        bool client_offered_early_data() const { return client_offered_early_data_; }

        // SERVER: Accept early data (must be called after processing ClientHello with early_data)
        // Returns true if early data was offered and we're accepting it
        bool accept_early_data() {
            if (!client_offered_early_data_) {
                return false;
            }

            // Check anti-replay (must be implemented by caller)
            if (!anti_replay_check_passed_) {
                echo::warn("Early data rejected: anti-replay check not passed");
                return false;
            }

            // Derive early data receive keys (server receives with client_early_traffic_secret)
            if (key_schedule_.client_early_traffic_secret.empty()) {
                echo::warn("Early data rejected: no early traffic secret");
                return false;
            }

            early_data_keys_ = derive_quic_keys(key_schedule_.client_early_traffic_secret);
            early_data_accepted_ = true;

            echo::debug("Server accepting early data");
            return true;
        }

        // SERVER: Reject early data (client must retry without early data)
        void reject_early_data() {
            early_data_accepted_ = false;
            echo::debug("Server rejecting early data");
        }

        // Set anti-replay check result (server should call this before accept_early_data)
        void set_anti_replay_check_passed(bool passed) { anti_replay_check_passed_ = passed; }

      private:
        bool is_client_;
        QuicTlsConfig config_;
        QuicTlsState state_;

        // Key schedule
        tls::KeySchedule key_schedule_;

        // Transcript (all handshake messages concatenated)
        dp::Vector<dp::u8> transcript_;

        // Random values
        dp::Vector<dp::u8> client_random_;
        dp::Vector<dp::u8> server_random_;

        // X25519 keys
        dp::Vector<dp::u8> our_x25519_private_;
        dp::Vector<dp::u8> our_x25519_public_;
        dp::Vector<dp::u8> peer_x25519_public_;

        // Selected cipher suite
        dp::u16 selected_cipher_suite_ = 0;

        // Peer certificate
        tls::Certificate peer_certificate_;

        // Peer transport parameters
        TransportParameters peer_transport_params_;

        // QUIC keys for each level
        QuicKeys handshake_send_keys_;
        QuicKeys handshake_recv_keys_;
        QuicKeys application_send_keys_;
        QuicKeys application_recv_keys_;
        QuicKeys early_data_keys_; // 0-RTT keys

        // Flags
        bool server_finished_received_ = false;
        bool early_data_enabled_ = false;
        bool early_data_accepted_ = false;
        bool client_offered_early_data_ = false;
        bool anti_replay_check_passed_ = false;

        // Generate X25519 keypair
        void generate_x25519_keypair() {
            our_x25519_public_.resize(crypto_scalarmult_BYTES);
            our_x25519_private_.resize(crypto_scalarmult_SCALARBYTES);

            crypto_box_keypair(our_x25519_public_.data(), our_x25519_private_.data());

            echo::debug("Generated X25519 keypair");
        }

        // Compute X25519 shared secret
        dp::Vector<dp::u8> compute_x25519_shared_secret() {
            if (peer_x25519_public_.size() != crypto_scalarmult_BYTES) {
                echo::error("Invalid peer X25519 public key size");
                return {};
            }

            dp::Vector<dp::u8> shared_secret(crypto_scalarmult_BYTES);

            if (crypto_scalarmult(shared_secret.data(), our_x25519_private_.data(), peer_x25519_public_.data()) != 0) {
                echo::error("X25519 scalar multiplication failed");
                return {};
            }

            echo::debug("Computed X25519 shared secret");
            return shared_secret;
        }

        // Update transcript
        void update_transcript(const dp::Vector<dp::u8> &message) {
            transcript_.insert(transcript_.end(), message.begin(), message.end());
            echo::trace("Transcript updated, total size=", transcript_.size());
        }

        // Derive QUIC handshake keys from TLS handshake secrets
        void derive_handshake_keys() {
            // For client: send with client_handshake_traffic_secret, recv with server_handshake_traffic_secret
            // For server: send with server_handshake_traffic_secret, recv with client_handshake_traffic_secret

            const auto &send_secret = is_client_ ? key_schedule_.client_handshake_traffic_secret
                                                 : key_schedule_.server_handshake_traffic_secret;
            const auto &recv_secret = is_client_ ? key_schedule_.server_handshake_traffic_secret
                                                 : key_schedule_.client_handshake_traffic_secret;

            handshake_send_keys_ = derive_quic_keys(send_secret);
            handshake_recv_keys_ = derive_quic_keys(recv_secret);

            echo::debug("Derived QUIC handshake keys");
        }

        // Derive QUIC application keys from TLS application secrets
        void derive_application_keys() {
            const auto &send_secret = is_client_ ? key_schedule_.client_application_traffic_secret
                                                 : key_schedule_.server_application_traffic_secret;
            const auto &recv_secret = is_client_ ? key_schedule_.server_application_traffic_secret
                                                 : key_schedule_.client_application_traffic_secret;

            application_send_keys_ = derive_quic_keys(send_secret);
            application_recv_keys_ = derive_quic_keys(recv_secret);

            echo::debug("Derived QUIC application keys");
        }

        // Derive QUIC keys (key, iv, hp) from a traffic secret
        // Uses QUIC-specific labels ("quic key", "quic iv", "quic hp")
        QuicKeys derive_quic_keys(const dp::Vector<dp::u8> &secret) {
            QuicKeys keys;

            // Determine key size from cipher suite
            dp::usize key_length = AEAD_KEY_LENGTH_128;
            if (selected_cipher_suite_ == tls::TLS_AES_256_GCM_SHA384) {
                key_length = AEAD_KEY_LENGTH_256;
            }

            // QUIC uses different labels than TLS
            keys.key = hkdf_expand_label(secret, QUIC_LABEL_KEY, nullptr, 0, key_length);
            keys.iv = hkdf_expand_label(secret, QUIC_LABEL_IV, nullptr, 0, AEAD_IV_LENGTH);
            keys.hp_key = hkdf_expand_label(secret, QUIC_LABEL_HP, nullptr, 0, HEADER_PROTECTION_KEY_LENGTH);

            return keys;
        }

        // Serialize QUIC transport parameters extension
        tls::Extension serialize_quic_transport_params() {
            tls::Extension ext;
            ext.type = static_cast<tls::ExtensionType>(0x39); // QUIC transport parameters

            // Serialize our transport params
            dp::Vector<dp::u8> data;

            // Each parameter is: varint(id) + varint(len) + value

            // initial_max_data
            auto id = varint_encode(0x04);
            auto val = varint_encode(config_.transport_params.initial_max_data);
            auto len = varint_encode(val.size());
            data.insert(data.end(), id.begin(), id.end());
            data.insert(data.end(), len.begin(), len.end());
            data.insert(data.end(), val.begin(), val.end());

            // initial_max_stream_data_bidi_local
            id = varint_encode(0x05);
            val = varint_encode(config_.transport_params.initial_max_stream_data_bidi_local);
            len = varint_encode(val.size());
            data.insert(data.end(), id.begin(), id.end());
            data.insert(data.end(), len.begin(), len.end());
            data.insert(data.end(), val.begin(), val.end());

            // initial_max_stream_data_bidi_remote
            id = varint_encode(0x06);
            val = varint_encode(config_.transport_params.initial_max_stream_data_bidi_remote);
            len = varint_encode(val.size());
            data.insert(data.end(), id.begin(), id.end());
            data.insert(data.end(), len.begin(), len.end());
            data.insert(data.end(), val.begin(), val.end());

            // initial_max_stream_data_uni
            id = varint_encode(0x07);
            val = varint_encode(config_.transport_params.initial_max_stream_data_uni);
            len = varint_encode(val.size());
            data.insert(data.end(), id.begin(), id.end());
            data.insert(data.end(), len.begin(), len.end());
            data.insert(data.end(), val.begin(), val.end());

            // initial_max_streams_bidi
            id = varint_encode(0x08);
            val = varint_encode(config_.transport_params.initial_max_streams_bidi);
            len = varint_encode(val.size());
            data.insert(data.end(), id.begin(), id.end());
            data.insert(data.end(), len.begin(), len.end());
            data.insert(data.end(), val.begin(), val.end());

            // initial_max_streams_uni
            id = varint_encode(0x09);
            val = varint_encode(config_.transport_params.initial_max_streams_uni);
            len = varint_encode(val.size());
            data.insert(data.end(), id.begin(), id.end());
            data.insert(data.end(), len.begin(), len.end());
            data.insert(data.end(), val.begin(), val.end());

            // max_idle_timeout
            id = varint_encode(0x01);
            val = varint_encode(config_.transport_params.max_idle_timeout);
            len = varint_encode(val.size());
            data.insert(data.end(), id.begin(), id.end());
            data.insert(data.end(), len.begin(), len.end());
            data.insert(data.end(), val.begin(), val.end());

            ext.data = std::move(data);
            return ext;
        }

        // Parse QUIC transport parameters from extension data
        void parse_quic_transport_params(const dp::Vector<dp::u8> &data) {
            dp::usize offset = 0;

            while (offset < data.size()) {
                // Parse parameter ID
                auto id_result = varint_decode(data.data() + offset, data.size() - offset);
                if (id_result.is_err()) {
                    break;
                }
                auto [param_id, id_len] = id_result.value();
                offset += id_len;

                // Parse length
                auto len_result = varint_decode(data.data() + offset, data.size() - offset);
                if (len_result.is_err()) {
                    break;
                }
                auto [param_len, len_bytes] = len_result.value();
                offset += len_bytes;

                if (offset + param_len > data.size()) {
                    break;
                }

                // Parse value (as varint for numeric params)
                auto val_result = varint_decode(data.data() + offset, param_len);
                dp::u64 value = 0;
                if (val_result.is_ok()) {
                    value = val_result.value().first;
                }

                // Apply parameter
                switch (param_id) {
                case 0x01:
                    peer_transport_params_.max_idle_timeout = value;
                    break;
                case 0x04:
                    peer_transport_params_.initial_max_data = value;
                    break;
                case 0x05:
                    peer_transport_params_.initial_max_stream_data_bidi_local = value;
                    break;
                case 0x06:
                    peer_transport_params_.initial_max_stream_data_bidi_remote = value;
                    break;
                case 0x07:
                    peer_transport_params_.initial_max_stream_data_uni = value;
                    break;
                case 0x08:
                    peer_transport_params_.initial_max_streams_bidi = value;
                    break;
                case 0x09:
                    peer_transport_params_.initial_max_streams_uni = value;
                    break;
                default:
                    // Unknown parameter - skip
                    break;
                }

                offset += param_len;
            }

            echo::debug("Parsed peer transport parameters");
        }

        // Create CertificateVerify message
        dp::Res<dp::Vector<dp::u8>> create_certificate_verify() {
            if (config_.private_key.empty()) {
                // Testing mode: create an empty/dummy CertificateVerify
                // In production, a valid private key should always be provided
                echo::warn("No private key configured, using dummy CertificateVerify for testing");
                tls::CertificateVerify cv;
                cv.algorithm = tls::SignatureScheme::Ed25519;
                cv.signature.resize(64, 0); // Dummy signature
                return dp::result::ok(cv.serialize());
            }

            // Build content to sign
            auto hash = tls::transcript_hash(transcript_);
            auto content = tls::CertificateVerify::build_signed_content(!is_client_, hash);

            // Sign with Ed25519
            std::vector<uint8_t> content_std(content.begin(), content.end());
            std::vector<uint8_t> key_std(config_.private_key.begin(), config_.private_key.end());

            keylock::crypto::Context crypto(keylock::crypto::Context::Algorithm::Ed25519);
            auto sign_result = crypto.sign(content_std, key_std);

            if (!sign_result.success) {
                return dp::result::err(dp::Error::io_error("Ed25519 signing failed"));
            }

            tls::CertificateVerify cv;
            cv.algorithm = tls::SignatureScheme::Ed25519;
            cv.signature = dp::Vector<dp::u8>(sign_result.data.begin(), sign_result.data.end());

            return dp::result::ok(cv.serialize());
        }

        // Verify CertificateVerify signature
        dp::Res<void> verify_certificate_verify(const tls::CertificateVerify &cv) {
            if (cv.algorithm != tls::SignatureScheme::Ed25519) {
                return dp::result::err(dp::Error::invalid_argument("unsupported signature algorithm"));
            }

            if (peer_certificate_.certificate_list.empty()) {
                return dp::result::err(dp::Error::invalid_argument("no peer certificate"));
            }

            // Parse the peer's certificate to extract the public key
            auto &cert_der = peer_certificate_.certificate_list[0].cert_data;
            std::vector<uint8_t> cert_der_std(cert_der.begin(), cert_der.end());

            auto parse_result = keylock::cert::Certificate::parse(cert_der_std);
            if (!parse_result.success) {
                return dp::result::err(dp::Error::invalid_argument("failed to parse peer certificate"));
            }

            auto &cert = parse_result.value;

            // Extract the public key from the certificate
            auto public_key_der = cert.public_key_der();
            if (public_key_der.empty()) {
                return dp::result::err(dp::Error::invalid_argument("failed to extract public key from certificate"));
            }

            // For Ed25519, extract raw key (last 32 bytes for simple case)
            dp::Vector<dp::u8> public_key;
            if (public_key_der.size() >= 32) {
                public_key = dp::Vector<dp::u8>(public_key_der.end() - 32, public_key_der.end());
            } else {
                return dp::result::err(dp::Error::invalid_argument("public key too short for Ed25519"));
            }

            // Build the content that was signed
            auto hash = tls::transcript_hash(transcript_);
            auto signed_content = tls::CertificateVerify::build_signed_content(true, hash); // Server signed

            // Verify the signature using Ed25519
            std::vector<uint8_t> content_std(signed_content.begin(), signed_content.end());
            std::vector<uint8_t> signature_std(cv.signature.begin(), cv.signature.end());
            std::vector<uint8_t> pubkey_std(public_key.begin(), public_key.end());

            keylock::crypto::Context crypto(keylock::crypto::Context::Algorithm::Ed25519);
            auto verify_result = crypto.verify(content_std, signature_std, pubkey_std);

            if (!verify_result.success || verify_result.data.empty() || verify_result.data[0] != 1) {
                return dp::result::err(dp::Error::invalid_argument("signature verification failed"));
            }

            echo::debug("CertificateVerify signature verified");
            return dp::result::ok();
        }

        // Verify server Finished message
        dp::Res<void> verify_server_finished(const tls::Finished &fin) {
            auto finished_key = tls::KeySchedule::derive_finished_key(key_schedule_.server_handshake_traffic_secret);
            auto hash = tls::transcript_hash(transcript_);
            auto expected_verify_data = tls::Finished::compute_verify_data(finished_key, hash);

            if (fin.verify_data != expected_verify_data) {
                return dp::result::err(dp::Error::invalid_argument("server Finished verify_data mismatch"));
            }

            return dp::result::ok();
        }

        // Derive early data keys from client_early_traffic_secret
        void derive_early_data_keys() {
            if (key_schedule_.client_early_traffic_secret.empty()) {
                echo::warn("Cannot derive early data keys: no early traffic secret");
                return;
            }

            // For 0-RTT, client sends with early data keys (derived from client_early_traffic_secret)
            early_data_keys_ = derive_quic_keys(key_schedule_.client_early_traffic_secret);

            echo::debug("Derived QUIC 0-RTT keys");
        }

        // Serialize pre_shared_key extension for ClientHello
        tls::Extension serialize_psk_extension() {
            tls::Extension ext;
            ext.type = tls::ExtensionType::PreSharedKey;

            dp::Vector<dp::u8> data;

            // identities (list of PSK identities)
            // Each identity: opaque identity<1..2^16-1> + uint32 obfuscated_ticket_age
            dp::Vector<dp::u8> identities;

            // Add our session ticket as identity
            const auto &ticket = config_.session_ticket->ticket;
            dp::u16 identity_len = static_cast<dp::u16>(ticket.size());
            identities.push_back(static_cast<dp::u8>((identity_len >> 8) & 0xFF));
            identities.push_back(static_cast<dp::u8>(identity_len & 0xFF));
            identities.insert(identities.end(), ticket.begin(), ticket.end());

            // obfuscated_ticket_age
            dp::u32 age = config_.session_ticket->obfuscated_ticket_age();
            identities.push_back(static_cast<dp::u8>((age >> 24) & 0xFF));
            identities.push_back(static_cast<dp::u8>((age >> 16) & 0xFF));
            identities.push_back(static_cast<dp::u8>((age >> 8) & 0xFF));
            identities.push_back(static_cast<dp::u8>(age & 0xFF));

            // identities length (2 bytes)
            dp::u16 identities_len = static_cast<dp::u16>(identities.size());
            data.push_back(static_cast<dp::u8>((identities_len >> 8) & 0xFF));
            data.push_back(static_cast<dp::u8>(identities_len & 0xFF));
            data.insert(data.end(), identities.begin(), identities.end());

            // binders (list of HMAC values)
            // For now, we put a placeholder - actual binder computed after serialization
            // binder_len (2 bytes) + binder_entry_len (1 byte) + binder (32 bytes for SHA-256)
            dp::u16 binders_len = 1 + tls::HASH_LENGTH; // 1 byte length + 32 bytes binder
            data.push_back(static_cast<dp::u8>((binders_len >> 8) & 0xFF));
            data.push_back(static_cast<dp::u8>(binders_len & 0xFF));
            data.push_back(static_cast<dp::u8>(tls::HASH_LENGTH)); // single binder length
            // Placeholder for binder (will be replaced)
            for (dp::usize i = 0; i < tls::HASH_LENGTH; i++) {
                data.push_back(0x00);
            }

            ext.data = std::move(data);
            return ext;
        }

        // Compute PSK binder over partial ClientHello
        dp::Vector<dp::u8> compute_psk_binder(const dp::Vector<dp::u8> &client_hello, const dp::Vector<dp::u8> &psk) {
            // The binder is computed over ClientHello up to (but not including) the binders list
            // binders_len (2) + binder_entry_len (1) + binder (32) = 35 bytes to exclude

            dp::usize binder_offset = client_hello.size() - (2 + 1 + tls::HASH_LENGTH);
            dp::Vector<dp::u8> truncated_ch(client_hello.begin(), client_hello.begin() + binder_offset);

            // Compute transcript hash of truncated ClientHello
            auto transcript_hash = tls::transcript_hash(truncated_ch);

            // Derive binder_key from early_secret
            auto binder_key =
                tls::hkdf_expand_label(key_schedule_.early_secret, "res binder", tls::empty_hash(), tls::HASH_LENGTH);

            // Derive finished_key from binder_key
            auto finished_key = tls::KeySchedule::derive_finished_key(binder_key);

            // Compute binder = HMAC(finished_key, transcript_hash)
            auto binder = tls::Finished::compute_verify_data(finished_key, transcript_hash);

            echo::debug("Computed PSK binder (", binder.size(), " bytes)");
            return binder;
        }

        // Insert computed binder into serialized ClientHello
        void insert_psk_binder(dp::Vector<dp::u8> &client_hello, const dp::Vector<dp::u8> &binder) {
            // Replace last HASH_LENGTH bytes with actual binder
            if (client_hello.size() < tls::HASH_LENGTH) {
                return;
            }

            dp::usize binder_start = client_hello.size() - tls::HASH_LENGTH;
            for (dp::usize i = 0; i < binder.size() && i < tls::HASH_LENGTH; i++) {
                client_hello[binder_start + i] = binder[i];
            }
        }

        // Create ticket data (server-side)
        // In production, this should be encrypted with a server-side key
        dp::Vector<dp::u8> create_ticket_data(const dp::Vector<dp::u8> &ticket_nonce) {
            dp::Vector<dp::u8> data;

            // For simplicity, include: resumption_master_secret (32) + cipher_suite (2)
            // In production, encrypt this with a server-side ticket key
            data.insert(data.end(), key_schedule_.resumption_master_secret.begin(),
                        key_schedule_.resumption_master_secret.end());
            data.push_back(static_cast<dp::u8>((selected_cipher_suite_ >> 8) & 0xFF));
            data.push_back(static_cast<dp::u8>(selected_cipher_suite_ & 0xFF));

            echo::debug("Created ticket data (", data.size(), " bytes)");
            return data;
        }
    };

} // namespace netpipe::quic

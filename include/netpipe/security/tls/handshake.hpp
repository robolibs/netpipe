#pragma once

#include <datapod/datapod.hpp>
#include <echo/echo.hpp>
#include <keylock/cert/certificate.hpp>
#include <keylock/cert/parser.hpp>
#include <keylock/crypto/common.hpp>
#include <keylock/crypto/context.hpp>
#include <netpipe/security/tls/extensions.hpp>
#include <netpipe/security/tls/key_schedule.hpp>
#include <netpipe/security/tls/messages.hpp>
#include <netpipe/security/tls/record.hpp>

#include <keylock/crypto/aead_aes256gcm/aead.hpp>
#include <keylock/crypto/box_seal_x25519/x25519.hpp>
#include <keylock/crypto/rng/randombytes.hpp>

namespace netpipe::tls {

    // Handshake states
    enum class HandshakeState {
        // Initial state
        Start,

        // Client states
        WaitServerHello,
        WaitEncryptedExtensions,
        WaitCertificateOrCertificateRequest,
        WaitCertificate,
        WaitCertificateVerify,
        WaitFinished,

        // Server states
        WaitClientHello,
        WaitClientCertificate,
        WaitClientCertificateVerify,
        WaitClientFinished,

        // Terminal states
        Connected,
        Error
    };

    // Role in the handshake
    enum class Role { Client, Server };

    // Configuration for handshake
    struct HandshakeConfig {
        // Our certificate (DER-encoded) - required for server, optional for client
        dp::Vector<dp::u8> certificate;

        // Our Ed25519 private key (64 bytes seed+public or 32 bytes seed)
        dp::Vector<dp::u8> private_key;

        // Server name (for SNI, client only)
        dp::String server_name;

        // Skip certificate verification (for testing only - INSECURE!)
        bool skip_cert_verification = false;

        // Trusted CA certificates (DER-encoded) - for certificate chain validation
        dp::Vector<dp::Vector<dp::u8>> trusted_cas;

        // Expected server hostname (for certificate hostname validation)
        dp::String expected_hostname;

        // ALPN protocols in preference order (e.g. {"h2", "http/1.1"})
        dp::Vector<dp::String> alpn_protocols;
    };

    // Handshake context - all state for an in-progress handshake
    class Handshake {
      public:
        explicit Handshake(Role role, const HandshakeConfig &config = {})
            : role_(role), config_(config), state_(HandshakeState::Start) {
            echo::trace("Handshake created: role=", role == Role::Client ? "client" : "server");

            // Initialize key schedule
            key_schedule_.init();

            // Generate our X25519 keypair
            generate_x25519_keypair();
        }

        // Get current state
        HandshakeState state() const { return state_; }

        // Check if handshake is complete
        bool is_complete() const { return state_ == HandshakeState::Connected; }

        // Check if in error state
        bool is_error() const { return state_ == HandshakeState::Error; }

        // Selected ALPN protocol (if negotiated)
        dp::Optional<dp::String> selected_alpn_protocol() const { return selected_alpn_protocol_; }

        // Get the record layer (for application data after handshake)
        RecordLayer &record_layer() { return record_; }

        // CLIENT: Create ClientHello message
        dp::Res<dp::Vector<dp::u8>> create_client_hello() {
            if (role_ != Role::Client || state_ != HandshakeState::Start) {
                return dp::result::err(dp::Error::invalid_argument("invalid state for create_client_hello"));
            }

            echo::trace("Creating ClientHello");

            ClientHello ch;
            ch.generate_random();
            client_random_ = dp::Vector<dp::u8>(ch.random.begin(), ch.random.end());

            // Generate a random session ID for compatibility
            auto session_id = keylock::crypto::Common::generate_random_bytes(32);
            ch.legacy_session_id = dp::Vector<dp::u8>(session_id.begin(), session_id.end());

            // Cipher suites - prefer AES-GCM if hardware supports it, otherwise ChaCha20
            if (keylock::crypto::aead_aes256gcm::is_available()) {
                ch.cipher_suites = {TLS_AES_256_GCM_SHA384, TLS_CHACHA20_POLY1305_SHA256};
            } else {
                ch.cipher_suites = {TLS_CHACHA20_POLY1305_SHA256};
            }

            // Build extensions
            // 1. supported_versions (TLS 1.3)
            SupportedVersionsExtension sv_ext;
            sv_ext.versions = {TLS_1_3};
            ch.extensions.push_back(sv_ext.serialize_client());

            // 2. supported_groups (X25519)
            SupportedGroupsExtension sg_ext;
            sg_ext.groups = {NamedGroup::X25519};
            ch.extensions.push_back(sg_ext.serialize());

            // 3. signature_algorithms (Ed25519)
            SignatureAlgorithmsExtension sa_ext;
            sa_ext.algorithms = {SignatureScheme::Ed25519};
            ch.extensions.push_back(sa_ext.serialize());

            // 4. key_share (our X25519 public key)
            KeyShareClientHello ks_ext;
            ks_ext.client_shares.push_back({NamedGroup::X25519, our_x25519_public_});
            ch.extensions.push_back(ks_ext.serialize());

            // 5. ALPN
            if (!config_.alpn_protocols.empty()) {
                AlpnExtension alpn;
                alpn.protocols = config_.alpn_protocols;
                ch.extensions.push_back(alpn.serialize());
            }

            // Serialize and update transcript
            auto ch_bytes = ch.serialize();
            update_transcript(ch_bytes);

            // Wrap in record
            auto record_result = RecordLayer::build_plaintext_record(ContentType::Handshake, ch_bytes);
            if (record_result.is_err()) {
                state_ = HandshakeState::Error;
                return dp::result::err(record_result.error());
            }

            state_ = HandshakeState::WaitServerHello;
            echo::debug("ClientHello created, waiting for ServerHello");

            return dp::result::ok(std::move(record_result.value()));
        }

        // CLIENT: Process ServerHello
        dp::Res<void> process_server_hello(const dp::Vector<dp::u8> &data) {
            if (role_ != Role::Client || state_ != HandshakeState::WaitServerHello) {
                return dp::result::err(dp::Error::invalid_argument("invalid state for process_server_hello"));
            }

            echo::trace("Processing ServerHello");

            // Parse handshake header
            auto header_result = parse_handshake_header(data.data(), data.size());
            if (header_result.is_err()) {
                state_ = HandshakeState::Error;
                return dp::result::err(header_result.error());
            }

            auto [msg_type, msg_length] = header_result.value();
            if (msg_type != HandshakeType::ServerHello) {
                state_ = HandshakeState::Error;
                return dp::result::err(dp::Error::invalid_argument("expected ServerHello"));
            }

            // Parse ServerHello
            auto sh_result = ServerHello::parse(data.data() + HANDSHAKE_HEADER_SIZE, msg_length);
            if (sh_result.is_err()) {
                state_ = HandshakeState::Error;
                return dp::result::err(sh_result.error());
            }

            auto &sh = sh_result.value();
            server_random_ = dp::Vector<dp::u8>(sh.random.begin(), sh.random.end());
            selected_cipher_suite_ = sh.cipher_suite;

            // Find key_share extension
            bool found_key_share = false;
            for (const auto &ext : sh.extensions) {
                if (ext.type == ExtensionType::KeyShare) {
                    auto ks_result = KeyShareServerHello::parse(ext.data);
                    if (ks_result.is_err()) {
                        state_ = HandshakeState::Error;
                        return dp::result::err(ks_result.error());
                    }

                    peer_x25519_public_ = ks_result.value().server_share.key_exchange;
                    found_key_share = true;
                }
            }

            if (!found_key_share) {
                state_ = HandshakeState::Error;
                return dp::result::err(dp::Error::invalid_argument("ServerHello missing key_share"));
            }

            // Update transcript with ServerHello
            update_transcript(data);

            // Compute shared secret
            auto shared_secret = compute_x25519_shared_secret();
            if (shared_secret.empty()) {
                state_ = HandshakeState::Error;
                return dp::result::err(dp::Error::io_error("X25519 key exchange failed"));
            }

            // Derive handshake secrets
            auto hello_hash = transcript_hash(transcript_);
            key_schedule_.derive_handshake_secrets(shared_secret, hello_hash);

            // Configure record layer cipher suite
            record_.set_cipher_suite(selected_cipher_suite_);

            // Set up record layer for encrypted handshake
            record_.set_read_traffic_secret(key_schedule_.server_handshake_traffic_secret);
            record_.set_write_traffic_secret(key_schedule_.client_handshake_traffic_secret);

            state_ = HandshakeState::WaitEncryptedExtensions;
            echo::debug("ServerHello processed, handshake keys derived");

            return dp::result::ok();
        }

        // CLIENT: Process encrypted handshake message
        dp::Res<void> process_encrypted_handshake(const dp::Vector<dp::u8> &record_data) {
            echo::trace("Processing encrypted handshake message");

            // Decrypt the record
            auto decrypt_result = record_.decrypt(record_data);
            if (decrypt_result.is_err()) {
                state_ = HandshakeState::Error;
                return dp::result::err(decrypt_result.error());
            }

            auto [content_type, plaintext] = decrypt_result.value();

            // Handle ChangeCipherSpec (ignored in TLS 1.3)
            if (content_type == ContentType::Invalid || content_type == ContentType::ChangeCipherSpec) {
                return dp::result::ok();
            }

            if (content_type != ContentType::Handshake) {
                state_ = HandshakeState::Error;
                return dp::result::err(dp::Error::invalid_argument("expected handshake content type"));
            }

            // Parse handshake header
            auto header_result = parse_handshake_header(plaintext.data(), plaintext.size());
            if (header_result.is_err()) {
                state_ = HandshakeState::Error;
                return dp::result::err(header_result.error());
            }

            auto [msg_type, msg_length] = header_result.value();
            const dp::u8 *msg_data = plaintext.data() + HANDSHAKE_HEADER_SIZE;

            switch (state_) {
            case HandshakeState::WaitEncryptedExtensions:
                if (msg_type != HandshakeType::EncryptedExtensions) {
                    state_ = HandshakeState::Error;
                    return dp::result::err(dp::Error::invalid_argument("expected EncryptedExtensions"));
                }

                if (msg_length > 0) {
                    auto ee_result = EncryptedExtensions::parse(msg_data, msg_length);
                    if (ee_result.is_err()) {
                        state_ = HandshakeState::Error;
                        return dp::result::err(ee_result.error());
                    }

                    for (const auto &ext : ee_result.value().extensions) {
                        if (ext.type == ExtensionType::ApplicationLayerProtocolNegotiation) {
                            auto alpn_result = AlpnExtension::parse(ext.data);
                            if (alpn_result.is_err()) {
                                state_ = HandshakeState::Error;
                                return dp::result::err(alpn_result.error());
                            }
                            if (alpn_result.value().protocols.size() != 1) {
                                state_ = HandshakeState::Error;
                                return dp::result::err(
                                    dp::Error::invalid_argument("server ALPN must select one protocol"));
                            }

                            selected_alpn_protocol_ = alpn_result.value().protocols[0];
                            bool found = false;
                            for (const auto &offered : config_.alpn_protocols) {
                                if (offered == selected_alpn_protocol_.value()) {
                                    found = true;
                                    break;
                                }
                            }
                            if (!config_.alpn_protocols.empty() && !found) {
                                state_ = HandshakeState::Error;
                                return dp::result::err(
                                    dp::Error::invalid_argument("server selected ALPN protocol not offered by client"));
                            }
                        }
                    }
                }

                update_transcript(plaintext);
                state_ = HandshakeState::WaitCertificateOrCertificateRequest;
                echo::debug("EncryptedExtensions received");
                break;

            case HandshakeState::WaitCertificateOrCertificateRequest:
            case HandshakeState::WaitCertificate:
                if (msg_type == HandshakeType::Certificate) {
                    auto cert_result = Certificate::parse(msg_data, msg_length);
                    if (cert_result.is_err()) {
                        state_ = HandshakeState::Error;
                        return dp::result::err(cert_result.error());
                    }
                    peer_certificate_ = cert_result.value();
                    update_transcript(plaintext);
                    state_ = HandshakeState::WaitCertificateVerify;
                    echo::debug("Certificate received");
                } else if (msg_type == HandshakeType::CertificateRequest) {
                    // Server is requesting client certificate - we'll handle this later
                    update_transcript(plaintext);
                    state_ = HandshakeState::WaitCertificate;
                    echo::debug("CertificateRequest received (client auth required)");
                } else {
                    state_ = HandshakeState::Error;
                    return dp::result::err(dp::Error::invalid_argument("expected Certificate or CertificateRequest"));
                }
                break;

            case HandshakeState::WaitCertificateVerify:
                if (msg_type != HandshakeType::CertificateVerify) {
                    state_ = HandshakeState::Error;
                    return dp::result::err(dp::Error::invalid_argument("expected CertificateVerify"));
                }
                {
                    auto cv_result = CertificateVerify::parse(msg_data, msg_length);
                    if (cv_result.is_err()) {
                        state_ = HandshakeState::Error;
                        return dp::result::err(cv_result.error());
                    }

                    // Verify signature (if not skipping verification)
                    if (!config_.skip_cert_verification) {
                        auto verify_result = verify_certificate_verify(cv_result.value(), true);
                        if (verify_result.is_err()) {
                            state_ = HandshakeState::Error;
                            return dp::result::err(verify_result.error());
                        }
                    }

                    update_transcript(plaintext);
                    state_ = HandshakeState::WaitFinished;
                    echo::debug("CertificateVerify received and verified");
                }
                break;

            case HandshakeState::WaitFinished:
                if (msg_type != HandshakeType::Finished) {
                    state_ = HandshakeState::Error;
                    return dp::result::err(dp::Error::invalid_argument("expected Finished"));
                }
                {
                    auto fin_result = Finished::parse(msg_data, msg_length);
                    if (fin_result.is_err()) {
                        state_ = HandshakeState::Error;
                        return dp::result::err(fin_result.error());
                    }

                    // Verify Finished
                    auto verify_result = verify_server_finished(fin_result.value());
                    if (verify_result.is_err()) {
                        state_ = HandshakeState::Error;
                        return dp::result::err(verify_result.error());
                    }

                    update_transcript(plaintext);

                    // Derive application secrets
                    auto full_hash = transcript_hash(transcript_);
                    key_schedule_.derive_application_secrets(full_hash);

                    // Update record layer for application data
                    // Note: client still sends with handshake keys until it sends Finished
                    server_finished_received_ = true;
                    echo::debug("Server Finished received and verified");
                }
                break;

            default:
                state_ = HandshakeState::Error;
                return dp::result::err(dp::Error::invalid_argument("unexpected handshake state"));
            }

            return dp::result::ok();
        }

        // CLIENT: Create client Finished message
        dp::Res<dp::Vector<dp::u8>> create_client_finished() {
            if (role_ != Role::Client || !server_finished_received_) {
                return dp::result::err(dp::Error::invalid_argument("invalid state for create_client_finished"));
            }

            echo::trace("Creating client Finished");

            // Compute verify_data
            auto finished_key = KeySchedule::derive_finished_key(key_schedule_.client_handshake_traffic_secret);
            auto hash = transcript_hash(transcript_);
            auto verify_data = Finished::compute_verify_data(finished_key, hash);

            Finished fin;
            fin.verify_data = std::move(verify_data);

            auto fin_bytes = fin.serialize();
            update_transcript(fin_bytes);

            // Encrypt and send
            auto record_result = record_.encrypt(ContentType::Handshake, fin_bytes);
            if (record_result.is_err()) {
                state_ = HandshakeState::Error;
                return dp::result::err(record_result.error());
            }

            // Switch to application keys
            record_.set_read_traffic_secret(key_schedule_.server_application_traffic_secret);
            record_.set_write_traffic_secret(key_schedule_.client_application_traffic_secret);

            state_ = HandshakeState::Connected;
            echo::debug("Client Finished sent, handshake complete");

            return dp::result::ok(std::move(record_result.value()));
        }

        // SERVER: Process ClientHello and generate response
        dp::Res<dp::Vector<dp::u8>> process_client_hello_and_respond(const dp::Vector<dp::u8> &data) {
            if (role_ != Role::Server || state_ != HandshakeState::Start) {
                return dp::result::err(dp::Error::invalid_argument("invalid state for process_client_hello"));
            }

            echo::trace("Processing ClientHello");

            // Parse handshake header
            auto header_result = parse_handshake_header(data.data(), data.size());
            if (header_result.is_err()) {
                state_ = HandshakeState::Error;
                return dp::result::err(header_result.error());
            }

            auto [msg_type, msg_length] = header_result.value();
            if (msg_type != HandshakeType::ClientHello) {
                state_ = HandshakeState::Error;
                return dp::result::err(dp::Error::invalid_argument("expected ClientHello"));
            }

            // Parse ClientHello
            auto ch_result = ClientHello::parse(data.data() + HANDSHAKE_HEADER_SIZE, msg_length);
            if (ch_result.is_err()) {
                state_ = HandshakeState::Error;
                return dp::result::err(ch_result.error());
            }

            auto &ch = ch_result.value();
            client_random_ = dp::Vector<dp::u8>(ch.random.begin(), ch.random.end());

            // Find supported cipher suite (prefer AES-GCM if available, then ChaCha20)
            bool found_cipher = false;
            bool aes_available = keylock::crypto::aead_aes256gcm::is_available();

            // First pass: look for AES-GCM if hardware supports it
            if (aes_available) {
                for (auto cs : ch.cipher_suites) {
                    if (cs == TLS_AES_256_GCM_SHA384 || cs == TLS_AES_128_GCM_SHA256) {
                        selected_cipher_suite_ = cs;
                        found_cipher = true;
                        echo::debug("Selected AES-GCM cipher suite");
                        break;
                    }
                }
            }

            // Second pass: look for ChaCha20-Poly1305
            if (!found_cipher) {
                for (auto cs : ch.cipher_suites) {
                    if (cs == TLS_CHACHA20_POLY1305_SHA256) {
                        selected_cipher_suite_ = cs;
                        found_cipher = true;
                        echo::debug("Selected ChaCha20-Poly1305 cipher suite");
                        break;
                    }
                }
            }

            if (!found_cipher) {
                state_ = HandshakeState::Error;
                return dp::result::err(dp::Error::invalid_argument("no supported cipher suite"));
            }

            // Find key_share extension with X25519
            bool found_key_share = false;
            dp::Vector<dp::String> client_alpn_protocols;
            for (const auto &ext : ch.extensions) {
                if (ext.type == ExtensionType::KeyShare) {
                    auto ks_result = KeyShareClientHello::parse(ext.data);
                    if (ks_result.is_err()) {
                        continue;
                    }

                    for (const auto &share : ks_result.value().client_shares) {
                        if (share.group == NamedGroup::X25519) {
                            peer_x25519_public_ = share.key_exchange;
                            found_key_share = true;
                            break;
                        }
                    }
                } else if (ext.type == ExtensionType::ApplicationLayerProtocolNegotiation) {
                    auto alpn_result = AlpnExtension::parse(ext.data);
                    if (alpn_result.is_ok()) {
                        client_alpn_protocols = alpn_result.value().protocols;
                    }
                }
            }

            if (!found_key_share) {
                state_ = HandshakeState::Error;
                return dp::result::err(dp::Error::invalid_argument("ClientHello missing X25519 key_share"));
            }

            // Update transcript with ClientHello
            update_transcript(data);

            // Create ServerHello
            ServerHello sh;
            sh.generate_random();
            server_random_ = dp::Vector<dp::u8>(sh.random.begin(), sh.random.end());
            sh.legacy_session_id_echo = ch.legacy_session_id;
            sh.cipher_suite = selected_cipher_suite_;

            // Add extensions
            sh.extensions.push_back(SupportedVersionsExtension::serialize_server(TLS_1_3));

            KeyShareServerHello ks_ext;
            ks_ext.server_share = {NamedGroup::X25519, our_x25519_public_};
            sh.extensions.push_back(ks_ext.serialize());

            auto sh_bytes = sh.serialize();
            update_transcript(sh_bytes);

            // Compute shared secret and derive handshake keys
            auto shared_secret = compute_x25519_shared_secret();
            if (shared_secret.empty()) {
                state_ = HandshakeState::Error;
                return dp::result::err(dp::Error::io_error("X25519 key exchange failed"));
            }

            auto hello_hash = transcript_hash(transcript_);
            key_schedule_.derive_handshake_secrets(shared_secret, hello_hash);

            // Configure record layer cipher suite
            record_.set_cipher_suite(selected_cipher_suite_);

            // Set up record layer for encrypted messages
            record_.set_read_traffic_secret(key_schedule_.client_handshake_traffic_secret);
            record_.set_write_traffic_secret(key_schedule_.server_handshake_traffic_secret);

            // Build response: ServerHello (plaintext) + encrypted messages
            dp::Vector<dp::u8> response;

            // ServerHello record (plaintext)
            auto sh_record_result = RecordLayer::build_plaintext_record(ContentType::Handshake, sh_bytes);
            if (sh_record_result.is_err()) {
                state_ = HandshakeState::Error;
                return dp::result::err(sh_record_result.error());
            }
            auto sh_record = sh_record_result.value();
            response.insert(response.end(), sh_record.begin(), sh_record.end());

            // ChangeCipherSpec (for middlebox compatibility)
            dp::Vector<dp::u8> ccs = {0x01};
            auto ccs_record_result = RecordLayer::build_plaintext_record(ContentType::ChangeCipherSpec, ccs);
            if (ccs_record_result.is_ok()) {
                auto ccs_record = ccs_record_result.value();
                response.insert(response.end(), ccs_record.begin(), ccs_record.end());
            }

            // EncryptedExtensions (encrypted)
            EncryptedExtensions ee;

            if (!config_.alpn_protocols.empty() && !client_alpn_protocols.empty()) {
                auto selected = AlpnExtension::negotiate(client_alpn_protocols, config_.alpn_protocols);
                if (!selected.has_value()) {
                    state_ = HandshakeState::Error;
                    return dp::result::err(dp::Error::invalid_argument("no shared ALPN protocol"));
                }

                selected_alpn_protocol_ = selected.value();
                AlpnExtension alpn;
                alpn.protocols = {selected.value()};
                ee.extensions.push_back(alpn.serialize());
            }

            auto ee_bytes = ee.serialize();
            update_transcript(ee_bytes);

            auto ee_record_result = record_.encrypt(ContentType::Handshake, ee_bytes);
            if (ee_record_result.is_err()) {
                state_ = HandshakeState::Error;
                return dp::result::err(ee_record_result.error());
            }
            auto ee_record = ee_record_result.value();
            response.insert(response.end(), ee_record.begin(), ee_record.end());

            // Certificate (encrypted)
            Certificate cert;
            CertificateEntry cert_entry;
            cert_entry.cert_data = config_.certificate;
            cert.certificate_list.push_back(std::move(cert_entry));

            auto cert_bytes = cert.serialize();
            update_transcript(cert_bytes);

            auto cert_record_result = record_.encrypt(ContentType::Handshake, cert_bytes);
            if (cert_record_result.is_err()) {
                state_ = HandshakeState::Error;
                return dp::result::err(cert_record_result.error());
            }
            auto cert_record = cert_record_result.value();
            response.insert(response.end(), cert_record.begin(), cert_record.end());

            // CertificateVerify (encrypted)
            auto cv_result = create_certificate_verify(true);
            if (cv_result.is_err()) {
                state_ = HandshakeState::Error;
                return dp::result::err(cv_result.error());
            }
            auto cv_bytes = cv_result.value();
            update_transcript(cv_bytes);

            auto cv_record_result = record_.encrypt(ContentType::Handshake, cv_bytes);
            if (cv_record_result.is_err()) {
                state_ = HandshakeState::Error;
                return dp::result::err(cv_record_result.error());
            }
            auto cv_record = cv_record_result.value();
            response.insert(response.end(), cv_record.begin(), cv_record.end());

            // Finished (encrypted)
            auto finished_key = KeySchedule::derive_finished_key(key_schedule_.server_handshake_traffic_secret);
            auto hash = transcript_hash(transcript_);
            auto verify_data = Finished::compute_verify_data(finished_key, hash);

            Finished fin;
            fin.verify_data = std::move(verify_data);

            auto fin_bytes = fin.serialize();
            update_transcript(fin_bytes);

            // Derive application secrets before sending Finished
            auto full_hash = transcript_hash(transcript_);
            key_schedule_.derive_application_secrets(full_hash);

            auto fin_record_result = record_.encrypt(ContentType::Handshake, fin_bytes);
            if (fin_record_result.is_err()) {
                state_ = HandshakeState::Error;
                return dp::result::err(fin_record_result.error());
            }
            auto fin_record = fin_record_result.value();
            response.insert(response.end(), fin_record.begin(), fin_record.end());

            state_ = HandshakeState::WaitClientFinished;
            echo::debug("ServerHello and server handshake messages sent, waiting for client Finished");

            return dp::result::ok(std::move(response));
        }

        // SERVER: Process client Finished
        dp::Res<void> process_client_finished(const dp::Vector<dp::u8> &record_data) {
            if (role_ != Role::Server || state_ != HandshakeState::WaitClientFinished) {
                return dp::result::err(dp::Error::invalid_argument("invalid state for process_client_finished"));
            }

            echo::trace("Processing client Finished");

            // Decrypt the record
            auto decrypt_result = record_.decrypt(record_data);
            if (decrypt_result.is_err()) {
                state_ = HandshakeState::Error;
                return dp::result::err(decrypt_result.error());
            }

            auto [content_type, plaintext] = decrypt_result.value();

            // Handle ChangeCipherSpec (ignored)
            if (content_type == ContentType::Invalid || content_type == ContentType::ChangeCipherSpec) {
                return dp::result::ok();
            }

            if (content_type != ContentType::Handshake) {
                state_ = HandshakeState::Error;
                return dp::result::err(dp::Error::invalid_argument("expected handshake content type"));
            }

            // Parse handshake header
            auto header_result = parse_handshake_header(plaintext.data(), plaintext.size());
            if (header_result.is_err()) {
                state_ = HandshakeState::Error;
                return dp::result::err(header_result.error());
            }

            auto [msg_type, msg_length] = header_result.value();
            if (msg_type != HandshakeType::Finished) {
                state_ = HandshakeState::Error;
                return dp::result::err(dp::Error::invalid_argument("expected Finished"));
            }

            // Parse Finished
            auto fin_result = Finished::parse(plaintext.data() + HANDSHAKE_HEADER_SIZE, msg_length);
            if (fin_result.is_err()) {
                state_ = HandshakeState::Error;
                return dp::result::err(fin_result.error());
            }

            // Verify client Finished
            auto finished_key = KeySchedule::derive_finished_key(key_schedule_.client_handshake_traffic_secret);
            auto hash = transcript_hash(transcript_);
            auto expected_verify_data = Finished::compute_verify_data(finished_key, hash);

            if (fin_result.value().verify_data != expected_verify_data) {
                state_ = HandshakeState::Error;
                return dp::result::err(dp::Error::invalid_argument("client Finished verify_data mismatch"));
            }

            // Switch to application keys
            record_.set_read_traffic_secret(key_schedule_.client_application_traffic_secret);
            record_.set_write_traffic_secret(key_schedule_.server_application_traffic_secret);

            state_ = HandshakeState::Connected;
            echo::debug("Client Finished verified, handshake complete");

            return dp::result::ok();
        }

      private:
        Role role_;
        HandshakeConfig config_;
        HandshakeState state_;

        // Key schedule
        KeySchedule key_schedule_;

        // Record layer
        RecordLayer record_;

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
        Certificate peer_certificate_;

        // Flags
        bool server_finished_received_ = false;
        dp::Optional<dp::String> selected_alpn_protocol_;

        // Generate X25519 keypair
        void generate_x25519_keypair() {
            our_x25519_public_.resize(keylock::crypto::x25519::PUBLICKEYBYTES);
            our_x25519_private_.resize(keylock::crypto::x25519::SECRETKEYBYTES);

            keylock::crypto::rng::randombytes_buf(our_x25519_private_.data(), our_x25519_private_.size());
            keylock::crypto::x25519::public_key(our_x25519_public_.data(), our_x25519_private_.data());

            echo::debug("Generated X25519 keypair");
        }

        // Compute X25519 shared secret
        dp::Vector<dp::u8> compute_x25519_shared_secret() {
            if (peer_x25519_public_.size() != keylock::crypto::x25519::PUBLICKEYBYTES) {
                echo::error("Invalid peer X25519 public key size");
                return {};
            }

            dp::Vector<dp::u8> shared_secret(keylock::crypto::x25519::PUBLICKEYBYTES);

            keylock::crypto::x25519::scalarmult(shared_secret.data(), our_x25519_private_.data(),
                                                peer_x25519_public_.data());

            echo::debug("Computed X25519 shared secret");
            return shared_secret;
        }

        // Update transcript with a handshake message
        void update_transcript(const dp::Vector<dp::u8> &message) {
            transcript_.insert(transcript_.end(), message.begin(), message.end());
            echo::trace("Transcript updated, total size=", transcript_.size());
        }

        // Create CertificateVerify message
        dp::Res<dp::Vector<dp::u8>> create_certificate_verify(bool is_server) {
            if (config_.private_key.empty()) {
                return dp::result::err(dp::Error::invalid_argument("private key not configured"));
            }

            // Build content to sign
            auto hash = transcript_hash(transcript_);
            auto content = CertificateVerify::build_signed_content(is_server, hash);

            // Sign with Ed25519
            std::vector<uint8_t> content_std(content.begin(), content.end());
            std::vector<uint8_t> key_std(config_.private_key.begin(), config_.private_key.end());

            keylock::crypto::Context crypto(keylock::crypto::Context::Algorithm::Ed25519);
            auto sign_result = crypto.sign(content_std, key_std);

            if (!sign_result.success) {
                return dp::result::err(dp::Error::io_error("Ed25519 signing failed"));
            }

            CertificateVerify cv;
            cv.algorithm = SignatureScheme::Ed25519;
            cv.signature = dp::Vector<dp::u8>(sign_result.data.begin(), sign_result.data.end());

            return dp::result::ok(cv.serialize());
        }

        // Verify CertificateVerify signature
        dp::Res<void> verify_certificate_verify(const CertificateVerify &cv, bool is_server) {
            if (cv.algorithm != SignatureScheme::Ed25519) {
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
                echo::error("Failed to parse peer certificate: ", parse_result.error.c_str());
                return dp::result::err(dp::Error::invalid_argument("failed to parse peer certificate"));
            }

            auto &cert = parse_result.value;

            // Validate certificate time if not skipping verification
            if (!config_.skip_cert_verification && !cert.check_validity()) {
                echo::error("Peer certificate is expired or not yet valid");
                return dp::result::err(dp::Error::invalid_argument("peer certificate validity check failed"));
            }

            // Validate hostname if configured (for client verifying server)
            if (!config_.skip_cert_verification && !config_.expected_hostname.empty() && is_server) {
                std::string hostname(config_.expected_hostname.c_str());
                if (!cert.match_hostname(hostname)) {
                    echo::error("Certificate hostname mismatch: expected ", hostname.c_str());
                    return dp::result::err(dp::Error::invalid_argument("certificate hostname mismatch"));
                }
            }

            // Extract the public key from the certificate
            auto public_key_der = cert.public_key_der();
            if (public_key_der.empty()) {
                return dp::result::err(dp::Error::invalid_argument("failed to extract public key from certificate"));
            }

            // For Ed25519, the public key is 32 bytes
            // The DER format includes algorithm identifier, so we need to extract the raw key
            // Ed25519 public key in SubjectPublicKeyInfo is: SEQUENCE { algorithm, BIT STRING { key } }
            // The raw key should be the last 32 bytes if it's Ed25519
            dp::Vector<dp::u8> public_key;
            if (public_key_der.size() >= 32) {
                // Try to extract Ed25519 public key (last 32 bytes for simple case)
                // For proper parsing, we'd need full ASN.1 parsing
                public_key = dp::Vector<dp::u8>(public_key_der.end() - 32, public_key_der.end());
            } else {
                return dp::result::err(dp::Error::invalid_argument("public key too short for Ed25519"));
            }

            // Build the content that was signed
            auto hash = transcript_hash(transcript_);
            auto signed_content = CertificateVerify::build_signed_content(is_server, hash);

            // Verify the signature using Ed25519
            std::vector<uint8_t> content_std(signed_content.begin(), signed_content.end());
            std::vector<uint8_t> signature_std(cv.signature.begin(), cv.signature.end());
            std::vector<uint8_t> pubkey_std(public_key.begin(), public_key.end());

            keylock::crypto::Context crypto(keylock::crypto::Context::Algorithm::Ed25519);
            auto verify_result = crypto.verify(content_std, signature_std, pubkey_std);

            if (!verify_result.success) {
                echo::error("CertificateVerify signature verification failed");
                return dp::result::err(dp::Error::invalid_argument("signature verification failed"));
            }

            // Check if verification passed (verify returns data with success=true if valid)
            // The verify function returns success=true and data[0]=1 if signature is valid
            if (verify_result.data.empty() || verify_result.data[0] != 1) {
                echo::error("CertificateVerify signature is invalid");
                return dp::result::err(dp::Error::invalid_argument("invalid signature"));
            }

            echo::info("CertificateVerify signature verified successfully");
            return dp::result::ok();
        }

        // Verify server Finished message
        dp::Res<void> verify_server_finished(const Finished &fin) {
            auto finished_key = KeySchedule::derive_finished_key(key_schedule_.server_handshake_traffic_secret);
            auto hash = transcript_hash(transcript_);
            auto expected_verify_data = Finished::compute_verify_data(finished_key, hash);

            if (fin.verify_data != expected_verify_data) {
                return dp::result::err(dp::Error::invalid_argument("server Finished verify_data mismatch"));
            }

            return dp::result::ok();
        }
    };

} // namespace netpipe::tls

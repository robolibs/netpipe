#pragma once

#include <datapod/datapod.hpp>
#include <echo/echo.hpp>
#include <keylock/hash/algorithms.hpp>
#include <keylock/utils/common.hpp>

namespace netpipe::tls {

    // TLS 1.3 Hash length for SHA-256
    constexpr dp::usize HASH_LENGTH = 32;

    // HKDF-Expand-Label as defined in RFC 8446 Section 7.1
    // Output = HKDF-Expand(Secret, HkdfLabel, Length)
    // struct HkdfLabel {
    //    uint16 length;
    //    opaque label<7..255> = "tls13 " + Label;
    //    opaque context<0..255> = Context;
    // }
    inline dp::Vector<dp::u8> hkdf_expand_label(const dp::Vector<dp::u8> &secret, const dp::String &label,
                                                const dp::Vector<dp::u8> &context, dp::u16 length) {
        echo::trace("hkdf_expand_label: label=", label.c_str(), " length=", length);

        // Build HkdfLabel structure
        dp::Vector<dp::u8> hkdf_label;

        // Length (2 bytes, big-endian)
        hkdf_label.push_back(static_cast<dp::u8>((length >> 8) & 0xFF));
        hkdf_label.push_back(static_cast<dp::u8>(length & 0xFF));

        // Label with "tls13 " prefix
        dp::String full_label = "tls13 " + label;
        hkdf_label.push_back(static_cast<dp::u8>(full_label.size()));
        for (char c : full_label) {
            hkdf_label.push_back(static_cast<dp::u8>(c));
        }

        // Context
        hkdf_label.push_back(static_cast<dp::u8>(context.size()));
        hkdf_label.insert(hkdf_label.end(), context.begin(), context.end());

        // Convert to std::vector for keylock
        std::vector<uint8_t> secret_std(secret.begin(), secret.end());
        std::vector<uint8_t> info_std(hkdf_label.begin(), hkdf_label.end());

        // Use HKDF-Expand from keylock
        auto result = keylock::hash::hkdf_expand(keylock::hash::Algorithm::SHA256, secret_std, info_std, length);

        if (!result.success) {
            echo::error("hkdf_expand_label failed: ", result.error_message.c_str());
            return {};
        }

        return dp::Vector<dp::u8>(result.data.begin(), result.data.end());
    }

    // Derive-Secret as defined in RFC 8446 Section 7.1
    // Derive-Secret(Secret, Label, Messages) = HKDF-Expand-Label(Secret, Label, Hash(Messages), Hash.length)
    inline dp::Vector<dp::u8> derive_secret(const dp::Vector<dp::u8> &secret, const dp::String &label,
                                            const dp::Vector<dp::u8> &transcript_hash) {
        echo::trace("derive_secret: label=", label.c_str());
        return hkdf_expand_label(secret, label, transcript_hash, HASH_LENGTH);
    }

    // Compute transcript hash (SHA-256 of handshake messages)
    inline dp::Vector<dp::u8> transcript_hash(const dp::Vector<dp::u8> &messages) {
        std::vector<uint8_t> messages_std(messages.begin(), messages.end());
        auto result = keylock::hash::digest(keylock::hash::Algorithm::SHA256, messages_std);

        if (!result.success) {
            echo::error("transcript_hash failed: ", result.error_message.c_str());
            return {};
        }

        return dp::Vector<dp::u8>(result.data.begin(), result.data.end());
    }

    // Empty hash - Hash("") for certain derivations
    inline dp::Vector<dp::u8> empty_hash() { return transcript_hash({}); }

    // TLS 1.3 Key Schedule
    // Manages all the secret derivation for a TLS 1.3 connection
    struct KeySchedule {
        // Secrets at each stage
        dp::Vector<dp::u8> early_secret;
        dp::Vector<dp::u8> handshake_secret;
        dp::Vector<dp::u8> master_secret;

        // Traffic secrets
        dp::Vector<dp::u8> client_handshake_traffic_secret;
        dp::Vector<dp::u8> server_handshake_traffic_secret;
        dp::Vector<dp::u8> client_application_traffic_secret;
        dp::Vector<dp::u8> server_application_traffic_secret;

        // 0-RTT / Early data secrets
        dp::Vector<dp::u8> client_early_traffic_secret;
        dp::Vector<dp::u8> early_exporter_master_secret;

        // Resumption secrets
        dp::Vector<dp::u8> resumption_master_secret;

        // Initialize with early secret (no PSK = zeros)
        void init() {
            echo::trace("KeySchedule::init");

            // Without PSK, IKM is zeros of hash length
            std::vector<uint8_t> zeros(HASH_LENGTH, 0);
            std::vector<uint8_t> empty_salt;

            // Early Secret = HKDF-Extract(0, 0)
            auto result = keylock::hash::hkdf_extract(keylock::hash::Algorithm::SHA256, zeros, empty_salt);

            if (!result.success) {
                echo::error("Failed to derive early secret: ", result.error_message.c_str());
                return;
            }

            early_secret = dp::Vector<dp::u8>(result.data.begin(), result.data.end());
            echo::debug("Derived early_secret (", early_secret.size(), " bytes)");
        }

        // Initialize with a Pre-Shared Key (for session resumption / 0-RTT)
        void init_with_psk(const dp::Vector<dp::u8> &psk) {
            echo::trace("KeySchedule::init_with_psk");

            std::vector<uint8_t> psk_std(psk.begin(), psk.end());
            std::vector<uint8_t> empty_salt;

            // Early Secret = HKDF-Extract(0, PSK)
            auto result = keylock::hash::hkdf_extract(keylock::hash::Algorithm::SHA256, psk_std, empty_salt);

            if (!result.success) {
                echo::error("Failed to derive early secret with PSK: ", result.error_message.c_str());
                return;
            }

            early_secret = dp::Vector<dp::u8>(result.data.begin(), result.data.end());
            echo::debug("Derived early_secret from PSK (", early_secret.size(), " bytes)");
        }

        // Derive early (0-RTT) secrets from ClientHello
        // transcript_hash should be Hash(ClientHello)
        void derive_early_secrets(const dp::Vector<dp::u8> &client_hello_hash) {
            echo::trace("KeySchedule::derive_early_secrets");

            // Client Early Traffic Secret
            client_early_traffic_secret = derive_secret(early_secret, "c e traffic", client_hello_hash);
            echo::debug("Derived client_early_traffic_secret (", client_early_traffic_secret.size(), " bytes)");

            // Early Exporter Master Secret
            early_exporter_master_secret = derive_secret(early_secret, "e exp master", client_hello_hash);
            echo::debug("Derived early_exporter_master_secret (", early_exporter_master_secret.size(), " bytes)");
        }

        // Derive handshake secrets from the ECDHE shared secret
        // transcript_hash should be Hash(ClientHello || ServerHello)
        void derive_handshake_secrets(const dp::Vector<dp::u8> &shared_secret,
                                      const dp::Vector<dp::u8> &hello_transcript_hash) {
            echo::trace("KeySchedule::derive_handshake_secrets");

            // First, derive the salt: Derive-Secret(early_secret, "derived", "")
            auto derived = derive_secret(early_secret, "derived", empty_hash());

            // Handshake Secret = HKDF-Extract(derived, shared_secret)
            std::vector<uint8_t> derived_std(derived.begin(), derived.end());
            std::vector<uint8_t> shared_std(shared_secret.begin(), shared_secret.end());

            auto result = keylock::hash::hkdf_extract(keylock::hash::Algorithm::SHA256, shared_std, derived_std);

            if (!result.success) {
                echo::error("Failed to derive handshake secret: ", result.error_message.c_str());
                return;
            }

            handshake_secret = dp::Vector<dp::u8>(result.data.begin(), result.data.end());
            echo::debug("Derived handshake_secret (", handshake_secret.size(), " bytes)");

            // Client Handshake Traffic Secret
            client_handshake_traffic_secret = derive_secret(handshake_secret, "c hs traffic", hello_transcript_hash);
            echo::debug("Derived client_handshake_traffic_secret (", client_handshake_traffic_secret.size(), " bytes)");

            // Server Handshake Traffic Secret
            server_handshake_traffic_secret = derive_secret(handshake_secret, "s hs traffic", hello_transcript_hash);
            echo::debug("Derived server_handshake_traffic_secret (", server_handshake_traffic_secret.size(), " bytes)");
        }

        // Derive application secrets
        // transcript_hash should be Hash(ClientHello ... server Finished)
        void derive_application_secrets(const dp::Vector<dp::u8> &full_transcript_hash) {
            echo::trace("KeySchedule::derive_application_secrets");

            // Salt: Derive-Secret(handshake_secret, "derived", "")
            auto derived = derive_secret(handshake_secret, "derived", empty_hash());

            // Master Secret = HKDF-Extract(derived, 0)
            std::vector<uint8_t> derived_std(derived.begin(), derived.end());
            std::vector<uint8_t> zeros(HASH_LENGTH, 0);

            auto result = keylock::hash::hkdf_extract(keylock::hash::Algorithm::SHA256, zeros, derived_std);

            if (!result.success) {
                echo::error("Failed to derive master secret: ", result.error_message.c_str());
                return;
            }

            master_secret = dp::Vector<dp::u8>(result.data.begin(), result.data.end());
            echo::debug("Derived master_secret (", master_secret.size(), " bytes)");

            // Client Application Traffic Secret
            client_application_traffic_secret = derive_secret(master_secret, "c ap traffic", full_transcript_hash);
            echo::debug("Derived client_application_traffic_secret (", client_application_traffic_secret.size(),
                        " bytes)");

            // Server Application Traffic Secret
            server_application_traffic_secret = derive_secret(master_secret, "s ap traffic", full_transcript_hash);
            echo::debug("Derived server_application_traffic_secret (", server_application_traffic_secret.size(),
                        " bytes)");
        }

        // Derive resumption master secret (for generating session tickets)
        // transcript_hash should be Hash(ClientHello ... client Finished)
        void derive_resumption_master_secret(const dp::Vector<dp::u8> &full_transcript_hash) {
            echo::trace("KeySchedule::derive_resumption_master_secret");

            resumption_master_secret = derive_secret(master_secret, "res master", full_transcript_hash);
            echo::debug("Derived resumption_master_secret (", resumption_master_secret.size(), " bytes)");
        }

        // Derive PSK from resumption master secret and ticket nonce
        // Used to create the PSK for session resumption
        static dp::Vector<dp::u8> derive_resumption_psk(const dp::Vector<dp::u8> &resumption_master_secret,
                                                        const dp::Vector<dp::u8> &ticket_nonce) {
            echo::trace("KeySchedule::derive_resumption_psk");
            return hkdf_expand_label(resumption_master_secret, "resumption", ticket_nonce, HASH_LENGTH);
        }

        // Derive traffic keys and IV from a traffic secret
        // Returns {key, iv}
        static std::pair<dp::Vector<dp::u8>, dp::Vector<dp::u8>>
        derive_traffic_keys(const dp::Vector<dp::u8> &traffic_secret, dp::usize key_length = 32,
                            dp::usize iv_length = 12) {
            echo::trace("KeySchedule::derive_traffic_keys");

            auto key = hkdf_expand_label(traffic_secret, "key", {}, static_cast<dp::u16>(key_length));
            auto iv = hkdf_expand_label(traffic_secret, "iv", {}, static_cast<dp::u16>(iv_length));

            return {key, iv};
        }

        // Derive the finished key from a traffic secret
        static dp::Vector<dp::u8> derive_finished_key(const dp::Vector<dp::u8> &traffic_secret) {
            echo::trace("KeySchedule::derive_finished_key");
            return hkdf_expand_label(traffic_secret, "finished", {}, HASH_LENGTH);
        }
    };

} // namespace netpipe::tls

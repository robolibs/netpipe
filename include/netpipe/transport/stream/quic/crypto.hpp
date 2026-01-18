#pragma once

#include <datapod/datapod.hpp>
#include <echo/echo.hpp>
#include <keylock/crypto/aead_chacha20poly1305_ietf/aead.hpp>
#include <keylock/crypto/chacha20/chacha20.hpp>
#include <keylock/crypto/common.hpp>
#include <keylock/hash/hmac/hmac_sha256.hpp>
#include <netpipe/transport/stream/quic/packet.hpp>
#include <netpipe/transport/stream/quic/types.hpp>

namespace netpipe::quic {

    // QUIC uses TLS 1.3 cipher suites
    // TLS_AES_128_GCM_SHA256 (0x1301) - most common
    // TLS_AES_256_GCM_SHA384 (0x1302)
    // TLS_CHACHA20_POLY1305_SHA256 (0x1303)

    // Key sizes
    constexpr dp::usize AEAD_KEY_LENGTH_128 = 16;
    constexpr dp::usize AEAD_KEY_LENGTH_256 = 32;
    constexpr dp::usize AEAD_IV_LENGTH = 12;
    constexpr dp::usize AEAD_TAG_LENGTH = 16;
    constexpr dp::usize HEADER_PROTECTION_KEY_LENGTH = 16;
    constexpr dp::usize HEADER_PROTECTION_SAMPLE_LENGTH = 16;

    // HKDF labels for QUIC (RFC 9001 Section 5.1)
    constexpr const char *QUIC_LABEL_KEY = "quic key";
    constexpr const char *QUIC_LABEL_IV = "quic iv";
    constexpr const char *QUIC_LABEL_HP = "quic hp";

    // Initial salt for QUIC version 1 (RFC 9001 Section 5.2)
    // This is used to derive initial keys from the Destination Connection ID
    constexpr dp::u8 QUIC_V1_INITIAL_SALT[] = {0x38, 0x76, 0x2c, 0xf7, 0xf5, 0x59, 0x34, 0xb3, 0x4d, 0x17,
                                               0x9a, 0xe6, 0xa4, 0xc8, 0x0c, 0xad, 0xcc, 0xbb, 0x7f, 0x0a};
    constexpr dp::usize QUIC_V1_INITIAL_SALT_LENGTH = 20;

    // QUIC version 2 uses a different salt
    constexpr dp::u8 QUIC_V2_INITIAL_SALT[] = {0x0d, 0xed, 0xe3, 0xde, 0xf7, 0x00, 0xa6, 0xdb, 0x81, 0x93,
                                               0x81, 0xbe, 0x6e, 0x26, 0x9d, 0xcb, 0xf9, 0xbd, 0x2e, 0xd9};
    constexpr dp::usize QUIC_V2_INITIAL_SALT_LENGTH = 20;

    // HKDF-Extract (uses HMAC-SHA256)
    inline dp::Vector<dp::u8> hkdf_extract(const dp::u8 *salt, dp::usize salt_len, const dp::u8 *ikm,
                                           dp::usize ikm_len) {
        dp::Vector<dp::u8> prk(keylock::hash::hmac_sha256::BYTES);

        keylock::hash::hmac_sha256::Context state;
        keylock::hash::hmac_sha256::init(&state, salt, salt_len);
        keylock::hash::hmac_sha256::update(&state, ikm, ikm_len);
        keylock::hash::hmac_sha256::final(&state, prk.data());

        return prk;
    }

    // HKDF-Expand-Label (TLS 1.3 style, RFC 8446 Section 7.1)
    inline dp::Vector<dp::u8> hkdf_expand_label(const dp::Vector<dp::u8> &secret, const char *label,
                                                const dp::u8 *context, dp::usize context_len, dp::usize length) {
        // Build HkdfLabel structure:
        // uint16 length;
        // opaque label<7..255> = "tls13 " + Label;
        // opaque context<0..255>;

        dp::String full_label = dp::String("tls13 ") + label;

        dp::Vector<dp::u8> hkdf_label;
        // Length (2 bytes, big-endian)
        hkdf_label.push_back(static_cast<dp::u8>((length >> 8) & 0xFF));
        hkdf_label.push_back(static_cast<dp::u8>(length & 0xFF));

        // Label length + label
        hkdf_label.push_back(static_cast<dp::u8>(full_label.size()));
        hkdf_label.insert(hkdf_label.end(), full_label.begin(), full_label.end());

        // Context length + context
        hkdf_label.push_back(static_cast<dp::u8>(context_len));
        if (context_len > 0 && context != nullptr) {
            hkdf_label.insert(hkdf_label.end(), context, context + context_len);
        }

        // HKDF-Expand using HMAC-SHA256
        dp::Vector<dp::u8> okm;
        okm.reserve(length);

        dp::Vector<dp::u8> t;
        dp::u8 counter = 1;

        while (okm.size() < length) {
            keylock::hash::hmac_sha256::Context state;
            keylock::hash::hmac_sha256::init(&state, secret.data(), secret.size());

            if (!t.empty()) {
                keylock::hash::hmac_sha256::update(&state, t.data(), t.size());
            }
            keylock::hash::hmac_sha256::update(&state, hkdf_label.data(), hkdf_label.size());
            keylock::hash::hmac_sha256::update(&state, &counter, 1);

            t.resize(keylock::hash::hmac_sha256::BYTES);
            keylock::hash::hmac_sha256::final(&state, t.data());

            dp::usize copy_len = std::min(t.size(), length - okm.size());
            okm.insert(okm.end(), t.begin(), t.begin() + copy_len);
            counter++;
        }

        return okm;
    }

    // Keys for a specific encryption level
    struct QuicKeys {
        dp::Vector<dp::u8> key;    // AEAD key
        dp::Vector<dp::u8> iv;     // AEAD IV (nonce base)
        dp::Vector<dp::u8> hp_key; // Header protection key

        bool is_valid() const { return !key.empty() && !iv.empty() && !hp_key.empty(); }
    };

    // Derive QUIC keys from a secret
    inline QuicKeys derive_keys(const dp::Vector<dp::u8> &secret, dp::usize key_length = AEAD_KEY_LENGTH_128) {
        QuicKeys keys;

        keys.key = hkdf_expand_label(secret, QUIC_LABEL_KEY, nullptr, 0, key_length);
        keys.iv = hkdf_expand_label(secret, QUIC_LABEL_IV, nullptr, 0, AEAD_IV_LENGTH);
        keys.hp_key = hkdf_expand_label(secret, QUIC_LABEL_HP, nullptr, 0, HEADER_PROTECTION_KEY_LENGTH);

        return keys;
    }

    // Derive initial secrets from Destination Connection ID
    inline std::pair<dp::Vector<dp::u8>, dp::Vector<dp::u8>> derive_initial_secrets(const ConnectionId &dcid,
                                                                                    dp::u32 version = QUIC_VERSION_1) {
        // Select salt based on version
        const dp::u8 *salt;
        dp::usize salt_len;
        if (version == QUIC_VERSION_2) {
            salt = QUIC_V2_INITIAL_SALT;
            salt_len = QUIC_V2_INITIAL_SALT_LENGTH;
        } else {
            salt = QUIC_V1_INITIAL_SALT;
            salt_len = QUIC_V1_INITIAL_SALT_LENGTH;
        }

        // Initial secret = HKDF-Extract(salt, DCID)
        auto initial_secret = hkdf_extract(salt, salt_len, dcid.bytes(), dcid.size());

        // Client initial secret
        auto client_secret = hkdf_expand_label(initial_secret, "client in", nullptr, 0, 32);

        // Server initial secret
        auto server_secret = hkdf_expand_label(initial_secret, "server in", nullptr, 0, 32);

        return {client_secret, server_secret};
    }

    // Key Update label (RFC 9001 Section 6.1)
    constexpr const char *QUIC_LABEL_KU = "quic ku";

    // Derive key update secret from current application traffic secret
    // RFC 9001 Section 6.1:
    // application_traffic_secret_N+1 =
    //     HKDF-Expand-Label(application_traffic_secret_N, "quic ku", "", Hash.length)
    inline dp::Vector<dp::u8> derive_key_update_secret(const dp::Vector<dp::u8> &current_secret) {
        return hkdf_expand_label(current_secret, QUIC_LABEL_KU, nullptr, 0, 32);
    }

    // Key phase state for managing key updates
    struct KeyPhaseState {
        bool current_phase = false;            // Current key phase bit
        dp::u64 current_phase_pn = 0;          // First PN sent with current phase
        dp::u64 key_update_count = 0;          // Number of key updates performed
        dp::u64 last_key_update_pn = 0;        // PN of last key update
        bool pending_key_update = false;       // Key update requested but not yet sent
        bool peer_key_update_received = false; // Received peer key update, need to respond

        // Secrets for each phase
        dp::Vector<dp::u8> read_secret;       // Current read (decryption) secret
        dp::Vector<dp::u8> write_secret;      // Current write (encryption) secret
        dp::Vector<dp::u8> next_read_secret;  // Next phase read secret (pre-computed)
        dp::Vector<dp::u8> next_write_secret; // Next phase write secret (pre-computed)

        // Keys derived from secrets
        QuicKeys read_keys;       // Current read keys
        QuicKeys write_keys;      // Current write keys
        QuicKeys next_read_keys;  // Next phase read keys
        QuicKeys next_write_keys; // Next phase write keys

        // Previous keys (kept briefly for out-of-order packets)
        QuicKeys prev_read_keys;
        dp::u64 prev_phase_end_pn = 0; // Last PN of previous phase

        // Initialize with application secrets from TLS handshake
        void initialize(const dp::Vector<dp::u8> &client_secret, const dp::Vector<dp::u8> &server_secret,
                        bool is_client) {
            if (is_client) {
                write_secret = client_secret;
                read_secret = server_secret;
            } else {
                write_secret = server_secret;
                read_secret = client_secret;
            }

            // Derive current keys
            write_keys = derive_keys(write_secret);
            read_keys = derive_keys(read_secret);

            // Pre-compute next phase secrets and keys
            next_write_secret = derive_key_update_secret(write_secret);
            next_read_secret = derive_key_update_secret(read_secret);
            next_write_keys = derive_keys(next_write_secret);
            next_read_keys = derive_keys(next_read_secret);

            current_phase = false;
            key_update_count = 0;

            echo::debug("Key phase state initialized");
        }

        // Initiate a key update (to be called by local endpoint)
        bool initiate_key_update(dp::u64 current_pn) {
            if (pending_key_update) {
                return false; // Already pending
            }

            // RFC 9001: Must wait until we've sent at least one packet with current keys
            if (key_update_count > 0 && current_pn <= current_phase_pn) {
                return false;
            }

            pending_key_update = true;
            echo::debug("Key update initiated at PN ", current_pn);
            return true;
        }

        // Apply key update before sending (toggles phase, rotates keys)
        void apply_pending_key_update(dp::u64 pn) {
            if (!pending_key_update) {
                return;
            }

            // Save previous keys for decrypting delayed packets
            prev_read_keys = read_keys;
            prev_phase_end_pn = last_key_update_pn > 0 ? last_key_update_pn : 0;

            // Rotate secrets and keys
            write_secret = next_write_secret;
            read_secret = next_read_secret;
            write_keys = next_write_keys;
            read_keys = next_read_keys;

            // Pre-compute next phase
            next_write_secret = derive_key_update_secret(write_secret);
            next_read_secret = derive_key_update_secret(read_secret);
            next_write_keys = derive_keys(next_write_secret);
            next_read_keys = derive_keys(next_read_secret);

            // Toggle phase
            current_phase = !current_phase;
            current_phase_pn = pn;
            last_key_update_pn = pn;
            key_update_count++;
            pending_key_update = false;
            peer_key_update_received = false;

            echo::debug("Key update applied at PN ", pn, ", phase now ", current_phase ? 1 : 0, ", update count ",
                        key_update_count);
        }

        // Process received packet with potentially different key phase
        // Returns the keys to use for decryption, or nullptr if phase is invalid
        const QuicKeys *get_decryption_keys(bool received_phase, dp::u64 pn) {
            if (received_phase == current_phase) {
                // Same phase - use current keys
                return &read_keys;
            }

            // Different phase - could be key update or previous phase packet
            if (key_update_count > 0 && pn <= prev_phase_end_pn) {
                // Old packet from previous phase
                return prev_read_keys.is_valid() ? &prev_read_keys : nullptr;
            }

            // This is a peer-initiated key update
            // We need to update our keys to match
            peer_key_update_received = true;

            // Save previous keys
            prev_read_keys = read_keys;
            prev_phase_end_pn = last_key_update_pn > 0 ? last_key_update_pn : 0;

            // Rotate secrets and keys (triggered by peer)
            write_secret = next_write_secret;
            read_secret = next_read_secret;
            write_keys = next_write_keys;
            read_keys = next_read_keys;

            // Pre-compute next phase
            next_write_secret = derive_key_update_secret(write_secret);
            next_read_secret = derive_key_update_secret(read_secret);
            next_write_keys = derive_keys(next_write_secret);
            next_read_keys = derive_keys(next_read_secret);

            // Toggle phase to match peer
            current_phase = received_phase;
            last_key_update_pn = pn;
            key_update_count++;

            echo::debug("Peer key update received at PN ", pn, ", phase now ", current_phase ? 1 : 0);

            return &read_keys;
        }

        // Check if we need to send with updated keys (to respond to peer update)
        bool should_send_key_update() const { return peer_key_update_received && !pending_key_update; }

        // Get current write phase
        bool get_write_phase() const { return current_phase; }

        // Get encryption keys for sending
        const QuicKeys &get_encryption_keys() const { return write_keys; }
    };

    // QUIC packet protection (AEAD encryption/decryption)
    class PacketProtection {
      public:
        PacketProtection() = default;

        explicit PacketProtection(const QuicKeys &keys) : keys_(keys) {}

        void set_keys(const QuicKeys &keys) { keys_ = keys; }

        const QuicKeys &keys() const { return keys_; }

        bool has_keys() const { return keys_.is_valid(); }

        // Construct nonce from IV and packet number
        dp::Vector<dp::u8> construct_nonce(dp::u64 packet_number) const {
            dp::Vector<dp::u8> nonce = keys_.iv;

            // XOR packet number into the last 8 bytes of the IV
            for (int i = 0; i < 8; i++) {
                nonce[nonce.size() - 1 - i] ^= static_cast<dp::u8>((packet_number >> (i * 8)) & 0xFF);
            }

            return nonce;
        }

        // Encrypt packet payload
        // header: the full packet header (for AAD)
        // payload: the plaintext payload
        // packet_number: for nonce construction
        dp::Res<dp::Vector<dp::u8>> encrypt(const dp::Vector<dp::u8> &header, const dp::Vector<dp::u8> &payload,
                                            dp::u64 packet_number) {
            if (!has_keys()) {
                return dp::result::err(dp::Error::invalid_argument("no encryption keys"));
            }

            auto nonce = construct_nonce(packet_number);

            dp::Vector<dp::u8> ciphertext(payload.size() + AEAD_TAG_LENGTH);
            unsigned long long ciphertext_len;

            // Use ChaCha20-Poly1305 for encryption
            if (keylock::crypto::aead_chacha20poly1305_ietf::encrypt(ciphertext.data(), &ciphertext_len, payload.data(),
                                                                     payload.size(), header.data(), header.size(),
                                                                     nullptr, nonce.data(), keys_.key.data()) != 0) {
                return dp::result::err(dp::Error::io_error("encryption failed"));
            }

            ciphertext.resize(ciphertext_len);
            return dp::result::ok(std::move(ciphertext));
        }

        // Decrypt packet payload
        dp::Res<dp::Vector<dp::u8>> decrypt(const dp::Vector<dp::u8> &header, const dp::Vector<dp::u8> &ciphertext,
                                            dp::u64 packet_number) {
            if (!has_keys()) {
                return dp::result::err(dp::Error::invalid_argument("no decryption keys"));
            }

            if (ciphertext.size() < AEAD_TAG_LENGTH) {
                return dp::result::err(dp::Error::invalid_argument("ciphertext too short"));
            }

            auto nonce = construct_nonce(packet_number);

            dp::Vector<dp::u8> plaintext(ciphertext.size() - AEAD_TAG_LENGTH);
            unsigned long long plaintext_len;

            if (keylock::crypto::aead_chacha20poly1305_ietf::decrypt(
                    plaintext.data(), &plaintext_len, nullptr, ciphertext.data(), ciphertext.size(), header.data(),
                    header.size(), nonce.data(), keys_.key.data()) != 0) {
                return dp::result::err(dp::Error::io_error("decryption failed"));
            }

            plaintext.resize(plaintext_len);
            return dp::result::ok(std::move(plaintext));
        }

      private:
        QuicKeys keys_;
    };

    // Header protection (RFC 9001 Section 5.4)
    // This protects the packet number field and some header bits
    class HeaderProtection {
      public:
        HeaderProtection() = default;

        explicit HeaderProtection(const dp::Vector<dp::u8> &hp_key) : hp_key_(hp_key) {}

        void set_key(const dp::Vector<dp::u8> &hp_key) { hp_key_ = hp_key; }

        bool has_key() const { return !hp_key_.empty(); }

        // Generate header protection mask using ChaCha20
        dp::Vector<dp::u8> generate_mask(const dp::u8 *sample) const {
            dp::Vector<dp::u8> mask(5); // 5 bytes needed for header protection

            // For ChaCha20, sample is used as counter (first 4 bytes) and nonce (next 12 bytes)
            // But QUIC uses a simplified approach: sample is 16 bytes fed to AES-ECB or ChaCha20

            // Using ChaCha20 with counter=sample[0..3], nonce=sample[4..15]
            dp::u8 nonce[12];
            std::memcpy(nonce, sample + 4, 12);

            dp::u32 counter = (static_cast<dp::u32>(sample[0])) | (static_cast<dp::u32>(sample[1]) << 8) |
                              (static_cast<dp::u32>(sample[2]) << 16) | (static_cast<dp::u32>(sample[3]) << 24);

            // Generate keystream
            dp::u8 zeros[5] = {0};
            keylock::crypto::chacha20::chacha20_ietf(mask.data(), zeros, 5, hp_key_.data(), nonce, counter);

            return mask;
        }

        // Apply header protection (encrypt)
        // This modifies the header in place
        void protect_header(dp::Vector<dp::u8> &packet, dp::usize pn_offset, dp::usize pn_length) const {
            if (!has_key() || packet.size() < pn_offset + pn_length + HEADER_PROTECTION_SAMPLE_LENGTH) {
                return;
            }

            // Sample starts 4 bytes after the start of the packet number
            const dp::u8 *sample = packet.data() + pn_offset + 4;
            auto mask = generate_mask(sample);

            // Apply mask to first byte
            if (is_long_header(packet[0])) {
                // Long header: protect lower 4 bits
                packet[0] ^= (mask[0] & 0x0F);
            } else {
                // Short header: protect lower 5 bits
                packet[0] ^= (mask[0] & 0x1F);
            }

            // Apply mask to packet number
            for (dp::usize i = 0; i < pn_length; i++) {
                packet[pn_offset + i] ^= mask[1 + i];
            }
        }

        // Remove header protection (decrypt)
        // Returns the decoded packet number length
        dp::Res<dp::u8> unprotect_header(dp::Vector<dp::u8> &packet, dp::usize pn_offset) const {
            if (!has_key()) {
                return dp::result::err(dp::Error::invalid_argument("no header protection key"));
            }

            if (packet.size() < pn_offset + 4 + HEADER_PROTECTION_SAMPLE_LENGTH) {
                return dp::result::err(dp::Error::invalid_argument("packet too short for header protection"));
            }

            // Sample starts 4 bytes after the start of the packet number
            const dp::u8 *sample = packet.data() + pn_offset + 4;
            auto mask = generate_mask(sample);

            // Unprotect first byte to get packet number length
            if (is_long_header(packet[0])) {
                packet[0] ^= (mask[0] & 0x0F);
            } else {
                packet[0] ^= (mask[0] & 0x1F);
            }

            // Get packet number length from first byte
            dp::u8 pn_length = (packet[0] & PACKET_NUMBER_LENGTH_MASK) + 1;

            if (packet.size() < pn_offset + pn_length) {
                return dp::result::err(dp::Error::invalid_argument("packet too short for packet number"));
            }

            // Unprotect packet number
            for (dp::u8 i = 0; i < pn_length; i++) {
                packet[pn_offset + i] ^= mask[1 + i];
            }

            return dp::result::ok(pn_length);
        }

      private:
        dp::Vector<dp::u8> hp_key_;
    };

    // Complete QUIC crypto state for one direction (send or receive)
    class CryptoState {
      public:
        CryptoState() = default;

        void set_keys(const QuicKeys &keys) {
            protection_.set_keys(keys);
            header_protection_.set_key(keys.hp_key);
        }

        bool has_keys() const { return protection_.has_keys(); }

        PacketProtection &packet_protection() { return protection_; }
        const PacketProtection &packet_protection() const { return protection_; }

        HeaderProtection &header_protection() { return header_protection_; }
        const HeaderProtection &header_protection() const { return header_protection_; }

        // Encrypt a complete packet (payload encryption + header protection)
        dp::Res<dp::Vector<dp::u8>> encrypt_packet(dp::Vector<dp::u8> header, const dp::Vector<dp::u8> &payload,
                                                   dp::u64 packet_number, dp::usize pn_offset, dp::u8 pn_length) {
            // First, encrypt the payload
            auto encrypted_result = protection_.encrypt(header, payload, packet_number);
            if (encrypted_result.is_err()) {
                return dp::result::err(encrypted_result.error());
            }

            // Combine header and encrypted payload
            dp::Vector<dp::u8> packet = std::move(header);
            auto &encrypted = encrypted_result.value();
            packet.insert(packet.end(), encrypted.begin(), encrypted.end());

            // Apply header protection
            header_protection_.protect_header(packet, pn_offset, pn_length);

            return dp::result::ok(std::move(packet));
        }

        // Decrypt a complete packet (header unprotection + payload decryption)
        dp::Res<std::pair<dp::u64, dp::Vector<dp::u8>>> decrypt_packet(dp::Vector<dp::u8> &packet, dp::usize pn_offset,
                                                                       dp::u64 largest_pn) {
            // Remove header protection
            auto pn_length_result = header_protection_.unprotect_header(packet, pn_offset);
            if (pn_length_result.is_err()) {
                return dp::result::err(pn_length_result.error());
            }
            dp::u8 pn_length = pn_length_result.value();

            // Decode truncated packet number
            dp::u64 truncated_pn = decode_packet_number(packet.data() + pn_offset, pn_length - 1);

            // Expand to full packet number
            dp::u64 packet_number = expand_packet_number(largest_pn, truncated_pn, pn_length - 1);

            // Extract header and ciphertext
            dp::usize header_len = pn_offset + pn_length;
            dp::Vector<dp::u8> header(packet.begin(), packet.begin() + header_len);
            dp::Vector<dp::u8> ciphertext(packet.begin() + header_len, packet.end());

            // Decrypt payload
            auto plaintext_result = protection_.decrypt(header, ciphertext, packet_number);
            if (plaintext_result.is_err()) {
                return dp::result::err(plaintext_result.error());
            }

            return dp::result::ok(std::make_pair(packet_number, std::move(plaintext_result.value())));
        }

      private:
        PacketProtection protection_;
        HeaderProtection header_protection_;
    };

    // Retry integrity tag computation (RFC 9001 Section 5.8)
    // Uses AES-128-GCM with fixed key and nonce (per QUIC version)
    constexpr dp::u8 RETRY_INTEGRITY_KEY_V1[] = {0xbe, 0x0c, 0x69, 0x0b, 0x9f, 0x66, 0x57, 0x5a,
                                                 0x1d, 0x76, 0x6b, 0x54, 0xe3, 0x68, 0xc8, 0x4e};
    constexpr dp::u8 RETRY_INTEGRITY_NONCE_V1[] = {0x46, 0x15, 0x99, 0xd3, 0x5d, 0x63,
                                                   0x2b, 0xf2, 0x23, 0x98, 0x25, 0xbb};

    // QUIC Version 2 Retry keys (RFC 9369)
    constexpr dp::u8 RETRY_INTEGRITY_KEY_V2[] = {0x8f, 0xb4, 0xb0, 0x1b, 0x56, 0xac, 0x48, 0xe2,
                                                 0x60, 0xfb, 0xcb, 0xce, 0xad, 0x7c, 0xcc, 0x92};
    constexpr dp::u8 RETRY_INTEGRITY_NONCE_V2[] = {0xd8, 0x69, 0x69, 0xbc, 0x2d, 0x7c,
                                                   0x6d, 0x99, 0x90, 0xef, 0xb0, 0x4a};

    namespace retry_detail {
        // Minimal AES-128 implementation for Retry integrity tag
        // (Only block encryption is needed for GCM with empty plaintext)
        namespace aes128 {
            constexpr dp::u8 sbox[256] = {
                0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
                0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
                0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
                0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
                0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
                0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
                0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
                0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
                0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
                0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
                0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
                0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
                0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
                0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
                0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
                0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};
            constexpr dp::u8 rcon[11] = {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36};

            inline dp::u32 sub_word(dp::u32 w) {
                return (static_cast<dp::u32>(sbox[(w >> 0) & 0xFF]) << 0) |
                       (static_cast<dp::u32>(sbox[(w >> 8) & 0xFF]) << 8) |
                       (static_cast<dp::u32>(sbox[(w >> 16) & 0xFF]) << 16) |
                       (static_cast<dp::u32>(sbox[(w >> 24) & 0xFF]) << 24);
            }

            inline dp::u32 rot_word(dp::u32 v) { return ((v >> 8) & 0x00FFFFFF) | ((v & 0xFF) << 24); }

            inline dp::u8 mul2(dp::u8 b) {
                dp::u8 m2 = b << 1;
                if (b & 0x80)
                    m2 ^= 0x1B;
                return m2;
            }

            struct Context {
                dp::u32 round_keys[44]; // 11 * 4 for AES-128
            };

            inline void key_setup(Context *ctx, const dp::u8 *key) {
                for (int i = 0; i < 4; ++i) {
                    std::memcpy(&ctx->round_keys[i], key + i * 4, 4);
                }
                for (int i = 4; i < 44; ++i) {
                    dp::u32 t = ctx->round_keys[i - 1];
                    if (i % 4 == 0) {
                        t = sub_word(rot_word(t)) ^ rcon[i / 4];
                    }
                    ctx->round_keys[i] = t ^ ctx->round_keys[i - 4];
                }
            }

            inline void add_round_key(dp::u8 state[16], const dp::u32 *rk) {
                for (int i = 0; i < 4; ++i) {
                    state[i * 4 + 0] ^= (rk[i] >> 0) & 0xFF;
                    state[i * 4 + 1] ^= (rk[i] >> 8) & 0xFF;
                    state[i * 4 + 2] ^= (rk[i] >> 16) & 0xFF;
                    state[i * 4 + 3] ^= (rk[i] >> 24) & 0xFF;
                }
            }

            inline void sub_bytes(dp::u8 state[16]) {
                for (int i = 0; i < 16; ++i)
                    state[i] = sbox[state[i]];
            }

            inline void shift_rows(dp::u8 state[16]) {
                dp::u8 t;
                t = state[1];
                state[1] = state[5];
                state[5] = state[9];
                state[9] = state[13];
                state[13] = t;
                t = state[2];
                state[2] = state[10];
                state[10] = t;
                t = state[6];
                state[6] = state[14];
                state[14] = t;
                t = state[15];
                state[15] = state[11];
                state[11] = state[7];
                state[7] = state[3];
                state[3] = t;
            }

            inline void mix_columns(dp::u8 state[16]) {
                for (int c = 0; c < 4; ++c) {
                    dp::u8 *col = &state[c * 4];
                    dp::u8 a = col[0], b = col[1], c_ = col[2], d = col[3];
                    col[0] = mul2(a) ^ mul2(b) ^ b ^ c_ ^ d;
                    col[1] = a ^ mul2(b) ^ mul2(c_) ^ c_ ^ d;
                    col[2] = a ^ b ^ mul2(c_) ^ mul2(d) ^ d;
                    col[3] = mul2(a) ^ a ^ b ^ c_ ^ mul2(d);
                }
            }

            inline void encrypt_block(const Context *ctx, const dp::u8 in[16], dp::u8 out[16]) {
                dp::u8 state[16];
                std::memcpy(state, in, 16);
                add_round_key(state, &ctx->round_keys[0]);
                for (int r = 1; r < 10; ++r) {
                    sub_bytes(state);
                    shift_rows(state);
                    mix_columns(state);
                    add_round_key(state, &ctx->round_keys[r * 4]);
                }
                sub_bytes(state);
                shift_rows(state);
                add_round_key(state, &ctx->round_keys[40]);
                std::memcpy(out, state, 16);
            }
        } // namespace aes128

        // GF(2^128) multiplication for GHASH
        inline void gf_mul(dp::u8 *Z, const dp::u8 *X, const dp::u8 *Y) {
            dp::u8 V[16];
            std::memset(Z, 0, 16);
            std::memcpy(V, Y, 16);

            for (int i = 0; i < 128; ++i) {
                int byte_idx = i / 8;
                int bit_idx = 7 - (i % 8);
                if (X[byte_idx] & (1 << bit_idx)) {
                    for (int j = 0; j < 16; ++j) {
                        Z[j] ^= V[j];
                    }
                }
                int lsb = V[15] & 1;
                for (int j = 15; j > 0; --j) {
                    V[j] = (V[j] >> 1) | (V[j - 1] << 7);
                }
                V[0] >>= 1;
                if (lsb) {
                    V[0] ^= 0xe1;
                }
            }
        }

        // GHASH for GCM
        inline void ghash(dp::u8 *result, const dp::u8 *H, const dp::u8 *data, dp::usize data_size) {
            dp::u8 Y[16] = {0};
            dp::usize blocks = data_size / 16;

            for (dp::usize i = 0; i < blocks; ++i) {
                for (int j = 0; j < 16; ++j) {
                    Y[j] ^= data[i * 16 + j];
                }
                dp::u8 tmp[16];
                gf_mul(tmp, Y, H);
                std::memcpy(Y, tmp, 16);
            }

            // Handle partial block
            dp::usize remainder = data_size % 16;
            if (remainder > 0) {
                for (dp::usize j = 0; j < remainder; ++j) {
                    Y[j] ^= data[blocks * 16 + j];
                }
                dp::u8 tmp[16];
                gf_mul(tmp, Y, H);
                std::memcpy(Y, tmp, 16);
            }

            std::memcpy(result, Y, 16);
        }

        // Compute AES-128-GCM tag for Retry integrity
        inline dp::Vector<dp::u8> compute_tag_aes128gcm(const dp::u8 *key, const dp::u8 *nonce, const dp::u8 *aad,
                                                        dp::usize aad_size) {
            aes128::Context aes_ctx;
            aes128::key_setup(&aes_ctx, key);

            // Compute H = AES(K, 0^128)
            dp::u8 zero_block[16] = {0};
            dp::u8 H[16];
            aes128::encrypt_block(&aes_ctx, zero_block, H);

            // Compute J0 for 96-bit nonce: nonce || 0^31 || 1
            dp::u8 J0[16];
            std::memcpy(J0, nonce, 12);
            J0[12] = 0;
            J0[13] = 0;
            J0[14] = 0;
            J0[15] = 1;

            // Build GHASH input: AAD || 0^v || [len(A)]_64 || [len(C)]_64
            // For empty ciphertext, this is: AAD || padding || len(A) || 0
            dp::usize aad_pad = (16 - (aad_size % 16)) % 16;
            dp::usize ghash_input_size = aad_size + aad_pad + 16;
            dp::Vector<dp::u8> ghash_input(ghash_input_size, 0);

            std::memcpy(ghash_input.data(), aad, aad_size);
            // Padding is already 0

            // Length block: [len(A) * 8]_64 || [len(C) * 8]_64
            dp::u64 aad_bits = aad_size * 8;
            dp::u64 cipher_bits = 0; // empty ciphertext
            dp::usize len_offset = aad_size + aad_pad;
            for (int i = 0; i < 8; ++i) {
                ghash_input[len_offset + i] = static_cast<dp::u8>((aad_bits >> (56 - i * 8)) & 0xFF);
                ghash_input[len_offset + 8 + i] = static_cast<dp::u8>((cipher_bits >> (56 - i * 8)) & 0xFF);
            }

            // Compute S = GHASH(H, ghash_input)
            dp::u8 S[16];
            ghash(S, H, ghash_input.data(), ghash_input.size());

            // Tag = GCTR(J0, S) = S XOR AES(K, J0)
            dp::u8 E_J0[16];
            aes128::encrypt_block(&aes_ctx, J0, E_J0);

            dp::Vector<dp::u8> tag(16);
            for (int i = 0; i < 16; ++i) {
                tag[i] = S[i] ^ E_J0[i];
            }

            return tag;
        }
    } // namespace retry_detail

    inline dp::Vector<dp::u8> compute_retry_integrity_tag(const dp::Vector<dp::u8> &retry_pseudo_packet,
                                                          dp::u32 version = QUIC_VERSION_1) {
        // Select key and nonce based on version
        const dp::u8 *key = RETRY_INTEGRITY_KEY_V1;
        const dp::u8 *nonce = RETRY_INTEGRITY_NONCE_V1;

        if (version == QUIC_VERSION_2) {
            key = RETRY_INTEGRITY_KEY_V2;
            nonce = RETRY_INTEGRITY_NONCE_V2;
        }

        // Compute AES-128-GCM tag over empty plaintext with pseudo-packet as AAD
        return retry_detail::compute_tag_aes128gcm(key, nonce, retry_pseudo_packet.data(), retry_pseudo_packet.size());
    }

    // Verify Retry packet integrity tag
    inline bool verify_retry_integrity_tag(const dp::Vector<dp::u8> &retry_pseudo_packet,
                                           const dp::Vector<dp::u8> &received_tag, dp::u32 version = QUIC_VERSION_1) {
        if (received_tag.size() != AEAD_TAG_LENGTH) {
            echo::warn("Retry integrity tag wrong size: ", received_tag.size());
            return false;
        }

        auto expected_tag = compute_retry_integrity_tag(retry_pseudo_packet, version);

        // Constant-time comparison
        bool match = true;
        for (dp::usize i = 0; i < AEAD_TAG_LENGTH; ++i) {
            if (expected_tag[i] != received_tag[i]) {
                match = false;
            }
        }

        if (!match) {
            echo::warn("Retry integrity tag mismatch");
        }
        return match;
    }

    // Build Retry pseudo-packet for integrity tag computation (RFC 9001 Section 5.8)
    // Pseudo-packet = ODCID length || ODCID || Retry packet without tag
    inline dp::Vector<dp::u8> build_retry_pseudo_packet(const ConnectionId &original_dcid,
                                                        const dp::Vector<dp::u8> &retry_packet_without_tag) {
        dp::Vector<dp::u8> pseudo;
        pseudo.reserve(1 + original_dcid.data.size() + retry_packet_without_tag.size());

        // Original DCID length (1 byte)
        pseudo.push_back(static_cast<dp::u8>(original_dcid.data.size()));
        // Original DCID
        pseudo.insert(pseudo.end(), original_dcid.data.begin(), original_dcid.data.end());
        // Retry packet (header + retry token, without integrity tag)
        pseudo.insert(pseudo.end(), retry_packet_without_tag.begin(), retry_packet_without_tag.end());

        return pseudo;
    }

} // namespace netpipe::quic

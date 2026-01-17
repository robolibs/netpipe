#pragma once

#include <datapod/datapod.hpp>
#include <echo/echo.hpp>
#include <keylock/crypto/common.hpp>
#include <netpipe/transport/stream/quic/packet.hpp>
#include <netpipe/transport/stream/quic/types.hpp>
#include <sodium.h>

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
        dp::Vector<dp::u8> prk(crypto_auth_hmacsha256_BYTES);

        crypto_auth_hmacsha256_state state;
        crypto_auth_hmacsha256_init(&state, salt, salt_len);
        crypto_auth_hmacsha256_update(&state, ikm, ikm_len);
        crypto_auth_hmacsha256_final(&state, prk.data());

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
            crypto_auth_hmacsha256_state state;
            crypto_auth_hmacsha256_init(&state, secret.data(), secret.size());

            if (!t.empty()) {
                crypto_auth_hmacsha256_update(&state, t.data(), t.size());
            }
            crypto_auth_hmacsha256_update(&state, hkdf_label.data(), hkdf_label.size());
            crypto_auth_hmacsha256_update(&state, &counter, 1);

            t.resize(crypto_auth_hmacsha256_BYTES);
            crypto_auth_hmacsha256_final(&state, t.data());

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
            if (crypto_aead_chacha20poly1305_ietf_encrypt(ciphertext.data(), &ciphertext_len, payload.data(),
                                                          payload.size(), header.data(), header.size(), nullptr,
                                                          nonce.data(), keys_.key.data()) != 0) {
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

            if (crypto_aead_chacha20poly1305_ietf_decrypt(plaintext.data(), &plaintext_len, nullptr, ciphertext.data(),
                                                          ciphertext.size(), header.data(), header.size(), nonce.data(),
                                                          keys_.key.data()) != 0) {
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
            crypto_stream_chacha20_ietf_xor_ic(mask.data(), zeros, 5, nonce, counter, hp_key_.data());

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
    constexpr dp::u8 RETRY_INTEGRITY_KEY_V1[] = {0xbe, 0x0c, 0x69, 0x0b, 0x9f, 0x66, 0x57, 0x5a,
                                                 0x1d, 0x76, 0x6b, 0x54, 0xe3, 0x68, 0xc8, 0x4e};
    constexpr dp::u8 RETRY_INTEGRITY_NONCE_V1[] = {0x46, 0x15, 0x99, 0xd3, 0x5d, 0x63,
                                                   0x2b, 0xf2, 0x23, 0x98, 0x25, 0xbb};

    inline dp::Vector<dp::u8> compute_retry_integrity_tag(const dp::Vector<dp::u8> &retry_pseudo_packet,
                                                          dp::u32 version = QUIC_VERSION_1) {
        dp::Vector<dp::u8> tag(AEAD_TAG_LENGTH);

        // For version 1, use the defined key and nonce
        // The retry pseudo packet is the AAD, empty plaintext
        unsigned long long tag_len;

        crypto_aead_chacha20poly1305_ietf_encrypt(tag.data(), &tag_len, nullptr, 0, // empty plaintext
                                                  retry_pseudo_packet.data(), retry_pseudo_packet.size(),
                                                  nullptr, // no secret key
                                                  RETRY_INTEGRITY_NONCE_V1, RETRY_INTEGRITY_KEY_V1);

        tag.resize(tag_len);
        return tag;
    }

} // namespace netpipe::quic

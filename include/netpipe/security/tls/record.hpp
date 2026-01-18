#pragma once

#include <datapod/datapod.hpp>
#include <echo/echo.hpp>
#include <keylock/crypto/common.hpp>
#include <keylock/crypto/context.hpp>
#include <netpipe/security/tls/key_schedule.hpp>

#include <sodium.h>

namespace netpipe::tls {

    // TLS 1.3 Content Types
    enum class ContentType : dp::u8 {
        Invalid = 0,
        ChangeCipherSpec = 20, // Legacy, ignored in TLS 1.3
        Alert = 21,
        Handshake = 22,
        ApplicationData = 23
    };

    // TLS 1.3 Record Header
    // ContentType (1) | Legacy Version (2) | Length (2)
    constexpr dp::usize RECORD_HEADER_SIZE = 5;

    // Maximum TLS record size (2^14 = 16384 bytes of plaintext)
    constexpr dp::usize MAX_RECORD_PLAINTEXT_SIZE = 16384;

    // Maximum encrypted record size (plaintext + content type + tag + possible padding)
    constexpr dp::usize MAX_RECORD_CIPHERTEXT_SIZE = MAX_RECORD_PLAINTEXT_SIZE + 256 + 16;

    // Legacy TLS 1.2 version for compatibility
    constexpr dp::u16 LEGACY_VERSION = 0x0303;

    // AEAD tag size (Poly1305 = 16 bytes)
    constexpr dp::usize AEAD_TAG_SIZE = 16;

    // Nonce/IV size for ChaCha20-Poly1305 IETF
    constexpr dp::usize NONCE_SIZE = 12;

    // Key size for ChaCha20-Poly1305 and AES-256-GCM
    constexpr dp::usize KEY_SIZE = 32;

    // Maximum sequence number before key update is required (RFC 8446 recommends updating before 2^24)
    // We use a conservative limit to ensure safety
    constexpr dp::u64 MAX_SEQUENCE_NUMBER = (1ULL << 48) - 1; // 2^48 - 1

    // Cipher suite identifiers
    constexpr dp::u16 CIPHER_CHACHA20_POLY1305 = 0x1303;
    constexpr dp::u16 CIPHER_AES_256_GCM = 0x1302;
    constexpr dp::u16 CIPHER_AES_128_GCM = 0x1301;

    // AEAD cipher selection
    enum class CipherSuite : dp::u16 {
        ChaCha20_Poly1305 = CIPHER_CHACHA20_POLY1305,
        AES_256_GCM = CIPHER_AES_256_GCM,
        AES_128_GCM = CIPHER_AES_128_GCM
    };

    // TLS 1.3 Record Layer
    // Handles encryption and decryption of TLS records
    struct RecordLayer {
        // Write (outgoing) traffic keys
        dp::Vector<dp::u8> write_key;
        dp::Vector<dp::u8> write_iv;
        dp::u64 write_seq = 0;

        // Read (incoming) traffic keys
        dp::Vector<dp::u8> read_key;
        dp::Vector<dp::u8> read_iv;
        dp::u64 read_seq = 0;

        // Whether encryption is active
        bool write_encrypted = false;
        bool read_encrypted = false;

        // Selected cipher suite (default: ChaCha20-Poly1305)
        CipherSuite cipher_suite = CipherSuite::ChaCha20_Poly1305;

        // Set cipher suite (call before setting traffic secrets)
        void set_cipher_suite(dp::u16 suite) {
            switch (suite) {
            case CIPHER_AES_256_GCM:
                cipher_suite = CipherSuite::AES_256_GCM;
                echo::debug("Using AES-256-GCM cipher");
                break;
            case CIPHER_AES_128_GCM:
                cipher_suite = CipherSuite::AES_128_GCM;
                echo::debug("Using AES-128-GCM cipher");
                break;
            case CIPHER_CHACHA20_POLY1305:
            default:
                cipher_suite = CipherSuite::ChaCha20_Poly1305;
                echo::debug("Using ChaCha20-Poly1305 cipher");
                break;
            }
        }

        // Get key size for current cipher
        dp::usize get_key_size() const {
            switch (cipher_suite) {
            case CipherSuite::AES_128_GCM:
                return 16;
            default:
                return 32;
            }
        }

        // Set write traffic secret (derives key and IV)
        void set_write_traffic_secret(const dp::Vector<dp::u8> &traffic_secret) {
            echo::trace("RecordLayer::set_write_traffic_secret");
            auto [key, iv] = KeySchedule::derive_traffic_keys(traffic_secret, KEY_SIZE, NONCE_SIZE);
            write_key = std::move(key);
            write_iv = std::move(iv);
            write_seq = 0;
            write_encrypted = true;
            echo::debug("Write encryption enabled (key=", write_key.size(), " bytes, iv=", write_iv.size(), " bytes)");
        }

        // Set read traffic secret (derives key and IV)
        void set_read_traffic_secret(const dp::Vector<dp::u8> &traffic_secret) {
            echo::trace("RecordLayer::set_read_traffic_secret");
            auto [key, iv] = KeySchedule::derive_traffic_keys(traffic_secret, KEY_SIZE, NONCE_SIZE);
            read_key = std::move(key);
            read_iv = std::move(iv);
            read_seq = 0;
            read_encrypted = true;
            echo::debug("Read encryption enabled (key=", read_key.size(), " bytes, iv=", read_iv.size(), " bytes)");
        }

        // Compute nonce: IV XOR sequence_number (padded to 12 bytes)
        static dp::Vector<dp::u8> compute_nonce(const dp::Vector<dp::u8> &iv, dp::u64 seq) {
            dp::Vector<dp::u8> nonce(NONCE_SIZE, 0);

            // Sequence number as 8-byte big-endian, right-aligned in 12 bytes
            for (dp::usize i = 0; i < 8; ++i) {
                nonce[NONCE_SIZE - 8 + i] = static_cast<dp::u8>((seq >> (56 - i * 8)) & 0xFF);
            }

            // XOR with IV
            for (dp::usize i = 0; i < NONCE_SIZE; ++i) {
                nonce[i] ^= iv[i];
            }

            return nonce;
        }

        // Build Additional Authenticated Data (AAD) for AEAD
        // AAD = record header (content_type || legacy_version || length)
        static dp::Vector<dp::u8> build_aad(ContentType outer_type, dp::u16 ciphertext_length) {
            dp::Vector<dp::u8> aad;
            aad.push_back(static_cast<dp::u8>(outer_type));
            aad.push_back(static_cast<dp::u8>((LEGACY_VERSION >> 8) & 0xFF));
            aad.push_back(static_cast<dp::u8>(LEGACY_VERSION & 0xFF));
            aad.push_back(static_cast<dp::u8>((ciphertext_length >> 8) & 0xFF));
            aad.push_back(static_cast<dp::u8>(ciphertext_length & 0xFF));
            return aad;
        }

        // Check if sequence number is approaching limit (needs key update)
        bool needs_key_update() const {
            return write_seq >= MAX_SEQUENCE_NUMBER - 1000 || read_seq >= MAX_SEQUENCE_NUMBER - 1000;
        }

        // Encrypt a TLS record (returns full record including header)
        // inner_type: the real content type (stored in encrypted payload)
        // plaintext: the actual data to encrypt
        dp::Res<dp::Vector<dp::u8>> encrypt(ContentType inner_type, const dp::Vector<dp::u8> &plaintext) {
            echo::trace("RecordLayer::encrypt: inner_type=", static_cast<int>(inner_type),
                        " plaintext_size=", plaintext.size());

            if (!write_encrypted) {
                // Not encrypted - send as plaintext record
                return build_plaintext_record(inner_type, plaintext);
            }

            // Check for sequence number overflow (RFC 8446 Section 5.3)
            if (write_seq >= MAX_SEQUENCE_NUMBER) {
                echo::error("Write sequence number overflow - key update required");
                return dp::result::err(dp::Error::io_error("sequence number overflow - key update required"));
            }

            if (plaintext.size() > MAX_RECORD_PLAINTEXT_SIZE) {
                return dp::result::err(dp::Error::invalid_argument("plaintext exceeds maximum record size"));
            }

            // Build inner plaintext: payload || inner_content_type
            // (No padding for simplicity)
            dp::Vector<dp::u8> inner_plaintext;
            inner_plaintext.reserve(plaintext.size() + 1);
            inner_plaintext.insert(inner_plaintext.end(), plaintext.begin(), plaintext.end());
            inner_plaintext.push_back(static_cast<dp::u8>(inner_type));

            // Compute nonce
            auto nonce = compute_nonce(write_iv, write_seq);

            // Ciphertext length = plaintext + content type + tag
            dp::u16 ciphertext_length = static_cast<dp::u16>(inner_plaintext.size() + AEAD_TAG_SIZE);

            // Build AAD (using outer type = ApplicationData for encrypted records)
            auto aad = build_aad(ContentType::ApplicationData, ciphertext_length);

            // Convert to std::vector for keylock
            std::vector<uint8_t> plaintext_std(inner_plaintext.begin(), inner_plaintext.end());
            std::vector<uint8_t> key_std(write_key.begin(), write_key.end());
            std::vector<uint8_t> aad_std(aad.begin(), aad.end());

            // Encrypt using ChaCha20-Poly1305
            keylock::crypto::Context crypto(keylock::crypto::Context::Algorithm::ChaCha20_Poly1305);

            // Need to prepend nonce to plaintext for keylock's encrypt API
            // Actually, looking at keylock API, it generates random nonce internally
            // We need to use the raw libsodium API or modify our approach

            // Encrypt using the selected cipher
            std::vector<uint8_t> ciphertext(inner_plaintext.size() + AEAD_TAG_SIZE);
            unsigned long long ciphertext_len;
            std::vector<uint8_t> nonce_std(nonce.begin(), nonce.end());

            int ret;
            if (cipher_suite == CipherSuite::AES_256_GCM || cipher_suite == CipherSuite::AES_128_GCM) {
                // Use AES-GCM (requires hardware support)
                if (!crypto_aead_aes256gcm_is_available()) {
                    echo::error("AES-GCM not available on this hardware");
                    return dp::result::err(dp::Error::io_error("AES-GCM not available"));
                }
                ret = crypto_aead_aes256gcm_encrypt(ciphertext.data(), &ciphertext_len, plaintext_std.data(),
                                                    plaintext_std.size(), aad_std.data(), aad_std.size(), nullptr,
                                                    nonce_std.data(), key_std.data());
            } else {
                // Use ChaCha20-Poly1305 (default, software implementation)
                ret = crypto_aead_chacha20poly1305_ietf_encrypt(
                    ciphertext.data(), &ciphertext_len, plaintext_std.data(), plaintext_std.size(), aad_std.data(),
                    aad_std.size(), nullptr, nonce_std.data(), key_std.data());
            }

            if (ret != 0) {
                echo::error("AEAD encryption failed");
                return dp::result::err(dp::Error::io_error("AEAD encryption failed"));
            }

            // Increment sequence number
            write_seq++;

            // Build full record: header || ciphertext
            dp::Vector<dp::u8> record;
            record.reserve(RECORD_HEADER_SIZE + ciphertext_len);

            // Header: ApplicationData (0x17) || 0x03 0x03 || length
            record.push_back(static_cast<dp::u8>(ContentType::ApplicationData));
            record.push_back(0x03);
            record.push_back(0x03);
            record.push_back(static_cast<dp::u8>((ciphertext_len >> 8) & 0xFF));
            record.push_back(static_cast<dp::u8>(ciphertext_len & 0xFF));

            // Ciphertext
            record.insert(record.end(), ciphertext.begin(), ciphertext.begin() + ciphertext_len);

            echo::debug("Encrypted record: ", record.size(), " bytes (seq=", write_seq - 1, ")");
            return dp::result::ok(std::move(record));
        }

        // Decrypt a TLS record (input is full record including header)
        // Returns: {inner_content_type, decrypted_payload}
        dp::Res<std::pair<ContentType, dp::Vector<dp::u8>>> decrypt(const dp::Vector<dp::u8> &record) {
            echo::trace("RecordLayer::decrypt: record_size=", record.size());

            if (record.size() < RECORD_HEADER_SIZE) {
                return dp::result::err(dp::Error::invalid_argument("record too short"));
            }

            // Parse header
            ContentType outer_type = static_cast<ContentType>(record[0]);
            dp::u16 version = (static_cast<dp::u16>(record[1]) << 8) | record[2];
            dp::u16 length = (static_cast<dp::u16>(record[3]) << 8) | record[4];

            (void)version; // Ignore legacy version

            if (record.size() != RECORD_HEADER_SIZE + length) {
                return dp::result::err(dp::Error::invalid_argument("record length mismatch"));
            }

            if (!read_encrypted) {
                // Not encrypted - return plaintext directly
                dp::Vector<dp::u8> payload(record.begin() + RECORD_HEADER_SIZE, record.end());
                return dp::result::ok(std::make_pair(outer_type, std::move(payload)));
            }

            // Check for sequence number overflow (RFC 8446 Section 5.3)
            if (read_seq >= MAX_SEQUENCE_NUMBER) {
                echo::error("Read sequence number overflow - key update required");
                return dp::result::err(dp::Error::io_error("sequence number overflow - key update required"));
            }

            // Must be ApplicationData when encrypted
            if (outer_type != ContentType::ApplicationData) {
                // ChangeCipherSpec is allowed and ignored
                if (outer_type == ContentType::ChangeCipherSpec) {
                    echo::trace("Ignoring ChangeCipherSpec record");
                    return dp::result::ok(std::make_pair(ContentType::Invalid, dp::Vector<dp::u8>{}));
                }
                return dp::result::err(dp::Error::invalid_argument("unexpected content type in encrypted record"));
            }

            if (length < AEAD_TAG_SIZE + 1) {
                return dp::result::err(dp::Error::invalid_argument("ciphertext too short"));
            }

            // Compute nonce
            auto nonce = compute_nonce(read_iv, read_seq);

            // Build AAD
            auto aad = build_aad(outer_type, length);

            // Extract ciphertext
            std::vector<uint8_t> ciphertext(record.begin() + RECORD_HEADER_SIZE, record.end());
            std::vector<uint8_t> key_std(read_key.begin(), read_key.end());
            std::vector<uint8_t> aad_std(aad.begin(), aad.end());
            std::vector<uint8_t> nonce_std(nonce.begin(), nonce.end());

            // Decrypt using the selected cipher
            std::vector<uint8_t> plaintext(ciphertext.size() - AEAD_TAG_SIZE);
            unsigned long long plaintext_len;

            int ret;
            if (cipher_suite == CipherSuite::AES_256_GCM || cipher_suite == CipherSuite::AES_128_GCM) {
                // Use AES-GCM
                if (!crypto_aead_aes256gcm_is_available()) {
                    echo::error("AES-GCM not available on this hardware");
                    return dp::result::err(dp::Error::io_error("AES-GCM not available"));
                }
                ret = crypto_aead_aes256gcm_decrypt(plaintext.data(), &plaintext_len, nullptr, ciphertext.data(),
                                                    ciphertext.size(), aad_std.data(), aad_std.size(), nonce_std.data(),
                                                    key_std.data());
            } else {
                // Use ChaCha20-Poly1305
                ret = crypto_aead_chacha20poly1305_ietf_decrypt(plaintext.data(), &plaintext_len, nullptr,
                                                                ciphertext.data(), ciphertext.size(), aad_std.data(),
                                                                aad_std.size(), nonce_std.data(), key_std.data());
            }

            if (ret != 0) {
                echo::error("AEAD decryption failed (bad MAC or corrupted)");
                return dp::result::err(dp::Error::io_error("AEAD decryption failed"));
            }

            // Increment sequence number
            read_seq++;

            // Parse inner plaintext: payload || content_type || zeros
            // Find the inner content type (last non-zero byte)
            if (plaintext_len == 0) {
                return dp::result::err(dp::Error::invalid_argument("empty inner plaintext"));
            }

            // Remove padding zeros and find content type
            size_t content_end = plaintext_len;
            while (content_end > 0 && plaintext[content_end - 1] == 0) {
                content_end--;
            }

            if (content_end == 0) {
                return dp::result::err(dp::Error::invalid_argument("no content type in inner plaintext"));
            }

            ContentType inner_type = static_cast<ContentType>(plaintext[content_end - 1]);
            dp::Vector<dp::u8> payload(plaintext.begin(), plaintext.begin() + content_end - 1);

            echo::debug("Decrypted record: inner_type=", static_cast<int>(inner_type), " payload_size=", payload.size(),
                        " (seq=", read_seq - 1, ")");

            return dp::result::ok(std::make_pair(inner_type, std::move(payload)));
        }

        // Build a plaintext (unencrypted) record
        static dp::Res<dp::Vector<dp::u8>> build_plaintext_record(ContentType type, const dp::Vector<dp::u8> &payload) {
            if (payload.size() > MAX_RECORD_PLAINTEXT_SIZE) {
                return dp::result::err(dp::Error::invalid_argument("payload exceeds maximum record size"));
            }

            dp::Vector<dp::u8> record;
            record.reserve(RECORD_HEADER_SIZE + payload.size());

            // Header
            record.push_back(static_cast<dp::u8>(type));
            record.push_back(0x03);
            record.push_back(0x03);
            record.push_back(static_cast<dp::u8>((payload.size() >> 8) & 0xFF));
            record.push_back(static_cast<dp::u8>(payload.size() & 0xFF));

            // Payload
            record.insert(record.end(), payload.begin(), payload.end());

            return dp::result::ok(std::move(record));
        }

        // Parse a record header (returns content type, version, length)
        static dp::Res<std::tuple<ContentType, dp::u16, dp::u16>> parse_record_header(const dp::u8 *data,
                                                                                      dp::usize size) {
            if (size < RECORD_HEADER_SIZE) {
                return dp::result::err(dp::Error::invalid_argument("not enough data for record header"));
            }

            ContentType type = static_cast<ContentType>(data[0]);
            dp::u16 version = (static_cast<dp::u16>(data[1]) << 8) | data[2];
            dp::u16 length = (static_cast<dp::u16>(data[3]) << 8) | data[4];

            return dp::result::ok(std::make_tuple(type, version, length));
        }
    };

} // namespace netpipe::tls

#pragma once

#include <datapod/datapod.hpp>
#include <echo/echo.hpp>
#include <keylock/utils/common.hpp>
#include <netpipe/tls/extensions.hpp>

namespace netpipe::tls {

    // TLS 1.3 Handshake Message Types
    enum class HandshakeType : dp::u8 {
        ClientHello = 1,
        ServerHello = 2,
        NewSessionTicket = 4,
        EndOfEarlyData = 5,
        EncryptedExtensions = 8,
        Certificate = 11,
        CertificateRequest = 13,
        CertificateVerify = 15,
        Finished = 20,
        KeyUpdate = 24
    };

    // Handshake message header size: type (1) + length (3)
    constexpr dp::usize HANDSHAKE_HEADER_SIZE = 4;

    // Cipher suites we support
    constexpr dp::u16 TLS_CHACHA20_POLY1305_SHA256 = 0x1303;
    constexpr dp::u16 TLS_AES_256_GCM_SHA384 = 0x1302;
    constexpr dp::u16 TLS_AES_128_GCM_SHA256 = 0x1301;

    // X25519 key size
    constexpr dp::usize X25519_KEY_SIZE = 32;

    // Helper to build handshake header
    inline dp::Vector<dp::u8> build_handshake_header(HandshakeType type, dp::u32 length) {
        dp::Vector<dp::u8> header;
        header.push_back(static_cast<dp::u8>(type));
        header.push_back(static_cast<dp::u8>((length >> 16) & 0xFF));
        header.push_back(static_cast<dp::u8>((length >> 8) & 0xFF));
        header.push_back(static_cast<dp::u8>(length & 0xFF));
        return header;
    }

    // Parse handshake header
    inline dp::Res<std::pair<HandshakeType, dp::u32>> parse_handshake_header(const dp::u8 *data, dp::usize size) {
        if (size < HANDSHAKE_HEADER_SIZE) {
            return dp::result::err(dp::Error::invalid_argument("handshake header too short"));
        }

        HandshakeType type = static_cast<HandshakeType>(data[0]);
        dp::u32 length = (static_cast<dp::u32>(data[1]) << 16) | (static_cast<dp::u32>(data[2]) << 8) |
                         static_cast<dp::u32>(data[3]);

        return dp::result::ok(std::make_pair(type, length));
    }

    // ClientHello message
    struct ClientHello {
        dp::u16 legacy_version = 0x0303; // TLS 1.2 for compatibility
        dp::Array<dp::u8, 32> random;
        dp::Vector<dp::u8> legacy_session_id; // 0-32 bytes
        dp::Vector<dp::u16> cipher_suites;
        dp::Vector<dp::u8> legacy_compression_methods = {0x00};
        dp::Vector<Extension> extensions;

        // Generate random bytes for this ClientHello
        void generate_random() {
            auto random_bytes = keylock::utils::Common::generate_random_bytes(32);
            std::copy(random_bytes.begin(), random_bytes.end(), random.begin());
        }

        // Serialize to bytes (without handshake header)
        dp::Vector<dp::u8> serialize_body() const {
            dp::Vector<dp::u8> result;

            // Legacy version (2 bytes)
            result.push_back(static_cast<dp::u8>((legacy_version >> 8) & 0xFF));
            result.push_back(static_cast<dp::u8>(legacy_version & 0xFF));

            // Random (32 bytes)
            result.insert(result.end(), random.begin(), random.end());

            // Session ID (1 byte length + data)
            result.push_back(static_cast<dp::u8>(legacy_session_id.size()));
            result.insert(result.end(), legacy_session_id.begin(), legacy_session_id.end());

            // Cipher suites (2 byte length + data)
            dp::u16 cs_length = static_cast<dp::u16>(cipher_suites.size() * 2);
            result.push_back(static_cast<dp::u8>((cs_length >> 8) & 0xFF));
            result.push_back(static_cast<dp::u8>(cs_length & 0xFF));
            for (auto cs : cipher_suites) {
                result.push_back(static_cast<dp::u8>((cs >> 8) & 0xFF));
                result.push_back(static_cast<dp::u8>(cs & 0xFF));
            }

            // Compression methods (1 byte length + data)
            result.push_back(static_cast<dp::u8>(legacy_compression_methods.size()));
            result.insert(result.end(), legacy_compression_methods.begin(), legacy_compression_methods.end());

            // Extensions
            auto ext_bytes = serialize_extensions(extensions);
            result.insert(result.end(), ext_bytes.begin(), ext_bytes.end());

            return result;
        }

        // Serialize with handshake header
        dp::Vector<dp::u8> serialize() const {
            auto body = serialize_body();
            auto header = build_handshake_header(HandshakeType::ClientHello, static_cast<dp::u32>(body.size()));
            header.insert(header.end(), body.begin(), body.end());
            return header;
        }

        // Parse from bytes (without handshake header)
        static dp::Res<ClientHello> parse(const dp::u8 *data, dp::usize size) {
            echo::trace("ClientHello::parse: size=", size);

            if (size < 35) { // minimum: version + random + session_id_len
                return dp::result::err(dp::Error::invalid_argument("ClientHello too short"));
            }

            ClientHello msg;
            dp::usize offset = 0;

            // Legacy version
            msg.legacy_version = (static_cast<dp::u16>(data[offset]) << 8) | data[offset + 1];
            offset += 2;

            // Random
            std::copy(data + offset, data + offset + 32, msg.random.begin());
            offset += 32;

            // Session ID
            dp::u8 session_id_len = data[offset++];
            if (offset + session_id_len > size) {
                return dp::result::err(dp::Error::invalid_argument("ClientHello session_id truncated"));
            }
            msg.legacy_session_id = dp::Vector<dp::u8>(data + offset, data + offset + session_id_len);
            offset += session_id_len;

            // Cipher suites
            if (offset + 2 > size) {
                return dp::result::err(dp::Error::invalid_argument("ClientHello cipher_suites length missing"));
            }
            dp::u16 cs_length = (static_cast<dp::u16>(data[offset]) << 8) | data[offset + 1];
            offset += 2;
            if (offset + cs_length > size || cs_length % 2 != 0) {
                return dp::result::err(dp::Error::invalid_argument("ClientHello cipher_suites truncated"));
            }
            for (dp::usize i = 0; i < cs_length; i += 2) {
                dp::u16 cs = (static_cast<dp::u16>(data[offset + i]) << 8) | data[offset + i + 1];
                msg.cipher_suites.push_back(cs);
            }
            offset += cs_length;

            // Compression methods
            if (offset >= size) {
                return dp::result::err(dp::Error::invalid_argument("ClientHello compression_methods length missing"));
            }
            dp::u8 comp_len = data[offset++];
            if (offset + comp_len > size) {
                return dp::result::err(dp::Error::invalid_argument("ClientHello compression_methods truncated"));
            }
            msg.legacy_compression_methods = dp::Vector<dp::u8>(data + offset, data + offset + comp_len);
            offset += comp_len;

            // Extensions (optional in theory, but required for TLS 1.3)
            if (offset < size) {
                auto ext_result = parse_extensions(data + offset, size - offset);
                if (ext_result.is_err()) {
                    return dp::result::err(ext_result.error());
                }
                msg.extensions = std::move(ext_result.value());
            }

            echo::debug("Parsed ClientHello: cipher_suites=", msg.cipher_suites.size(),
                        " extensions=", msg.extensions.size());

            return dp::result::ok(std::move(msg));
        }
    };

    // ServerHello message
    struct ServerHello {
        dp::u16 legacy_version = 0x0303; // TLS 1.2 for compatibility
        dp::Array<dp::u8, 32> random;
        dp::Vector<dp::u8> legacy_session_id_echo;
        dp::u16 cipher_suite;
        dp::u8 legacy_compression_method = 0x00;
        dp::Vector<Extension> extensions;

        // Generate random bytes
        void generate_random() {
            auto random_bytes = keylock::utils::Common::generate_random_bytes(32);
            std::copy(random_bytes.begin(), random_bytes.end(), random.begin());
        }

        // Serialize body (without handshake header)
        dp::Vector<dp::u8> serialize_body() const {
            dp::Vector<dp::u8> result;

            // Legacy version
            result.push_back(static_cast<dp::u8>((legacy_version >> 8) & 0xFF));
            result.push_back(static_cast<dp::u8>(legacy_version & 0xFF));

            // Random
            result.insert(result.end(), random.begin(), random.end());

            // Session ID echo
            result.push_back(static_cast<dp::u8>(legacy_session_id_echo.size()));
            result.insert(result.end(), legacy_session_id_echo.begin(), legacy_session_id_echo.end());

            // Cipher suite
            result.push_back(static_cast<dp::u8>((cipher_suite >> 8) & 0xFF));
            result.push_back(static_cast<dp::u8>(cipher_suite & 0xFF));

            // Compression method
            result.push_back(legacy_compression_method);

            // Extensions
            auto ext_bytes = serialize_extensions(extensions);
            result.insert(result.end(), ext_bytes.begin(), ext_bytes.end());

            return result;
        }

        // Serialize with handshake header
        dp::Vector<dp::u8> serialize() const {
            auto body = serialize_body();
            auto header = build_handshake_header(HandshakeType::ServerHello, static_cast<dp::u32>(body.size()));
            header.insert(header.end(), body.begin(), body.end());
            return header;
        }

        // Parse from bytes (without handshake header)
        static dp::Res<ServerHello> parse(const dp::u8 *data, dp::usize size) {
            echo::trace("ServerHello::parse: size=", size);

            if (size < 38) { // minimum size
                return dp::result::err(dp::Error::invalid_argument("ServerHello too short"));
            }

            ServerHello msg;
            dp::usize offset = 0;

            // Legacy version
            msg.legacy_version = (static_cast<dp::u16>(data[offset]) << 8) | data[offset + 1];
            offset += 2;

            // Random
            std::copy(data + offset, data + offset + 32, msg.random.begin());
            offset += 32;

            // Session ID echo
            dp::u8 session_id_len = data[offset++];
            if (offset + session_id_len > size) {
                return dp::result::err(dp::Error::invalid_argument("ServerHello session_id truncated"));
            }
            msg.legacy_session_id_echo = dp::Vector<dp::u8>(data + offset, data + offset + session_id_len);
            offset += session_id_len;

            // Cipher suite
            if (offset + 2 > size) {
                return dp::result::err(dp::Error::invalid_argument("ServerHello cipher_suite missing"));
            }
            msg.cipher_suite = (static_cast<dp::u16>(data[offset]) << 8) | data[offset + 1];
            offset += 2;

            // Compression method
            if (offset >= size) {
                return dp::result::err(dp::Error::invalid_argument("ServerHello compression_method missing"));
            }
            msg.legacy_compression_method = data[offset++];

            // Extensions
            if (offset < size) {
                auto ext_result = parse_extensions(data + offset, size - offset);
                if (ext_result.is_err()) {
                    return dp::result::err(ext_result.error());
                }
                msg.extensions = std::move(ext_result.value());
            }

            echo::debug("Parsed ServerHello: cipher_suite=", msg.cipher_suite, " extensions=", msg.extensions.size());

            return dp::result::ok(std::move(msg));
        }
    };

    // EncryptedExtensions message
    struct EncryptedExtensions {
        dp::Vector<Extension> extensions;

        dp::Vector<dp::u8> serialize_body() const { return serialize_extensions(extensions); }

        dp::Vector<dp::u8> serialize() const {
            auto body = serialize_body();
            auto header = build_handshake_header(HandshakeType::EncryptedExtensions, static_cast<dp::u32>(body.size()));
            header.insert(header.end(), body.begin(), body.end());
            return header;
        }

        static dp::Res<EncryptedExtensions> parse(const dp::u8 *data, dp::usize size) {
            echo::trace("EncryptedExtensions::parse: size=", size);

            EncryptedExtensions msg;
            if (size > 0) {
                auto ext_result = parse_extensions(data, size);
                if (ext_result.is_err()) {
                    return dp::result::err(ext_result.error());
                }
                msg.extensions = std::move(ext_result.value());
            }

            return dp::result::ok(std::move(msg));
        }
    };

    // Certificate Entry (for Certificate message)
    struct CertificateEntry {
        dp::Vector<dp::u8> cert_data; // DER-encoded X.509
        dp::Vector<Extension> extensions;

        dp::Vector<dp::u8> serialize() const {
            dp::Vector<dp::u8> result;

            // Certificate data length (3 bytes)
            result.push_back(static_cast<dp::u8>((cert_data.size() >> 16) & 0xFF));
            result.push_back(static_cast<dp::u8>((cert_data.size() >> 8) & 0xFF));
            result.push_back(static_cast<dp::u8>(cert_data.size() & 0xFF));

            // Certificate data
            result.insert(result.end(), cert_data.begin(), cert_data.end());

            // Extensions
            auto ext_bytes = serialize_extensions(extensions);
            result.insert(result.end(), ext_bytes.begin(), ext_bytes.end());

            return result;
        }

        static dp::Res<std::pair<CertificateEntry, dp::usize>> parse(const dp::u8 *data, dp::usize size) {
            if (size < 3) {
                return dp::result::err(dp::Error::invalid_argument("certificate entry too short"));
            }

            CertificateEntry entry;
            dp::usize offset = 0;

            // Certificate data length
            dp::u32 cert_len = (static_cast<dp::u32>(data[0]) << 16) | (static_cast<dp::u32>(data[1]) << 8) |
                               static_cast<dp::u32>(data[2]);
            offset += 3;

            if (offset + cert_len > size) {
                return dp::result::err(dp::Error::invalid_argument("certificate data truncated"));
            }

            entry.cert_data = dp::Vector<dp::u8>(data + offset, data + offset + cert_len);
            offset += cert_len;

            // Extensions
            if (offset + 2 <= size) {
                auto ext_result = parse_extensions(data + offset, size - offset);
                if (ext_result.is_err()) {
                    return dp::result::err(ext_result.error());
                }
                entry.extensions = std::move(ext_result.value());

                dp::u16 ext_len = (static_cast<dp::u16>(data[offset]) << 8) | data[offset + 1];
                offset += 2 + ext_len;
            }

            return dp::result::ok(std::make_pair(std::move(entry), offset));
        }
    };

    // Certificate message
    struct Certificate {
        dp::Vector<dp::u8> certificate_request_context; // Empty for server
        dp::Vector<CertificateEntry> certificate_list;

        dp::Vector<dp::u8> serialize_body() const {
            dp::Vector<dp::u8> result;

            // Certificate request context (1 byte length + data)
            result.push_back(static_cast<dp::u8>(certificate_request_context.size()));
            result.insert(result.end(), certificate_request_context.begin(), certificate_request_context.end());

            // Serialize certificate entries
            dp::Vector<dp::u8> entries_data;
            for (const auto &entry : certificate_list) {
                auto entry_bytes = entry.serialize();
                entries_data.insert(entries_data.end(), entry_bytes.begin(), entry_bytes.end());
            }

            // Certificate list length (3 bytes)
            result.push_back(static_cast<dp::u8>((entries_data.size() >> 16) & 0xFF));
            result.push_back(static_cast<dp::u8>((entries_data.size() >> 8) & 0xFF));
            result.push_back(static_cast<dp::u8>(entries_data.size() & 0xFF));

            // Certificate entries
            result.insert(result.end(), entries_data.begin(), entries_data.end());

            return result;
        }

        dp::Vector<dp::u8> serialize() const {
            auto body = serialize_body();
            auto header = build_handshake_header(HandshakeType::Certificate, static_cast<dp::u32>(body.size()));
            header.insert(header.end(), body.begin(), body.end());
            return header;
        }

        static dp::Res<Certificate> parse(const dp::u8 *data, dp::usize size) {
            echo::trace("Certificate::parse: size=", size);

            if (size < 4) {
                return dp::result::err(dp::Error::invalid_argument("Certificate message too short"));
            }

            Certificate msg;
            dp::usize offset = 0;

            // Certificate request context
            dp::u8 ctx_len = data[offset++];
            if (offset + ctx_len > size) {
                return dp::result::err(dp::Error::invalid_argument("certificate request context truncated"));
            }
            msg.certificate_request_context = dp::Vector<dp::u8>(data + offset, data + offset + ctx_len);
            offset += ctx_len;

            // Certificate list length
            if (offset + 3 > size) {
                return dp::result::err(dp::Error::invalid_argument("certificate list length missing"));
            }
            dp::u32 list_len = (static_cast<dp::u32>(data[offset]) << 16) |
                               (static_cast<dp::u32>(data[offset + 1]) << 8) | static_cast<dp::u32>(data[offset + 2]);
            offset += 3;

            if (offset + list_len > size) {
                return dp::result::err(dp::Error::invalid_argument("certificate list truncated"));
            }

            // Parse certificate entries
            dp::usize end_offset = offset + list_len;
            while (offset < end_offset) {
                auto entry_result = CertificateEntry::parse(data + offset, end_offset - offset);
                if (entry_result.is_err()) {
                    return dp::result::err(entry_result.error());
                }

                auto [entry, consumed] = entry_result.value();
                msg.certificate_list.push_back(std::move(entry));
                offset += consumed;
            }

            echo::debug("Parsed Certificate: ", msg.certificate_list.size(), " certificates");

            return dp::result::ok(std::move(msg));
        }
    };

    // CertificateVerify message
    struct CertificateVerify {
        SignatureScheme algorithm;
        dp::Vector<dp::u8> signature;

        dp::Vector<dp::u8> serialize_body() const {
            dp::Vector<dp::u8> result;

            // Algorithm (2 bytes)
            result.push_back(static_cast<dp::u8>((static_cast<dp::u16>(algorithm) >> 8) & 0xFF));
            result.push_back(static_cast<dp::u8>(static_cast<dp::u16>(algorithm) & 0xFF));

            // Signature length (2 bytes) + signature
            result.push_back(static_cast<dp::u8>((signature.size() >> 8) & 0xFF));
            result.push_back(static_cast<dp::u8>(signature.size() & 0xFF));
            result.insert(result.end(), signature.begin(), signature.end());

            return result;
        }

        dp::Vector<dp::u8> serialize() const {
            auto body = serialize_body();
            auto header = build_handshake_header(HandshakeType::CertificateVerify, static_cast<dp::u32>(body.size()));
            header.insert(header.end(), body.begin(), body.end());
            return header;
        }

        static dp::Res<CertificateVerify> parse(const dp::u8 *data, dp::usize size) {
            echo::trace("CertificateVerify::parse: size=", size);

            if (size < 4) {
                return dp::result::err(dp::Error::invalid_argument("CertificateVerify too short"));
            }

            CertificateVerify msg;

            // Algorithm
            msg.algorithm = static_cast<SignatureScheme>((static_cast<dp::u16>(data[0]) << 8) | data[1]);

            // Signature length
            dp::usize sig_len = (static_cast<dp::u16>(data[2]) << 8) | data[3];

            if (size < 4 + sig_len) {
                return dp::result::err(dp::Error::invalid_argument("CertificateVerify signature truncated"));
            }

            msg.signature = dp::Vector<dp::u8>(data + 4, data + 4 + sig_len);

            echo::debug("Parsed CertificateVerify: algorithm=", static_cast<dp::u16>(msg.algorithm),
                        " sig_len=", msg.signature.size());

            return dp::result::ok(std::move(msg));
        }

        // Build the content to be signed for CertificateVerify
        // Content = 64 spaces || context_string || 0x00 || transcript_hash
        static dp::Vector<dp::u8> build_signed_content(bool is_server, const dp::Vector<dp::u8> &transcript_hash) {
            dp::Vector<dp::u8> content;

            // 64 spaces
            for (int i = 0; i < 64; i++) {
                content.push_back(0x20);
            }

            // Context string
            const char *context = is_server ? "TLS 1.3, server CertificateVerify" : "TLS 1.3, client CertificateVerify";
            for (const char *p = context; *p; p++) {
                content.push_back(static_cast<dp::u8>(*p));
            }

            // Separator
            content.push_back(0x00);

            // Transcript hash
            content.insert(content.end(), transcript_hash.begin(), transcript_hash.end());

            return content;
        }
    };

    // Finished message
    struct Finished {
        dp::Vector<dp::u8> verify_data;

        dp::Vector<dp::u8> serialize_body() const { return verify_data; }

        dp::Vector<dp::u8> serialize() const {
            auto body = serialize_body();
            auto header = build_handshake_header(HandshakeType::Finished, static_cast<dp::u32>(body.size()));
            header.insert(header.end(), body.begin(), body.end());
            return header;
        }

        static dp::Res<Finished> parse(const dp::u8 *data, dp::usize size) {
            echo::trace("Finished::parse: size=", size);

            Finished msg;
            msg.verify_data = dp::Vector<dp::u8>(data, data + size);

            echo::debug("Parsed Finished: verify_data_len=", msg.verify_data.size());

            return dp::result::ok(std::move(msg));
        }

        // Compute verify_data for Finished message
        // verify_data = HMAC(finished_key, transcript_hash)
        static dp::Vector<dp::u8> compute_verify_data(const dp::Vector<dp::u8> &finished_key,
                                                      const dp::Vector<dp::u8> &transcript_hash) {
            std::vector<uint8_t> key_std(finished_key.begin(), finished_key.end());
            std::vector<uint8_t> data_std(transcript_hash.begin(), transcript_hash.end());

            auto result = keylock::hash::hmac(keylock::hash::Algorithm::SHA256, data_std, key_std);

            if (!result.success) {
                echo::error("Failed to compute verify_data: ", result.error_message.c_str());
                return {};
            }

            return dp::Vector<dp::u8>(result.data.begin(), result.data.end());
        }
    };

} // namespace netpipe::tls

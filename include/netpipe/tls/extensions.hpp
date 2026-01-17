#pragma once

#include <datapod/datapod.hpp>
#include <echo/echo.hpp>

namespace netpipe::tls {

    // TLS 1.3 Extension Types (RFC 8446)
    enum class ExtensionType : dp::u16 {
        ServerName = 0,
        SupportedGroups = 10,
        SignatureAlgorithms = 13,
        SupportedVersions = 43,
        Cookie = 44,
        PskKeyExchangeModes = 45,
        KeyShare = 51
    };

    // Named Groups (for key exchange)
    enum class NamedGroup : dp::u16 {
        // Elliptic Curve Groups
        Secp256r1 = 23,
        Secp384r1 = 24,
        Secp521r1 = 25,
        X25519 = 29,
        X448 = 30,

        // Finite Field Groups (FFDHE)
        Ffdhe2048 = 256,
        Ffdhe3072 = 257,
        Ffdhe4096 = 258
    };

    // Signature Algorithms
    enum class SignatureScheme : dp::u16 {
        // RSASSA-PKCS1-v1_5
        RsaPkcs1Sha256 = 0x0401,
        RsaPkcs1Sha384 = 0x0501,
        RsaPkcs1Sha512 = 0x0601,

        // ECDSA
        EcdsaSecp256r1Sha256 = 0x0403,
        EcdsaSecp384r1Sha384 = 0x0503,
        EcdsaSecp521r1Sha512 = 0x0603,

        // RSASSA-PSS (public key OID rsaEncryption)
        RsaPssRsaeSha256 = 0x0804,
        RsaPssRsaeSha384 = 0x0805,
        RsaPssRsaeSha512 = 0x0806,

        // EdDSA
        Ed25519 = 0x0807,
        Ed448 = 0x0808
    };

    // TLS Versions
    constexpr dp::u16 TLS_1_2 = 0x0303;
    constexpr dp::u16 TLS_1_3 = 0x0304;

    // Base Extension structure
    struct Extension {
        ExtensionType type;
        dp::Vector<dp::u8> data;

        dp::Vector<dp::u8> serialize() const {
            dp::Vector<dp::u8> result;
            result.reserve(4 + data.size());

            // Type (2 bytes)
            result.push_back(static_cast<dp::u8>((static_cast<dp::u16>(type) >> 8) & 0xFF));
            result.push_back(static_cast<dp::u8>(static_cast<dp::u16>(type) & 0xFF));

            // Length (2 bytes)
            result.push_back(static_cast<dp::u8>((data.size() >> 8) & 0xFF));
            result.push_back(static_cast<dp::u8>(data.size() & 0xFF));

            // Data
            result.insert(result.end(), data.begin(), data.end());

            return result;
        }

        static dp::Res<std::pair<Extension, dp::usize>> parse(const dp::u8 *data, dp::usize size) {
            if (size < 4) {
                return dp::result::err(dp::Error::invalid_argument("extension too short"));
            }

            Extension ext;
            ext.type = static_cast<ExtensionType>((static_cast<dp::u16>(data[0]) << 8) | data[1]);
            dp::u16 length = (static_cast<dp::u16>(data[2]) << 8) | data[3];

            if (size < static_cast<dp::usize>(4 + length)) {
                return dp::result::err(dp::Error::invalid_argument("extension data truncated"));
            }

            ext.data = dp::Vector<dp::u8>(data + 4, data + 4 + length);

            return dp::result::ok(std::make_pair(std::move(ext), static_cast<dp::usize>(4 + length)));
        }
    };

    // Key Share Entry (for key_share extension)
    struct KeyShareEntry {
        NamedGroup group;
        dp::Vector<dp::u8> key_exchange;

        dp::Vector<dp::u8> serialize() const {
            dp::Vector<dp::u8> result;
            result.reserve(4 + key_exchange.size());

            // Group (2 bytes)
            result.push_back(static_cast<dp::u8>((static_cast<dp::u16>(group) >> 8) & 0xFF));
            result.push_back(static_cast<dp::u8>(static_cast<dp::u16>(group) & 0xFF));

            // Key exchange length (2 bytes)
            result.push_back(static_cast<dp::u8>((key_exchange.size() >> 8) & 0xFF));
            result.push_back(static_cast<dp::u8>(key_exchange.size() & 0xFF));

            // Key exchange data
            result.insert(result.end(), key_exchange.begin(), key_exchange.end());

            return result;
        }

        static dp::Res<std::pair<KeyShareEntry, dp::usize>> parse(const dp::u8 *data, dp::usize size) {
            if (size < 4) {
                return dp::result::err(dp::Error::invalid_argument("key share entry too short"));
            }

            KeyShareEntry entry;
            entry.group = static_cast<NamedGroup>((static_cast<dp::u16>(data[0]) << 8) | data[1]);
            dp::u16 length = (static_cast<dp::u16>(data[2]) << 8) | data[3];

            if (size < static_cast<dp::usize>(4 + length)) {
                return dp::result::err(dp::Error::invalid_argument("key share data truncated"));
            }

            entry.key_exchange = dp::Vector<dp::u8>(data + 4, data + 4 + length);

            return dp::result::ok(std::make_pair(std::move(entry), static_cast<dp::usize>(4 + length)));
        }
    };

    // Supported Versions Extension
    struct SupportedVersionsExtension {
        dp::Vector<dp::u16> versions;

        // Serialize for ClientHello (list of versions)
        Extension serialize_client() const {
            dp::Vector<dp::u8> data;
            data.reserve(1 + versions.size() * 2);

            // Length of versions list (1 byte)
            data.push_back(static_cast<dp::u8>(versions.size() * 2));

            // Versions
            for (auto v : versions) {
                data.push_back(static_cast<dp::u8>((v >> 8) & 0xFF));
                data.push_back(static_cast<dp::u8>(v & 0xFF));
            }

            return Extension{ExtensionType::SupportedVersions, std::move(data)};
        }

        // Serialize for ServerHello (single selected version)
        static Extension serialize_server(dp::u16 selected_version) {
            dp::Vector<dp::u8> data;
            data.push_back(static_cast<dp::u8>((selected_version >> 8) & 0xFF));
            data.push_back(static_cast<dp::u8>(selected_version & 0xFF));

            return Extension{ExtensionType::SupportedVersions, std::move(data)};
        }

        // Parse from ClientHello
        static dp::Res<SupportedVersionsExtension> parse_client(const dp::Vector<dp::u8> &data) {
            if (data.empty()) {
                return dp::result::err(dp::Error::invalid_argument("supported_versions extension empty"));
            }

            dp::usize list_length = data[0];
            if (data.size() < 1 + list_length || list_length % 2 != 0) {
                return dp::result::err(dp::Error::invalid_argument("supported_versions extension malformed"));
            }

            SupportedVersionsExtension ext;
            for (dp::usize i = 1; i < 1 + list_length; i += 2) {
                dp::u16 version = (static_cast<dp::u16>(data[i]) << 8) | data[i + 1];
                ext.versions.push_back(version);
            }

            return dp::result::ok(std::move(ext));
        }

        // Parse from ServerHello (single version)
        static dp::Res<dp::u16> parse_server(const dp::Vector<dp::u8> &data) {
            if (data.size() != 2) {
                return dp::result::err(dp::Error::invalid_argument("supported_versions server extension malformed"));
            }

            return dp::result::ok(static_cast<dp::u16>((static_cast<dp::u16>(data[0]) << 8) | data[1]));
        }
    };

    // Key Share Extension (ClientHello)
    struct KeyShareClientHello {
        dp::Vector<KeyShareEntry> client_shares;

        Extension serialize() const {
            dp::Vector<dp::u8> entries_data;
            for (const auto &entry : client_shares) {
                auto entry_bytes = entry.serialize();
                entries_data.insert(entries_data.end(), entry_bytes.begin(), entry_bytes.end());
            }

            dp::Vector<dp::u8> data;
            data.reserve(2 + entries_data.size());

            // Length of client_shares (2 bytes)
            data.push_back(static_cast<dp::u8>((entries_data.size() >> 8) & 0xFF));
            data.push_back(static_cast<dp::u8>(entries_data.size() & 0xFF));

            // Entries
            data.insert(data.end(), entries_data.begin(), entries_data.end());

            return Extension{ExtensionType::KeyShare, std::move(data)};
        }

        static dp::Res<KeyShareClientHello> parse(const dp::Vector<dp::u8> &data) {
            if (data.size() < 2) {
                return dp::result::err(dp::Error::invalid_argument("key_share extension too short"));
            }

            dp::usize list_length = (static_cast<dp::u16>(data[0]) << 8) | data[1];
            if (data.size() < 2 + list_length) {
                return dp::result::err(dp::Error::invalid_argument("key_share extension data truncated"));
            }

            KeyShareClientHello ext;
            dp::usize offset = 2;
            while (offset < 2 + list_length) {
                auto result = KeyShareEntry::parse(data.data() + offset, data.size() - offset);
                if (result.is_err()) {
                    return dp::result::err(result.error());
                }

                auto [entry, consumed] = result.value();
                ext.client_shares.push_back(std::move(entry));
                offset += consumed;
            }

            return dp::result::ok(std::move(ext));
        }
    };

    // Key Share Extension (ServerHello)
    struct KeyShareServerHello {
        KeyShareEntry server_share;

        Extension serialize() const { return Extension{ExtensionType::KeyShare, server_share.serialize()}; }

        static dp::Res<KeyShareServerHello> parse(const dp::Vector<dp::u8> &data) {
            auto result = KeyShareEntry::parse(data.data(), data.size());
            if (result.is_err()) {
                return dp::result::err(result.error());
            }

            KeyShareServerHello ext;
            ext.server_share = std::move(result.value().first);
            return dp::result::ok(std::move(ext));
        }
    };

    // Supported Groups Extension
    struct SupportedGroupsExtension {
        dp::Vector<NamedGroup> groups;

        Extension serialize() const {
            dp::Vector<dp::u8> data;
            data.reserve(2 + groups.size() * 2);

            // Length (2 bytes)
            dp::u16 list_length = static_cast<dp::u16>(groups.size() * 2);
            data.push_back(static_cast<dp::u8>((list_length >> 8) & 0xFF));
            data.push_back(static_cast<dp::u8>(list_length & 0xFF));

            // Groups
            for (auto g : groups) {
                data.push_back(static_cast<dp::u8>((static_cast<dp::u16>(g) >> 8) & 0xFF));
                data.push_back(static_cast<dp::u8>(static_cast<dp::u16>(g) & 0xFF));
            }

            return Extension{ExtensionType::SupportedGroups, std::move(data)};
        }

        static dp::Res<SupportedGroupsExtension> parse(const dp::Vector<dp::u8> &data) {
            if (data.size() < 2) {
                return dp::result::err(dp::Error::invalid_argument("supported_groups extension too short"));
            }

            dp::usize list_length = (static_cast<dp::u16>(data[0]) << 8) | data[1];
            if (data.size() < 2 + list_length || list_length % 2 != 0) {
                return dp::result::err(dp::Error::invalid_argument("supported_groups extension malformed"));
            }

            SupportedGroupsExtension ext;
            for (dp::usize i = 2; i < 2 + list_length; i += 2) {
                auto group = static_cast<NamedGroup>((static_cast<dp::u16>(data[i]) << 8) | data[i + 1]);
                ext.groups.push_back(group);
            }

            return dp::result::ok(std::move(ext));
        }
    };

    // Signature Algorithms Extension
    struct SignatureAlgorithmsExtension {
        dp::Vector<SignatureScheme> algorithms;

        Extension serialize() const {
            dp::Vector<dp::u8> data;
            data.reserve(2 + algorithms.size() * 2);

            // Length (2 bytes)
            dp::u16 list_length = static_cast<dp::u16>(algorithms.size() * 2);
            data.push_back(static_cast<dp::u8>((list_length >> 8) & 0xFF));
            data.push_back(static_cast<dp::u8>(list_length & 0xFF));

            // Algorithms
            for (auto a : algorithms) {
                data.push_back(static_cast<dp::u8>((static_cast<dp::u16>(a) >> 8) & 0xFF));
                data.push_back(static_cast<dp::u8>(static_cast<dp::u16>(a) & 0xFF));
            }

            return Extension{ExtensionType::SignatureAlgorithms, std::move(data)};
        }

        static dp::Res<SignatureAlgorithmsExtension> parse(const dp::Vector<dp::u8> &data) {
            if (data.size() < 2) {
                return dp::result::err(dp::Error::invalid_argument("signature_algorithms extension too short"));
            }

            dp::usize list_length = (static_cast<dp::u16>(data[0]) << 8) | data[1];
            if (data.size() < 2 + list_length || list_length % 2 != 0) {
                return dp::result::err(dp::Error::invalid_argument("signature_algorithms extension malformed"));
            }

            SignatureAlgorithmsExtension ext;
            for (dp::usize i = 2; i < 2 + list_length; i += 2) {
                auto algo = static_cast<SignatureScheme>((static_cast<dp::u16>(data[i]) << 8) | data[i + 1]);
                ext.algorithms.push_back(algo);
            }

            return dp::result::ok(std::move(ext));
        }
    };

    // Helper to serialize a list of extensions
    inline dp::Vector<dp::u8> serialize_extensions(const dp::Vector<Extension> &extensions) {
        dp::Vector<dp::u8> ext_data;
        for (const auto &ext : extensions) {
            auto ext_bytes = ext.serialize();
            ext_data.insert(ext_data.end(), ext_bytes.begin(), ext_bytes.end());
        }

        dp::Vector<dp::u8> result;
        result.reserve(2 + ext_data.size());

        // Extensions length (2 bytes)
        result.push_back(static_cast<dp::u8>((ext_data.size() >> 8) & 0xFF));
        result.push_back(static_cast<dp::u8>(ext_data.size() & 0xFF));

        // Extensions data
        result.insert(result.end(), ext_data.begin(), ext_data.end());

        return result;
    }

    // Helper to parse a list of extensions
    inline dp::Res<dp::Vector<Extension>> parse_extensions(const dp::u8 *data, dp::usize size) {
        if (size < 2) {
            return dp::result::err(dp::Error::invalid_argument("extensions list too short"));
        }

        dp::usize ext_length = (static_cast<dp::u16>(data[0]) << 8) | data[1];
        if (size < 2 + ext_length) {
            return dp::result::err(dp::Error::invalid_argument("extensions data truncated"));
        }

        dp::Vector<Extension> extensions;
        dp::usize offset = 2;
        while (offset < 2 + ext_length) {
            auto result = Extension::parse(data + offset, size - offset);
            if (result.is_err()) {
                return dp::result::err(result.error());
            }

            auto [ext, consumed] = result.value();
            extensions.push_back(std::move(ext));
            offset += consumed;
        }

        return dp::result::ok(std::move(extensions));
    }

} // namespace netpipe::tls

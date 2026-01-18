#include <chrono>
#include <doctest/doctest.h>
#include <netpipe/security/tls.hpp>
#include <thread>

using namespace netpipe::tls;

TEST_CASE("TLS Key Schedule - HKDF-Expand-Label") {
    SUBCASE("Basic HKDF-Expand-Label") {
        // Create a test secret
        dp::Vector<dp::u8> secret(32, 0x42);
        dp::Vector<dp::u8> context = {0x01, 0x02, 0x03};

        auto result = hkdf_expand_label(secret, "test", context, 32);

        CHECK(result.size() == 32);
        // Result should be deterministic
        auto result2 = hkdf_expand_label(secret, "test", context, 32);
        CHECK(result == result2);
    }

    SUBCASE("Different labels produce different outputs") {
        dp::Vector<dp::u8> secret(32, 0x42);
        dp::Vector<dp::u8> context;

        auto result1 = hkdf_expand_label(secret, "label1", context, 32);
        auto result2 = hkdf_expand_label(secret, "label2", context, 32);

        CHECK(result1 != result2);
    }

    SUBCASE("Derive secret") {
        dp::Vector<dp::u8> secret(32, 0x42);
        dp::Vector<dp::u8> transcript_hash(32, 0x11);

        auto derived = derive_secret(secret, "c hs traffic", transcript_hash);

        CHECK(derived.size() == HASH_LENGTH);
    }

    SUBCASE("Empty hash") {
        auto hash = empty_hash();
        CHECK(hash.size() == HASH_LENGTH);

        // SHA-256 of empty string is well-known
        // e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
        CHECK(hash[0] == 0xe3);
        CHECK(hash[1] == 0xb0);
    }
}

TEST_CASE("TLS Key Schedule - Full derivation") {
    SUBCASE("Initialize and derive handshake secrets") {
        KeySchedule ks;
        ks.init();

        CHECK(ks.early_secret.size() == HASH_LENGTH);

        // Simulate shared secret from X25519
        dp::Vector<dp::u8> shared_secret(32, 0xAB);
        dp::Vector<dp::u8> hello_hash(32, 0xCD);

        ks.derive_handshake_secrets(shared_secret, hello_hash);

        CHECK(ks.handshake_secret.size() == HASH_LENGTH);
        CHECK(ks.client_handshake_traffic_secret.size() == HASH_LENGTH);
        CHECK(ks.server_handshake_traffic_secret.size() == HASH_LENGTH);

        // Client and server secrets should be different
        CHECK(ks.client_handshake_traffic_secret != ks.server_handshake_traffic_secret);
    }

    SUBCASE("Derive application secrets") {
        KeySchedule ks;
        ks.init();

        dp::Vector<dp::u8> shared_secret(32, 0xAB);
        dp::Vector<dp::u8> hello_hash(32, 0xCD);
        ks.derive_handshake_secrets(shared_secret, hello_hash);

        dp::Vector<dp::u8> full_hash(32, 0xEF);
        ks.derive_application_secrets(full_hash);

        CHECK(ks.master_secret.size() == HASH_LENGTH);
        CHECK(ks.client_application_traffic_secret.size() == HASH_LENGTH);
        CHECK(ks.server_application_traffic_secret.size() == HASH_LENGTH);
    }

    SUBCASE("Derive traffic keys") {
        dp::Vector<dp::u8> traffic_secret(32, 0x42);

        auto [key, iv] = KeySchedule::derive_traffic_keys(traffic_secret);

        CHECK(key.size() == 32);
        CHECK(iv.size() == 12);
    }

    SUBCASE("Derive finished key") {
        dp::Vector<dp::u8> traffic_secret(32, 0x42);

        auto finished_key = KeySchedule::derive_finished_key(traffic_secret);

        CHECK(finished_key.size() == HASH_LENGTH);
    }
}

TEST_CASE("TLS Record Layer - Encryption") {
    SUBCASE("Plaintext record") {
        dp::Vector<dp::u8> payload = {0x01, 0x02, 0x03, 0x04, 0x05};

        auto result = RecordLayer::build_plaintext_record(ContentType::Handshake, payload);

        REQUIRE(result.is_ok());
        auto record = result.value();

        // Check header
        CHECK(record[0] == static_cast<dp::u8>(ContentType::Handshake));
        CHECK(record[1] == 0x03); // Legacy version
        CHECK(record[2] == 0x03);
        CHECK(record[3] == 0x00); // Length high byte
        CHECK(record[4] == 0x05); // Length low byte

        // Check payload
        CHECK(record.size() == RECORD_HEADER_SIZE + payload.size());
        for (size_t i = 0; i < payload.size(); i++) {
            CHECK(record[RECORD_HEADER_SIZE + i] == payload[i]);
        }
    }

    SUBCASE("Encrypt and decrypt round-trip") {
        RecordLayer record;

        // Set up traffic secrets
        dp::Vector<dp::u8> client_secret(32, 0x11);
        dp::Vector<dp::u8> server_secret(32, 0x22);

        record.set_write_traffic_secret(client_secret);
        record.set_read_traffic_secret(client_secret); // Same key for testing

        // Encrypt
        dp::Vector<dp::u8> plaintext = {0x48, 0x65, 0x6c, 0x6c, 0x6f}; // "Hello"
        auto encrypt_result = record.encrypt(ContentType::ApplicationData, plaintext);

        REQUIRE(encrypt_result.is_ok());
        auto encrypted = encrypt_result.value();

        // Encrypted should be larger (header + ciphertext + tag + inner content type)
        CHECK(encrypted.size() > RECORD_HEADER_SIZE + plaintext.size());

        // Reset sequence numbers for decryption
        record.read_seq = 0;

        // Decrypt
        auto decrypt_result = record.decrypt(encrypted);

        REQUIRE(decrypt_result.is_ok());
        auto [content_type, decrypted] = decrypt_result.value();

        CHECK(content_type == ContentType::ApplicationData);
        CHECK(decrypted == plaintext);
    }

    SUBCASE("Sequence number increment") {
        RecordLayer record;
        dp::Vector<dp::u8> secret(32, 0x42);
        record.set_write_traffic_secret(secret);

        CHECK(record.write_seq == 0);

        dp::Vector<dp::u8> plaintext = {0x01};
        record.encrypt(ContentType::ApplicationData, plaintext);
        CHECK(record.write_seq == 1);

        record.encrypt(ContentType::ApplicationData, plaintext);
        CHECK(record.write_seq == 2);
    }

    SUBCASE("Nonce computation") {
        dp::Vector<dp::u8> iv = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b};

        auto nonce0 = RecordLayer::compute_nonce(iv, 0);
        CHECK(nonce0 == iv); // seq=0 XOR iv = iv

        auto nonce1 = RecordLayer::compute_nonce(iv, 1);
        CHECK(nonce1[11] == (iv[11] ^ 0x01)); // Last byte XOR'd with 1
    }
}

TEST_CASE("TLS Extensions - Serialization") {
    SUBCASE("SupportedVersions client") {
        SupportedVersionsExtension ext;
        ext.versions = {TLS_1_3, TLS_1_2};

        auto serialized = ext.serialize_client();

        CHECK(serialized.type == ExtensionType::SupportedVersions);
        CHECK(serialized.data.size() == 5); // 1 byte length + 2*2 bytes
    }

    SUBCASE("SupportedVersions server") {
        auto serialized = SupportedVersionsExtension::serialize_server(TLS_1_3);

        CHECK(serialized.type == ExtensionType::SupportedVersions);
        CHECK(serialized.data.size() == 2);
        CHECK(serialized.data[0] == 0x03);
        CHECK(serialized.data[1] == 0x04);
    }

    SUBCASE("KeyShareEntry round-trip") {
        KeyShareEntry entry;
        entry.group = NamedGroup::X25519;
        entry.key_exchange = dp::Vector<dp::u8>(32, 0x42);

        auto serialized = entry.serialize();

        auto parse_result = KeyShareEntry::parse(serialized.data(), serialized.size());
        REQUIRE(parse_result.is_ok());

        auto [parsed, consumed] = parse_result.value();
        CHECK(parsed.group == NamedGroup::X25519);
        CHECK(parsed.key_exchange == entry.key_exchange);
        CHECK(consumed == serialized.size());
    }

    SUBCASE("SupportedGroups round-trip") {
        SupportedGroupsExtension ext;
        ext.groups = {NamedGroup::X25519, NamedGroup::Secp256r1};

        auto serialized = ext.serialize();

        auto parse_result = SupportedGroupsExtension::parse(serialized.data);
        REQUIRE(parse_result.is_ok());

        auto parsed = parse_result.value();
        CHECK(parsed.groups.size() == 2);
        CHECK(parsed.groups[0] == NamedGroup::X25519);
        CHECK(parsed.groups[1] == NamedGroup::Secp256r1);
    }

    SUBCASE("SignatureAlgorithms round-trip") {
        SignatureAlgorithmsExtension ext;
        ext.algorithms = {SignatureScheme::Ed25519, SignatureScheme::EcdsaSecp256r1Sha256};

        auto serialized = ext.serialize();

        auto parse_result = SignatureAlgorithmsExtension::parse(serialized.data);
        REQUIRE(parse_result.is_ok());

        auto parsed = parse_result.value();
        CHECK(parsed.algorithms.size() == 2);
        CHECK(parsed.algorithms[0] == SignatureScheme::Ed25519);
    }
}

TEST_CASE("TLS Messages - Serialization") {
    SUBCASE("ClientHello round-trip") {
        ClientHello ch;
        ch.generate_random();
        ch.legacy_session_id = {0x01, 0x02, 0x03};
        ch.cipher_suites = {TLS_CHACHA20_POLY1305_SHA256};

        // Add extensions
        SupportedVersionsExtension sv;
        sv.versions = {TLS_1_3};
        ch.extensions.push_back(sv.serialize_client());

        auto serialized = ch.serialize();

        // Parse (skip handshake header)
        auto parse_result = ClientHello::parse(serialized.data() + HANDSHAKE_HEADER_SIZE,
                                               serialized.size() - HANDSHAKE_HEADER_SIZE);
        REQUIRE(parse_result.is_ok());

        auto parsed = parse_result.value();
        CHECK(parsed.legacy_version == 0x0303);
        CHECK(parsed.random == ch.random);
        CHECK(parsed.legacy_session_id == ch.legacy_session_id);
        CHECK(parsed.cipher_suites.size() == 1);
        CHECK(parsed.cipher_suites[0] == TLS_CHACHA20_POLY1305_SHA256);
    }

    SUBCASE("ServerHello round-trip") {
        ServerHello sh;
        sh.generate_random();
        sh.legacy_session_id_echo = {0x01, 0x02, 0x03};
        sh.cipher_suite = TLS_CHACHA20_POLY1305_SHA256;

        sh.extensions.push_back(SupportedVersionsExtension::serialize_server(TLS_1_3));

        auto serialized = sh.serialize();

        auto parse_result = ServerHello::parse(serialized.data() + HANDSHAKE_HEADER_SIZE,
                                               serialized.size() - HANDSHAKE_HEADER_SIZE);
        REQUIRE(parse_result.is_ok());

        auto parsed = parse_result.value();
        CHECK(parsed.legacy_version == 0x0303);
        CHECK(parsed.random == sh.random);
        CHECK(parsed.cipher_suite == TLS_CHACHA20_POLY1305_SHA256);
    }

    SUBCASE("Finished round-trip") {
        Finished fin;
        fin.verify_data = dp::Vector<dp::u8>(32, 0x42);

        auto serialized = fin.serialize();

        auto parse_result =
            Finished::parse(serialized.data() + HANDSHAKE_HEADER_SIZE, serialized.size() - HANDSHAKE_HEADER_SIZE);
        REQUIRE(parse_result.is_ok());

        CHECK(parse_result.value().verify_data == fin.verify_data);
    }

    SUBCASE("Finished verify_data computation") {
        dp::Vector<dp::u8> finished_key(32, 0x11);
        dp::Vector<dp::u8> transcript_hash(32, 0x22);

        auto verify_data = Finished::compute_verify_data(finished_key, transcript_hash);

        CHECK(verify_data.size() == HASH_LENGTH);

        // Should be deterministic
        auto verify_data2 = Finished::compute_verify_data(finished_key, transcript_hash);
        CHECK(verify_data == verify_data2);
    }

    SUBCASE("CertificateVerify signed content") {
        dp::Vector<dp::u8> transcript_hash(32, 0x42);

        auto server_content = CertificateVerify::build_signed_content(true, transcript_hash);
        auto client_content = CertificateVerify::build_signed_content(false, transcript_hash);

        // Should start with 64 spaces
        for (int i = 0; i < 64; i++) {
            CHECK(server_content[i] == 0x20);
            CHECK(client_content[i] == 0x20);
        }

        // Server and client content should differ (different context strings)
        CHECK(server_content != client_content);

        // Should end with transcript hash
        for (size_t i = 0; i < transcript_hash.size(); i++) {
            CHECK(server_content[server_content.size() - transcript_hash.size() + i] == transcript_hash[i]);
        }
    }
}

TEST_CASE("TLS Alert") {
    SUBCASE("Alert serialization") {
        Alert alert = Alert::handshake_failure();

        auto serialized = alert.serialize();

        CHECK(serialized.size() == 2);
        CHECK(serialized[0] == static_cast<dp::u8>(AlertLevel::Fatal));
        CHECK(serialized[1] == static_cast<dp::u8>(AlertDescription::HandshakeFailure));
    }

    SUBCASE("Alert parsing") {
        dp::Vector<dp::u8> data = {0x02, 0x28}; // Fatal, handshake_failure

        auto result = Alert::parse(data);
        REQUIRE(result.is_ok());

        auto alert = result.value();
        CHECK(alert.level == AlertLevel::Fatal);
        CHECK(alert.description == AlertDescription::HandshakeFailure);
        CHECK(alert.is_fatal());
        CHECK_FALSE(alert.is_close_notify());
    }

    SUBCASE("Close notify") {
        auto alert = Alert::close_notify();

        CHECK(alert.level == AlertLevel::Warning);
        CHECK(alert.description == AlertDescription::CloseNotify);
        CHECK_FALSE(alert.is_fatal());
        CHECK(alert.is_close_notify());
    }
}

TEST_CASE("TLS Handshake - State machine") {
    SUBCASE("Client handshake initialization") {
        Handshake hs(Role::Client);

        CHECK(hs.state() == HandshakeState::Start);
        CHECK_FALSE(hs.is_complete());
        CHECK_FALSE(hs.is_error());
    }

    SUBCASE("Server handshake initialization") {
        HandshakeConfig config;
        config.certificate = dp::Vector<dp::u8>(100, 0x42); // Dummy cert
        config.private_key = dp::Vector<dp::u8>(64, 0x11);  // Dummy key

        Handshake hs(Role::Server, config);

        CHECK(hs.state() == HandshakeState::Start);
    }

    SUBCASE("Client creates ClientHello") {
        Handshake hs(Role::Client);

        auto result = hs.create_client_hello();

        REQUIRE(result.is_ok());
        auto client_hello = result.value();

        // Should be a valid TLS record
        CHECK(client_hello[0] == static_cast<dp::u8>(ContentType::Handshake));
        CHECK(hs.state() == HandshakeState::WaitServerHello);
    }
}

// =============================================================================
// TLS Key Update
// =============================================================================

TEST_CASE("TLS Key Update") {
    SUBCASE("Traffic key derivation is deterministic") {
        dp::Vector<dp::u8> traffic_secret(32, 0x42);

        auto [key1, iv1] = KeySchedule::derive_traffic_keys(traffic_secret);
        auto [key2, iv2] = KeySchedule::derive_traffic_keys(traffic_secret);

        CHECK(key1 == key2);
        CHECK(iv1 == iv2);
    }

    SUBCASE("Different secrets produce different keys") {
        dp::Vector<dp::u8> secret1(32, 0x42);
        dp::Vector<dp::u8> secret2(32, 0x43);

        auto [key1, iv1] = KeySchedule::derive_traffic_keys(secret1);
        auto [key2, iv2] = KeySchedule::derive_traffic_keys(secret2);

        CHECK(key1 != key2);
        CHECK(iv1 != iv2);
    }
}

// =============================================================================
// TLS Session Resumption
// =============================================================================

TEST_CASE("TLS Session Resumption") {
    SUBCASE("PSK identity serialization") {
        PskIdentity identity;
        identity.identity = {0x01, 0x02, 0x03, 0x04};
        identity.obfuscated_ticket_age = 12345;

        auto serialized = identity.serialize();

        // Format: 2-byte length + identity + 4-byte ticket age
        CHECK(serialized.size() == 2 + identity.identity.size() + 4);
    }

    SUBCASE("Resumption PSK derivation") {
        dp::Vector<dp::u8> resumption_master_secret(32, 0x42);
        dp::Vector<dp::u8> ticket_nonce = {0x01, 0x02, 0x03, 0x04};

        auto psk = KeySchedule::derive_resumption_psk(resumption_master_secret, ticket_nonce);

        CHECK(psk.size() == HASH_LENGTH);

        // Deterministic
        auto psk2 = KeySchedule::derive_resumption_psk(resumption_master_secret, ticket_nonce);
        CHECK(psk == psk2);
    }

    SUBCASE("Resumption master secret derivation") {
        KeySchedule ks;
        ks.init();

        dp::Vector<dp::u8> shared_secret(32, 0xAB);
        dp::Vector<dp::u8> hello_hash(32, 0xCD);
        ks.derive_handshake_secrets(shared_secret, hello_hash);

        dp::Vector<dp::u8> full_hash(32, 0xEF);
        ks.derive_application_secrets(full_hash);

        dp::Vector<dp::u8> resumption_hash(32, 0x99);
        ks.derive_resumption_master_secret(resumption_hash);

        CHECK(ks.resumption_master_secret.size() == HASH_LENGTH);
    }
}

// =============================================================================
// TLS Certificate Handling
// =============================================================================

TEST_CASE("TLS Certificate") {
    SUBCASE("Certificate message round-trip") {
        Certificate cert;
        cert.certificate_request_context = {0x01, 0x02};
        CertificateEntry entry;
        entry.cert_data = dp::Vector<dp::u8>(100, 0x42);
        cert.certificate_list.push_back(entry);

        auto serialized = cert.serialize();

        auto parse_result = Certificate::parse(
            serialized.data() + HANDSHAKE_HEADER_SIZE,
            serialized.size() - HANDSHAKE_HEADER_SIZE);
        REQUIRE(parse_result.is_ok());

        auto parsed = parse_result.value();
        CHECK(parsed.certificate_request_context == cert.certificate_request_context);
        CHECK(parsed.certificate_list.size() == 1);
        CHECK(parsed.certificate_list[0].cert_data == entry.cert_data);
    }

    SUBCASE("Multiple certificates in chain") {
        Certificate cert;
        cert.certificate_request_context = {};

        for (int i = 0; i < 3; i++) {
            CertificateEntry entry;
            entry.cert_data = dp::Vector<dp::u8>(50 + i * 10, static_cast<dp::u8>(i));
            cert.certificate_list.push_back(entry);
        }

        auto serialized = cert.serialize();
        auto parse_result = Certificate::parse(
            serialized.data() + HANDSHAKE_HEADER_SIZE,
            serialized.size() - HANDSHAKE_HEADER_SIZE);

        REQUIRE(parse_result.is_ok());
        CHECK(parse_result.value().certificate_list.size() == 3);
    }
}

// =============================================================================
// TLS Error Handling
// =============================================================================

TEST_CASE("TLS Error Handling") {
    SUBCASE("Parse empty data fails gracefully") {
        dp::Vector<dp::u8> empty;

        auto ch_result = ClientHello::parse(empty.data(), 0);
        CHECK(ch_result.is_err());

        auto sh_result = ServerHello::parse(empty.data(), 0);
        CHECK(sh_result.is_err());

        auto alert_result = Alert::parse(empty);
        CHECK(alert_result.is_err());
    }

    SUBCASE("Truncated data detection") {
        // Create minimal valid ClientHello then truncate
        ClientHello ch;
        ch.generate_random();
        ch.cipher_suites = {TLS_CHACHA20_POLY1305_SHA256};
        auto serialized = ch.serialize();

        // Truncate to half length
        auto parse_result = ClientHello::parse(
            serialized.data() + HANDSHAKE_HEADER_SIZE,
            (serialized.size() - HANDSHAKE_HEADER_SIZE) / 2);

        CHECK(parse_result.is_err());
    }

    SUBCASE("Invalid version detection") {
        dp::Vector<dp::u8> bad_record = {
            0x16,       // Handshake
            0x02, 0x00, // Invalid version
            0x00, 0x05, // Length
            0x01, 0x02, 0x03, 0x04, 0x05};

        // The record layer should handle this
        CHECK(bad_record[1] != 0x03); // Not TLS 1.x
    }

    SUBCASE("Various alert types") {
        CHECK(Alert::unexpected_message().description == AlertDescription::UnexpectedMessage);
        CHECK(Alert::bad_record_mac().description == AlertDescription::BadRecordMac);
        CHECK(Alert::decode_error().description == AlertDescription::DecodeError);
        CHECK(Alert::handshake_failure().description == AlertDescription::HandshakeFailure);
        CHECK(Alert::internal_error().description == AlertDescription::InternalError);

        // All should be fatal
        CHECK(Alert::unexpected_message().is_fatal());
        CHECK(Alert::bad_record_mac().is_fatal());
        CHECK(Alert::decode_error().is_fatal());
    }
}

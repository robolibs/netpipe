#pragma once

#include <datapod/datapod.hpp>
#include <echo/echo.hpp>
#include <netpipe/stream.hpp>
#include <netpipe/tls/handshake.hpp>
#include <netpipe/tls/record.hpp>

namespace netpipe::tls {

    // TLS Session configuration
    struct SessionConfig {
        // Our certificate (DER-encoded) - required for server
        dp::Vector<dp::u8> certificate;

        // Our Ed25519 private key - required for server
        dp::Vector<dp::u8> private_key;

        // Server name (for SNI, client only)
        dp::String server_name;

        // Skip certificate verification (for testing only!)
        bool skip_cert_verification = false;
    };

    // TLS Session
    // Main API for TLS encryption over a stream
    class Session {
      public:
        explicit Session(const SessionConfig &config = {}) : config_(config), established_(false) {
            echo::trace("TLS Session created");
        }

        // Perform TLS handshake as client
        // stream: underlying transport (must already be connected)
        dp::Res<void> handshake_client(Stream &stream) {
            echo::info("Starting TLS client handshake");

            HandshakeConfig hs_config;
            hs_config.certificate = config_.certificate;
            hs_config.private_key = config_.private_key;
            hs_config.server_name = config_.server_name;
            hs_config.skip_cert_verification = config_.skip_cert_verification;

            handshake_ = std::make_unique<Handshake>(Role::Client, hs_config);

            // Send ClientHello
            auto ch_result = handshake_->create_client_hello();
            if (ch_result.is_err()) {
                echo::error("Failed to create ClientHello: ", ch_result.error().message.c_str());
                return dp::result::err(ch_result.error());
            }

            auto send_result = send_record(stream, ch_result.value());
            if (send_result.is_err()) {
                return dp::result::err(send_result.error());
            }

            // Receive all server messages (may come as one concatenated buffer)
            auto server_data = recv_record(stream);
            if (server_data.is_err()) {
                return dp::result::err(server_data.error());
            }

            // Split into individual TLS records
            auto records = split_records(server_data.value());
            if (records.empty()) {
                return dp::result::err(dp::Error::invalid_argument("no TLS records received"));
            }

            echo::debug("Received ", records.size(), " TLS records from server");

            // Process first record as ServerHello
            auto sh_data = extract_handshake_from_record(records[0]);
            if (sh_data.is_err()) {
                return dp::result::err(sh_data.error());
            }

            auto process_result = handshake_->process_server_hello(sh_data.value());
            if (process_result.is_err()) {
                echo::error("Failed to process ServerHello: ", process_result.error().message.c_str());
                return dp::result::err(process_result.error());
            }

            // Process remaining records (encrypted handshake messages)
            for (size_t i = 1; i < records.size(); i++) {
                auto &record = records[i];

                // Check for ChangeCipherSpec (ignore it)
                if (!record.empty() && record[0] == static_cast<dp::u8>(ContentType::ChangeCipherSpec)) {
                    echo::trace("Ignoring ChangeCipherSpec record");
                    continue;
                }

                auto proc_result = handshake_->process_encrypted_handshake(record);
                if (proc_result.is_err()) {
                    echo::error("Failed to process handshake message: ", proc_result.error().message.c_str());
                    return dp::result::err(proc_result.error());
                }
            }

            // Send client Finished
            auto fin_result = handshake_->create_client_finished();
            if (fin_result.is_err()) {
                echo::error("Failed to create client Finished: ", fin_result.error().message.c_str());
                return dp::result::err(fin_result.error());
            }

            send_result = send_record(stream, fin_result.value());
            if (send_result.is_err()) {
                return dp::result::err(send_result.error());
            }

            if (handshake_->is_complete()) {
                established_ = true;
                echo::info("TLS client handshake complete");
                return dp::result::ok();
            } else {
                echo::error("TLS handshake failed");
                return dp::result::err(dp::Error::io_error("TLS handshake failed"));
            }
        }

        // Perform TLS handshake as server
        // stream: underlying transport (accepted connection)
        dp::Res<void> handshake_server(Stream &stream) {
            echo::info("Starting TLS server handshake");

            if (config_.certificate.empty() || config_.private_key.empty()) {
                return dp::result::err(dp::Error::invalid_argument("server requires certificate and private key"));
            }

            HandshakeConfig hs_config;
            hs_config.certificate = config_.certificate;
            hs_config.private_key = config_.private_key;
            hs_config.skip_cert_verification = config_.skip_cert_verification;

            handshake_ = std::make_unique<Handshake>(Role::Server, hs_config);

            // Receive ClientHello
            auto ch_record = recv_record(stream);
            if (ch_record.is_err()) {
                return dp::result::err(ch_record.error());
            }

            // Extract handshake message from record
            auto ch_data = extract_handshake_from_record(ch_record.value());
            if (ch_data.is_err()) {
                return dp::result::err(ch_data.error());
            }

            // Process ClientHello and generate all server messages
            auto response_result = handshake_->process_client_hello_and_respond(ch_data.value());
            if (response_result.is_err()) {
                echo::error("Failed to process ClientHello: ", response_result.error().message.c_str());
                return dp::result::err(response_result.error());
            }

            // Send all server messages (ServerHello, EncryptedExtensions, Certificate, CertificateVerify, Finished)
            auto send_result = send_raw(stream, response_result.value());
            if (send_result.is_err()) {
                return dp::result::err(send_result.error());
            }

            // Receive client Finished (might have ChangeCipherSpec first)
            while (!handshake_->is_complete() && !handshake_->is_error()) {
                auto record = recv_record(stream);
                if (record.is_err()) {
                    return dp::result::err(record.error());
                }

                // Split in case multiple records arrived
                auto records = split_records(record.value());
                for (auto &rec : records) {
                    // Check for ChangeCipherSpec (ignore it)
                    if (!rec.empty() && rec[0] == static_cast<dp::u8>(ContentType::ChangeCipherSpec)) {
                        echo::trace("Ignoring ChangeCipherSpec record");
                        continue;
                    }

                    auto proc_result = handshake_->process_client_finished(rec);
                    if (proc_result.is_err()) {
                        echo::error("Failed to process client Finished: ", proc_result.error().message.c_str());
                        return dp::result::err(proc_result.error());
                    }
                }
            }

            if (handshake_->is_complete()) {
                established_ = true;
                echo::info("TLS server handshake complete");
                return dp::result::ok();
            } else {
                echo::error("TLS handshake failed");
                return dp::result::err(dp::Error::io_error("TLS handshake failed"));
            }
        }

        // Check if session is established
        bool is_established() const { return established_; }

        // Send encrypted application data
        dp::Res<void> send(Stream &stream, const dp::Vector<dp::u8> &data) {
            if (!established_) {
                return dp::result::err(dp::Error::invalid_argument("TLS session not established"));
            }

            echo::trace("Sending ", data.size(), " bytes of application data");

            // Encrypt the data
            auto record_result = handshake_->record_layer().encrypt(ContentType::ApplicationData, data);
            if (record_result.is_err()) {
                return dp::result::err(record_result.error());
            }

            return send_record(stream, record_result.value());
        }

        // Receive and decrypt application data
        dp::Res<dp::Vector<dp::u8>> recv(Stream &stream) {
            if (!established_) {
                return dp::result::err(dp::Error::invalid_argument("TLS session not established"));
            }

            echo::trace("Receiving application data");

            // Receive a record
            auto record = recv_record(stream);
            if (record.is_err()) {
                return dp::result::err(record.error());
            }

            // Decrypt the record
            auto decrypt_result = handshake_->record_layer().decrypt(record.value());
            if (decrypt_result.is_err()) {
                return dp::result::err(decrypt_result.error());
            }

            auto [content_type, plaintext] = decrypt_result.value();

            if (content_type == ContentType::Alert) {
                echo::warn("Received TLS alert");
                return dp::result::err(dp::Error::io_error("TLS alert received"));
            }

            if (content_type != ContentType::ApplicationData) {
                return dp::result::err(dp::Error::invalid_argument("expected application data"));
            }

            echo::trace("Received ", plaintext.size(), " bytes of application data");
            return dp::result::ok(std::move(plaintext));
        }

        // Encrypt data without sending (for manual control)
        dp::Res<dp::Vector<dp::u8>> encrypt(const dp::Vector<dp::u8> &plaintext) {
            if (!established_) {
                return dp::result::err(dp::Error::invalid_argument("TLS session not established"));
            }

            return handshake_->record_layer().encrypt(ContentType::ApplicationData, plaintext);
        }

        // Decrypt data without receiving (for manual control)
        dp::Res<dp::Vector<dp::u8>> decrypt(const dp::Vector<dp::u8> &record) {
            if (!established_) {
                return dp::result::err(dp::Error::invalid_argument("TLS session not established"));
            }

            auto result = handshake_->record_layer().decrypt(record);
            if (result.is_err()) {
                return dp::result::err(result.error());
            }

            auto [content_type, plaintext] = result.value();
            return dp::result::ok(std::move(plaintext));
        }

        // Close the TLS session (send close_notify alert)
        dp::Res<void> close(Stream &stream) {
            if (!established_) {
                return dp::result::ok(); // Already closed or never established
            }

            echo::trace("Closing TLS session");

            // Build close_notify alert
            dp::Vector<dp::u8> alert = {
                static_cast<dp::u8>(1), // Warning level
                static_cast<dp::u8>(0)  // close_notify
            };

            auto record_result = handshake_->record_layer().encrypt(ContentType::Alert, alert);
            if (record_result.is_err()) {
                echo::warn("Failed to encrypt close_notify alert");
            } else {
                send_record(stream, record_result.value()); // Ignore errors
            }

            established_ = false;
            return dp::result::ok();
        }

      private:
        SessionConfig config_;
        std::unique_ptr<Handshake> handshake_;
        bool established_;

        // Split concatenated TLS records into individual records
        static dp::Vector<dp::Vector<dp::u8>> split_records(const dp::Vector<dp::u8> &data) {
            dp::Vector<dp::Vector<dp::u8>> records;
            dp::usize offset = 0;

            while (offset + RECORD_HEADER_SIZE <= data.size()) {
                // Parse record header
                dp::u16 length = (static_cast<dp::u16>(data[offset + 3]) << 8) | data[offset + 4];
                dp::usize record_size = RECORD_HEADER_SIZE + length;

                if (offset + record_size > data.size()) {
                    echo::warn("Incomplete TLS record at offset ", offset);
                    break;
                }

                // Extract this record
                records.push_back(dp::Vector<dp::u8>(data.begin() + offset, data.begin() + offset + record_size));
                offset += record_size;
            }

            return records;
        }

        // Send raw bytes over the stream (using netpipe's length-prefix framing)
        dp::Res<void> send_raw(Stream &stream, const dp::Vector<dp::u8> &data) {
            Message msg(data.begin(), data.end());
            return stream.send(msg);
        }

        // Send a TLS record over the stream
        dp::Res<void> send_record(Stream &stream, const dp::Vector<dp::u8> &record) { return send_raw(stream, record); }

        // Receive a TLS record from the stream
        dp::Res<dp::Vector<dp::u8>> recv_record(Stream &stream) {
            auto msg_result = stream.recv();
            if (msg_result.is_err()) {
                return dp::result::err(msg_result.error());
            }

            auto &msg = msg_result.value();
            return dp::result::ok(dp::Vector<dp::u8>(msg.begin(), msg.end()));
        }

        // Extract handshake message from a plaintext TLS record
        dp::Res<dp::Vector<dp::u8>> extract_handshake_from_record(const dp::Vector<dp::u8> &record) {
            if (record.size() < RECORD_HEADER_SIZE) {
                return dp::result::err(dp::Error::invalid_argument("record too short"));
            }

            auto type = static_cast<ContentType>(record[0]);
            if (type != ContentType::Handshake) {
                return dp::result::err(dp::Error::invalid_argument("expected handshake record"));
            }

            dp::u16 length = (static_cast<dp::u16>(record[3]) << 8) | record[4];

            if (record.size() < RECORD_HEADER_SIZE + length) {
                return dp::result::err(dp::Error::invalid_argument("record data truncated"));
            }

            return dp::result::ok(
                dp::Vector<dp::u8>(record.begin() + RECORD_HEADER_SIZE, record.begin() + RECORD_HEADER_SIZE + length));
        }
    };

} // namespace netpipe::tls

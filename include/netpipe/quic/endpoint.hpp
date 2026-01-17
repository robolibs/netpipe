#pragma once

#include <datapod/datapod.hpp>
#include <echo/echo.hpp>
#include <netpipe/datagram/udp.hpp>
#include <netpipe/quic/connection.hpp>
#include <netpipe/quic/tls_adapter.hpp>
#include <netpipe/stream.hpp>

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <poll.h>
#include <thread>

namespace netpipe::quic {

    // QUIC configuration
    struct QuicConfig {
        // Certificate (DER-encoded) - required for server
        dp::Vector<dp::u8> certificate;

        // Private key (Ed25519) - required for server
        dp::Vector<dp::u8> private_key;

        // Server name (SNI) - for client
        dp::String server_name;

        // Skip certificate verification (testing only!)
        bool skip_cert_verification = false;

        // Transport parameters
        TransportParameters transport_params;

        // Timeouts
        std::chrono::milliseconds idle_timeout{30000};
        std::chrono::milliseconds handshake_timeout{10000};

        // Session ticket for 0-RTT (client only)
        dp::Optional<tls::SessionTicket> session_ticket;

        // Enable 0-RTT early data
        bool enable_early_data = false;

        // Max early data size (server only)
        dp::u32 max_early_data_size = 16384;
    };

    // Forward declaration
    class QuicStream;

    // A handle to a single QUIC stream within a connection
    class QuicStreamHandle {
      public:
        QuicStreamHandle(std::shared_ptr<Connection> conn, dp::u64 stream_id) : conn_(conn), stream_id_(stream_id) {}

        dp::u64 id() const { return stream_id_; }

        dp::Res<void> send(const Message &msg) {
            if (!conn_ || !conn_->is_connected()) {
                return dp::result::err(dp::Error::io_error("not connected"));
            }
            return conn_->stream_write(stream_id_, msg);
        }

        dp::Res<Message> recv() {
            if (!conn_ || !conn_->is_connected()) {
                return dp::result::err(dp::Error::io_error("not connected"));
            }
            return conn_->stream_read(stream_id_);
        }

        dp::Res<void> finish() {
            if (!conn_) {
                return dp::result::err(dp::Error::io_error("not connected"));
            }
            return conn_->stream_finish(stream_id_);
        }

        bool is_finished() const {
            if (!conn_)
                return true;
            auto *stream = conn_->streams().get(stream_id_);
            if (!stream)
                return true;
            // Stream is finished when we've received FIN and read all data
            return stream->recv_buffer().all_read();
        }

      private:
        std::shared_ptr<Connection> conn_;
        dp::u64 stream_id_;
    };

    // QUIC Stream - implements netpipe::Stream interface
    // Provides TCP-like API over QUIC protocol
    class QuicStream : public Stream {
      private:
        QuicConfig config_;
        UdpDatagram udp_;

        // Server state
        bool is_server_ = false;
        bool listening_ = false;
        std::map<dp::String, std::shared_ptr<Connection>> connections_;
        std::map<dp::String, std::unique_ptr<QuicTlsAdapter>> server_tls_adapters_;
        std::map<dp::String, dp::Vector<dp::Vector<dp::u8>>> pending_server_datagrams_;
        std::mutex connections_mutex_;

        // Client state
        std::shared_ptr<Connection> client_conn_;
        dp::u64 default_stream_id_ = 0;
        bool has_default_stream_ = false;

        // TLS adapter for handshake (client-side)
        std::unique_ptr<QuicTlsAdapter> tls_;

        // Remote endpoint (for client)
        UdpEndpoint remote_endpoint_;

        // Receive timeout
        dp::u32 recv_timeout_ms_ = 0;

        // Helper to make endpoint key
        static dp::String endpoint_key(const UdpEndpoint &ep) { return ep.host + ":" + std::to_string(ep.port); }

        // Process incoming UDP datagram
        dp::Res<void> process_datagram(const Message &data, const UdpEndpoint &from) {
            if (data.size() < 1) {
                return dp::result::err(dp::Error::invalid_argument("empty datagram"));
            }

            // Check if this is a long header (Initial/Handshake) or short header
            bool is_long = is_long_header(data[0]);

            if (is_server_) {
                // Server: route to connection or create new one
                dp::String key = endpoint_key(from);

                std::lock_guard<std::mutex> lock(connections_mutex_);
                auto it = connections_.find(key);

                if (it == connections_.end()) {
                    // New connection - must be Initial packet
                    if (!is_long) {
                        echo::warn("received short header from unknown client");
                        return dp::result::err(dp::Error::invalid_argument("unknown connection"));
                    }

                    // Parse to get destination CID
                    auto header_result = LongHeader::parse(data.data(), data.size());
                    if (header_result.is_err()) {
                        return dp::result::err(header_result.error());
                    }
                    auto [parsed_header, consumed] = header_result.value();

                    // Create new connection
                    auto conn = std::make_shared<Connection>(false); // server
                    conn->set_original_dcid(parsed_header.dest_cid);
                    conn->set_remote_cid(parsed_header.src_cid);
                    conn->set_local_cid(ConnectionId::generate());

                    // Set transport parameters
                    conn->set_local_params(config_.transport_params);

                    // Start handshake
                    auto start_result = conn->start_handshake();
                    if (start_result.is_err()) {
                        return dp::result::err(start_result.error());
                    }

                    // Create server TLS adapter
                    QuicTlsConfig tls_config;
                    tls_config.certificate = config_.certificate;
                    tls_config.private_key = config_.private_key;
                    tls_config.transport_params = config_.transport_params;
                    server_tls_adapters_[key] = std::make_unique<QuicTlsAdapter>(false, tls_config);

                    connections_[key] = conn;
                    echo::debug("new QUIC connection from ", from.to_string());
                }

                // Process packet with connection
                auto &conn = connections_[key];
                auto proc_result = conn->process_packet(data);
                if (proc_result.is_err()) {
                    return proc_result;
                }

                // Check for CRYPTO data and drive server TLS handshake
                auto tls_it = server_tls_adapters_.find(key);
                if (tls_it != server_tls_adapters_.end() && conn->has_pending_crypto()) {
                    auto &tls = tls_it->second;
                    auto crypto_data = conn->take_crypto_data();
                    echo::debug("Server processing CRYPTO data (", crypto_data.size(), " bytes)");

                    if (tls->state() == QuicTlsState::Start) {
                        // Process ClientHello and generate server response
                        auto resp_result = tls->process_client_hello_and_respond(crypto_data);
                        if (resp_result.is_err()) {
                            echo::error("Server TLS error: ", resp_result.error().message.c_str());
                            return dp::result::err(resp_result.error());
                        }

                        // Install handshake keys
                        if (tls->has_handshake_keys()) {
                            conn->set_handshake_keys(tls->handshake_send_keys(), tls->handshake_recv_keys());
                        }

                        // Send server response messages
                        auto &messages = resp_result.value();
                        if (!messages.empty()) {
                            // First message (ServerHello) goes in Initial packet
                            auto initial_result = conn->build_initial_packet(messages[0]);
                            if (initial_result.is_ok()) {
                                pending_server_datagrams_[key].push_back(initial_result.value());
                            }

                            // Remaining messages go in Handshake packets
                            for (size_t i = 1; i < messages.size(); i++) {
                                auto hs_result = conn->build_handshake_packet(messages[i]);
                                if (hs_result.is_ok()) {
                                    pending_server_datagrams_[key].push_back(hs_result.value());
                                }
                            }
                        }

                        echo::debug("Server TLS response prepared");
                    } else if (tls->state() == QuicTlsState::WaitClientFinished) {
                        // Process client Finished
                        auto fin_result = tls->process_client_finished(crypto_data);
                        if (fin_result.is_err()) {
                            echo::error("Server TLS Finished error: ", fin_result.error().message.c_str());
                            return dp::result::err(fin_result.error());
                        }

                        // Install application keys
                        if (tls->has_application_keys()) {
                            conn->set_application_keys(tls->application_send_keys(), tls->application_recv_keys());
                        }

                        // Apply transport parameters
                        conn->set_remote_params(tls->peer_transport_params());

                        // Mark handshake complete
                        conn->handshake_complete();
                        echo::info("Server QUIC handshake complete for ", key);
                    }
                }

                return dp::result::ok();
            } else {
                // Client: all packets go to our connection
                if (!client_conn_) {
                    return dp::result::err(dp::Error::invalid_argument("no client connection"));
                }
                return client_conn_->process_packet(data);
            }
        }

        // Perform handshake (blocking) - drives TLS 1.3 handshake over QUIC
        dp::Res<void> do_handshake() {
            auto deadline = std::chrono::steady_clock::now() + config_.handshake_timeout;

            // Step 1: Create ClientHello and send Initial packet
            auto client_hello_result = tls_->create_client_hello();
            if (client_hello_result.is_err()) {
                return dp::result::err(client_hello_result.error());
            }

            auto client_hello = client_hello_result.value();
            echo::debug("Created ClientHello (", client_hello.size(), " bytes)");

            // Build and send Initial packet with ClientHello
            auto initial_result = client_conn_->build_initial_packet(client_hello);
            if (initial_result.is_err()) {
                return dp::result::err(initial_result.error());
            }

            auto send_result = udp_.send_to(initial_result.value(), remote_endpoint_);
            if (send_result.is_err()) {
                return dp::result::err(send_result.error());
            }
            echo::debug("Sent Initial packet with ClientHello");

            bool sent_client_finished = false;

            // Step 2: Wait for ServerHello and process handshake messages
            while (!tls_->is_complete()) {
                if (std::chrono::steady_clock::now() > deadline) {
                    return dp::result::err(dp::Error::io_error("handshake timeout"));
                }

                if (tls_->is_error()) {
                    return dp::result::err(dp::Error::io_error("TLS handshake error"));
                }

                // Receive packet
                udp_.set_recv_timeout(100);
                auto recv_result = udp_.recv_from();
                if (recv_result.is_err()) {
                    continue; // Timeout, try again
                }

                auto &[data, from] = recv_result.value();

                // Process the packet through connection
                auto proc_result = client_conn_->process_packet(data);
                if (proc_result.is_err()) {
                    echo::warn("Failed to process packet: ", proc_result.error().message.c_str());
                    continue;
                }

                // Check if connection has CRYPTO data for us
                if (client_conn_->has_pending_crypto()) {
                    auto crypto_data = client_conn_->take_crypto_data();
                    echo::debug("Processing CRYPTO data (", crypto_data.size(), " bytes)");

                    // Feed to TLS adapter based on current level
                    auto level = tls_->current_level();
                    dp::Res<void> tls_result;

                    if (level == EncryptionLevel::Initial) {
                        tls_result = tls_->process_server_hello(crypto_data);

                        // After ServerHello, we have handshake keys
                        if (tls_result.is_ok() && tls_->has_handshake_keys()) {
                            auto send_keys = tls_->handshake_send_keys();
                            auto recv_keys = tls_->handshake_recv_keys();
                            client_conn_->set_handshake_keys(send_keys, recv_keys);
                            echo::debug("Installed handshake keys");
                        }
                    } else if (level == EncryptionLevel::Handshake) {
                        // Could be EncryptedExtensions, Certificate, CertificateVerify, or Finished
                        tls_result = tls_->process_handshake_message(crypto_data);

                        // Check if we need to send client Finished
                        if (tls_result.is_ok() && tls_->state() == QuicTlsState::Connected && !sent_client_finished) {
                            auto finished_result = tls_->create_client_finished();
                            if (finished_result.is_ok()) {
                                auto client_finished = finished_result.value();
                                echo::debug("Sending client Finished (", client_finished.size(), " bytes)");

                                auto hs_result = client_conn_->build_handshake_packet(client_finished);
                                if (hs_result.is_ok()) {
                                    udp_.send_to(hs_result.value(), remote_endpoint_);
                                    sent_client_finished = true;
                                }
                            }
                        }
                    }

                    if (tls_result.is_err()) {
                        echo::error("TLS processing failed: ", tls_result.error().message.c_str());
                        return dp::result::err(tls_result.error());
                    }

                    // Update connection with application keys if handshake complete
                    if (tls_->is_complete() && tls_->has_application_keys()) {
                        auto send_keys = tls_->application_send_keys();
                        auto recv_keys = tls_->application_recv_keys();
                        client_conn_->set_application_keys(send_keys, recv_keys);
                        echo::debug("Installed application keys");

                        // Apply peer transport parameters
                        client_conn_->set_remote_params(tls_->peer_transport_params());

                        // Mark connection as established
                        client_conn_->handshake_complete();
                    }
                }

                // Send any pending ACKs or retransmissions
                auto datagrams = client_conn_->get_datagrams_to_send();
                for (auto &dg : datagrams) {
                    udp_.send_to(dg, remote_endpoint_);
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            echo::info("QUIC handshake complete");
            return dp::result::ok();
        }

        // Create accepted stream wrapper
        std::unique_ptr<Stream> create_accepted_stream(std::shared_ptr<Connection> conn, const UdpEndpoint &remote) {
            // Create a new QuicStream for the accepted connection
            auto accepted = std::make_unique<QuicStream>(config_);
            accepted->client_conn_ = conn;
            accepted->remote_endpoint_ = remote;
            accepted->is_server_ = false;

            // Set up TLS and remote params
            TransportParameters params;
            params.initial_max_streams_bidi = 100;
            params.initial_max_streams_uni = 100;
            conn->set_remote_params(params);

            // Mark handshake complete (already done)
            conn->handshake_complete();

            return accepted;
        }

      public:
        explicit QuicStream(const QuicConfig &config = {}) : config_(config) { echo::trace("QuicStream constructed"); }

        ~QuicStream() override { close(); }

        // Server: start listening for QUIC connections
        dp::Res<void> listen(const TcpEndpoint &endpoint) override {
            echo::debug("QUIC listen on ", endpoint.to_string());

            // Bind UDP socket
            UdpEndpoint udp_ep{endpoint.host, endpoint.port};
            auto bind_result = udp_.bind(udp_ep);
            if (bind_result.is_err()) {
                return dp::result::err(bind_result.error());
            }

            is_server_ = true;
            listening_ = true;

            echo::info("QUIC server listening on ", endpoint.to_string());
            return dp::result::ok();
        }

        // Server: accept incoming connection
        dp::Res<std::unique_ptr<Stream>> accept() override {
            if (!is_server_ || !listening_) {
                return dp::result::err(dp::Error::invalid_argument("not listening"));
            }

            echo::debug("QUIC accepting connection...");

            // Wait for a connection to complete handshake
            while (true) {
                // Receive datagram
                auto recv_result = udp_.recv_from();
                if (recv_result.is_err()) {
                    if (recv_timeout_ms_ > 0) {
                        return dp::result::err(recv_result.error());
                    }
                    continue;
                }

                auto &[data, from] = recv_result.value();
                process_datagram(data, from);

                // Send any responses
                {
                    std::lock_guard<std::mutex> lock(connections_mutex_);
                    for (auto &[key, conn] : connections_) {
                        // Parse key to get endpoint
                        auto colon = key.find(':');
                        UdpEndpoint ep;
                        if (colon != dp::String::npos) {
                            ep = UdpEndpoint{dp::String(key.substr(0, colon)),
                                             static_cast<dp::u16>(std::stoi(key.substr(colon + 1).c_str()))};
                        } else {
                            continue;
                        }

                        // Send pending TLS response datagrams
                        auto pending_it = pending_server_datagrams_.find(key);
                        if (pending_it != pending_server_datagrams_.end()) {
                            for (auto &dg : pending_it->second) {
                                udp_.send_to(dg, ep);
                            }
                            pending_it->second.clear();
                        }

                        // Send any other pending datagrams (ACKs, etc.)
                        auto datagrams = conn->get_datagrams_to_send();
                        for (auto &dg : datagrams) {
                            udp_.send_to(dg, ep);
                        }

                        // Check if handshake complete
                        if (conn->is_connected()) {
                            echo::info("QUIC connection accepted from ", key);

                            // Remove from pending and return
                            auto accepted_conn = conn;
                            auto accepted_key = key;
                            connections_.erase(key);
                            server_tls_adapters_.erase(key);
                            pending_server_datagrams_.erase(key);

                            return dp::result::ok(create_accepted_stream(accepted_conn, ep));
                        }
                    }
                }
            }
        }

        // Client: connect to QUIC server
        dp::Res<void> connect(const TcpEndpoint &endpoint) override {
            echo::debug("QUIC connect to ", endpoint.to_string());

            remote_endpoint_ = UdpEndpoint{endpoint.host, endpoint.port};

            // Bind to ephemeral port
            UdpEndpoint local{"0.0.0.0", 0};
            auto bind_result = udp_.bind(local);
            if (bind_result.is_err()) {
                return dp::result::err(bind_result.error());
            }

            // Create client connection
            client_conn_ = std::make_shared<Connection>(true);
            auto dcid = ConnectionId::generate();
            client_conn_->set_original_dcid(dcid);
            client_conn_->set_remote_cid(dcid);
            client_conn_->set_local_cid(ConnectionId::generate());

            // Set transport parameters
            client_conn_->set_local_params(config_.transport_params);

            // Initialize TLS adapter
            QuicTlsConfig tls_config;
            tls_config.server_name = config_.server_name;
            tls_config.skip_cert_verification = config_.skip_cert_verification;
            tls_config.transport_params = config_.transport_params;
            tls_config.session_ticket = config_.session_ticket;
            tls_config.enable_early_data = config_.enable_early_data;
            tls_ = std::make_unique<QuicTlsAdapter>(true, tls_config);

            // Start handshake - set initial keys based on DCID
            auto start_result = client_conn_->start_handshake();
            if (start_result.is_err()) {
                return dp::result::err(start_result.error());
            }

            // Check if we can use 0-RTT
            if (tls_->can_send_early_data()) {
                echo::debug("0-RTT available, setting up early data keys");
            }

            // Perform TLS handshake
            auto handshake_result = do_handshake();
            if (handshake_result.is_err()) {
                client_conn_.reset();
                tls_.reset();
                return dp::result::err(handshake_result.error());
            }

            // Create default stream
            auto stream_result = client_conn_->create_stream(true);
            if (stream_result.is_err()) {
                return dp::result::err(stream_result.error());
            }
            default_stream_id_ = stream_result.value();
            has_default_stream_ = true;

            echo::info("QUIC connected to ", endpoint.to_string());
            return dp::result::ok();
        }

        // Send message on default stream
        dp::Res<void> send(const Message &msg) override {
            if (!is_connected()) {
                return dp::result::err(dp::Error::io_error("not connected"));
            }

            if (!has_default_stream_) {
                // Create default stream if needed
                auto stream_result = client_conn_->create_stream(true);
                if (stream_result.is_err()) {
                    return dp::result::err(stream_result.error());
                }
                default_stream_id_ = stream_result.value();
                has_default_stream_ = true;
            }

            // Write to stream
            auto write_result = client_conn_->stream_write(default_stream_id_, msg);
            if (write_result.is_err()) {
                return dp::result::err(write_result.error());
            }

            // Send datagram(s)
            auto datagrams = client_conn_->get_datagrams_to_send();
            for (auto &dg : datagrams) {
                auto send_result = udp_.send_to(dg, remote_endpoint_);
                if (send_result.is_err()) {
                    return dp::result::err(send_result.error());
                }
            }

            return dp::result::ok();
        }

        // Receive message from default stream
        dp::Res<Message> recv() override {
            if (!is_connected()) {
                return dp::result::err(dp::Error::io_error("not connected"));
            }

            // Set timeout
            if (recv_timeout_ms_ > 0) {
                udp_.set_recv_timeout(recv_timeout_ms_);
            }

            // Loop until we have data on our stream
            auto deadline = recv_timeout_ms_ > 0
                                ? std::chrono::steady_clock::now() + std::chrono::milliseconds(recv_timeout_ms_)
                                : std::chrono::steady_clock::time_point::max();

            while (true) {
                // Check for readable data
                auto read_result = client_conn_->stream_read(default_stream_id_);
                if (read_result.is_ok() && !read_result.value().empty()) {
                    return dp::result::ok(std::move(read_result.value()));
                }

                // Check timeout
                if (recv_timeout_ms_ > 0 && std::chrono::steady_clock::now() > deadline) {
                    return dp::result::err(dp::Error::io_error("receive timeout"));
                }

                // Receive more data
                auto recv_result = udp_.recv_from();
                if (recv_result.is_ok()) {
                    auto &[data, from] = recv_result.value();
                    process_datagram(data, from);
                } else if (recv_timeout_ms_ > 0) {
                    return dp::result::err(recv_result.error());
                }

                // Send any pending ACKs etc
                auto datagrams = client_conn_->get_datagrams_to_send();
                for (auto &dg : datagrams) {
                    udp_.send_to(dg, remote_endpoint_);
                }
            }
        }

        dp::Res<void> set_recv_timeout(dp::u32 timeout_ms) override {
            recv_timeout_ms_ = timeout_ms;
            return udp_.set_recv_timeout(timeout_ms);
        }

        void close() override {
            if (client_conn_) {
                client_conn_->close();
                client_conn_.reset();
            }

            {
                std::lock_guard<std::mutex> lock(connections_mutex_);
                for (auto &[key, conn] : connections_) {
                    conn->close();
                }
                connections_.clear();
            }

            udp_.close();
            listening_ = false;
            has_default_stream_ = false;

            echo::debug("QuicStream closed");
        }

        bool is_connected() const override { return client_conn_ && client_conn_->is_connected(); }

        // === QUIC-specific extensions ===

        // Open additional stream on the connection
        dp::Res<std::shared_ptr<QuicStreamHandle>> open_stream(bool bidirectional = true) {
            if (!is_connected()) {
                return dp::result::err(dp::Error::io_error("not connected"));
            }

            auto stream_result = client_conn_->create_stream(bidirectional);
            if (stream_result.is_err()) {
                return dp::result::err(stream_result.error());
            }

            return dp::result::ok(std::make_shared<QuicStreamHandle>(client_conn_, stream_result.value()));
        }

        // Get underlying connection
        std::shared_ptr<Connection> connection() { return client_conn_; }

        // Get config
        const QuicConfig &config() const { return config_; }

        // === 0-RTT / Session Resumption ===

        // Request a session ticket from server (call after handshake complete)
        // Returns the ticket if server sends one, empty otherwise
        dp::Res<dp::Optional<tls::SessionTicket>> request_session_ticket() {
            if (!is_connected() || !tls_) {
                return dp::result::err(dp::Error::io_error("not connected"));
            }

            // Wait for NewSessionTicket message from server (may arrive in post-handshake data)
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

            while (std::chrono::steady_clock::now() < deadline) {
                udp_.set_recv_timeout(100);
                auto recv_result = udp_.recv_from();
                if (recv_result.is_err()) {
                    continue;
                }

                auto &[data, from] = recv_result.value();

                // Process through connection
                auto proc_result = client_conn_->process_packet(data);
                if (proc_result.is_err()) {
                    continue;
                }

                // Check for CRYPTO data (NewSessionTicket comes in CRYPTO frame)
                if (client_conn_->has_pending_crypto()) {
                    auto crypto_data = client_conn_->take_crypto_data();

                    // Try to parse as NewSessionTicket
                    auto ticket_result = tls_->process_new_session_ticket(crypto_data);
                    if (ticket_result.is_ok()) {
                        echo::debug("Received session ticket for future 0-RTT");
                        return dp::result::ok(dp::Optional<tls::SessionTicket>(ticket_result.value()));
                    }
                }

                // Send ACKs
                auto datagrams = client_conn_->get_datagrams_to_send();
                for (auto &dg : datagrams) {
                    udp_.send_to(dg, remote_endpoint_);
                }
            }

            // No ticket received
            return dp::result::ok(dp::Optional<tls::SessionTicket>());
        }

        // Check if 0-RTT early data was accepted by server
        bool early_data_accepted() const { return tls_ && tls_->early_data_accepted(); }

        // Send early data (0-RTT) - must be called before handshake completes
        // Only works if session ticket was provided in config
        dp::Res<void> send_early_data(const Message &msg) {
            if (!tls_ || !tls_->can_send_early_data()) {
                return dp::result::err(dp::Error::invalid_argument("0-RTT not available"));
            }

            if (!client_conn_) {
                return dp::result::err(dp::Error::io_error("not connected"));
            }

            // Install 0-RTT keys if not already done
            if (!client_conn_->has_0rtt_keys() && tls_->has_early_data_keys()) {
                client_conn_->set_0rtt_keys(tls_->early_data_keys());
            }

            if (!client_conn_->has_0rtt_keys()) {
                return dp::result::err(dp::Error::invalid_argument("0-RTT keys not available"));
            }

            // Create a stream for early data (stream 0)
            StreamFrame frame;
            frame.stream_id = 0; // Use stream 0 for early data
            frame.offset = 0;
            frame.data = msg;
            frame.fin = false;

            auto frame_bytes = frame.serialize();

            // Build 0-RTT packet
            auto packet_result = client_conn_->build_0rtt_packet(frame_bytes);
            if (packet_result.is_err()) {
                return dp::result::err(packet_result.error());
            }

            // Send it
            auto send_result = udp_.send_to(packet_result.value(), remote_endpoint_);
            if (send_result.is_err()) {
                return dp::result::err(send_result.error());
            }

            echo::debug("Sent 0-RTT early data (", msg.size(), " bytes)");
            return dp::result::ok();
        }
    };

} // namespace netpipe::quic

#include <chrono>
#include <doctest/doctest.h>
#include <netpipe/http3.hpp>
#include <netpipe/quic.hpp>
#include <netpipe/quic/ack_manager.hpp>
#include <netpipe/quic/congestion_control.hpp>
#include <netpipe/quic/flow_control.hpp>
#include <netpipe/quic/loss_detection.hpp>
#include <netpipe/quic/tls_adapter.hpp>
#include <netpipe/tls/messages.hpp>
#include <thread>

using namespace netpipe::quic;
namespace tls = netpipe::tls;

// =============================================================================
// VarInt Tests
// =============================================================================

TEST_CASE("QUIC VarInt encoding/decoding") {
    SUBCASE("1-byte encoding (0-63)") {
        CHECK(varint_encode(0).size() == 1);
        CHECK(varint_encode(63).size() == 1);

        auto encoded = varint_encode(42);
        CHECK(encoded.size() == 1);
        CHECK(encoded[0] == 42);

        auto decoded = varint_decode(encoded.data(), encoded.size());
        CHECK(decoded.is_ok());
        CHECK(decoded.value().first == 42);
        CHECK(decoded.value().second == 1);
    }

    SUBCASE("2-byte encoding (64-16383)") {
        CHECK(varint_encode(64).size() == 2);
        CHECK(varint_encode(16383).size() == 2);

        auto encoded = varint_encode(1000);
        CHECK(encoded.size() == 2);

        auto decoded = varint_decode(encoded.data(), encoded.size());
        CHECK(decoded.is_ok());
        CHECK(decoded.value().first == 1000);
        CHECK(decoded.value().second == 2);
    }

    SUBCASE("4-byte encoding (16384-1073741823)") {
        CHECK(varint_encode(16384).size() == 4);
        CHECK(varint_encode(1073741823).size() == 4);

        auto encoded = varint_encode(1000000);
        CHECK(encoded.size() == 4);

        auto decoded = varint_decode(encoded.data(), encoded.size());
        CHECK(decoded.is_ok());
        CHECK(decoded.value().first == 1000000);
        CHECK(decoded.value().second == 4);
    }

    SUBCASE("8-byte encoding (large values)") {
        CHECK(varint_encode(1073741824).size() == 8);

        auto encoded = varint_encode(1000000000000ULL);
        CHECK(encoded.size() == 8);

        auto decoded = varint_decode(encoded.data(), encoded.size());
        CHECK(decoded.is_ok());
        CHECK(decoded.value().first == 1000000000000ULL);
        CHECK(decoded.value().second == 8);
    }

    SUBCASE("Round-trip all boundary values") {
        dp::Vector<dp::u64> test_values = {0,          1,          63,         64,
                                           16383,      16384,      1073741823, 1073741824,
                                           VARINT_MAX, VARINT_MAX - 1};

        for (auto val : test_values) {
            auto encoded = varint_encode(val);
            auto decoded = varint_decode(encoded.data(), encoded.size());
            CHECK(decoded.is_ok());
            CHECK(decoded.value().first == val);
        }
    }
}

// =============================================================================
// Frame Tests
// =============================================================================

TEST_CASE("QUIC Frame encoding/decoding") {
    SUBCASE("PING frame") {
        PingFrame ping;
        auto encoded = ping.serialize();
        CHECK(encoded.size() == 1);
        CHECK(encoded[0] == 0x01);

        auto parsed = PingFrame::parse(encoded.data(), encoded.size());
        CHECK(parsed.is_ok());
    }

    SUBCASE("PADDING frame") {
        PaddingFrame padding;
        padding.count = 10;
        auto encoded = padding.serialize();
        CHECK(encoded.size() == 10);
        for (auto b : encoded) {
            CHECK(b == 0x00);
        }
    }

    SUBCASE("CRYPTO frame") {
        CryptoFrame crypto;
        crypto.offset = 100;
        crypto.data = {0x01, 0x02, 0x03, 0x04, 0x05};

        auto encoded = crypto.serialize();
        CHECK(encoded[0] == 0x06);

        auto parsed = CryptoFrame::parse(encoded.data(), encoded.size());
        CHECK(parsed.is_ok());
        CHECK(parsed.value().first.offset == 100);
        CHECK(parsed.value().first.data.size() == 5);
    }

    SUBCASE("STREAM frame") {
        StreamFrame stream;
        stream.stream_id = 4;
        stream.offset = 1000;
        stream.data = {0xDE, 0xAD, 0xBE, 0xEF};
        stream.fin = true;

        auto encoded = stream.serialize();
        CHECK(is_stream_frame(encoded[0]));
        CHECK((encoded[0] & STREAM_FIN_BIT) != 0);

        auto parsed = StreamFrame::parse(encoded.data(), encoded.size());
        CHECK(parsed.is_ok());
        CHECK(parsed.value().first.stream_id == 4);
        CHECK(parsed.value().first.offset == 1000);
        CHECK(parsed.value().first.fin == true);
        CHECK(parsed.value().first.data.size() == 4);
    }

    SUBCASE("ACK frame - simple") {
        AckFrame ack;
        ack.largest_ack = 100;
        ack.ack_delay = 500;
        ack.ack_ranges.push_back({0, 5}); // Acks 95-100

        auto encoded = ack.serialize();
        CHECK(encoded[0] == 0x02);

        auto parsed = AckFrame::parse(encoded.data(), encoded.size());
        CHECK(parsed.is_ok());
        CHECK(parsed.value().first.largest_ack == 100);
        CHECK(parsed.value().first.ack_delay == 500);
    }

    SUBCASE("ACK frame - with gaps") {
        AckFrame ack;
        ack.largest_ack = 100;
        ack.ack_delay = 100;
        ack.ack_ranges.push_back({0, 2});  // First range: 98-100
        ack.ack_ranges.push_back({2, 3});  // Gap of 3, then range of 4

        auto encoded = ack.serialize();
        auto parsed = AckFrame::parse(encoded.data(), encoded.size());
        CHECK(parsed.is_ok());
        CHECK(parsed.value().first.ack_ranges.size() == 2);
    }

    SUBCASE("CONNECTION_CLOSE frame") {
        ConnectionCloseFrame close;
        close.is_application_error = false;
        close.error_code = static_cast<dp::u64>(TransportError::FlowControlError);
        close.frame_type = 0x08;
        close.reason_phrase = "flow control exceeded";

        auto encoded = close.serialize();
        CHECK(encoded[0] == 0x1c);

        auto parsed = ConnectionCloseFrame::parse(encoded.data(), encoded.size());
        CHECK(parsed.is_ok());
        CHECK(parsed.value().first.error_code == static_cast<dp::u64>(TransportError::FlowControlError));
        CHECK(parsed.value().first.reason_phrase == "flow control exceeded");
    }
}

// =============================================================================
// Packet Header Tests
// =============================================================================

TEST_CASE("QUIC Packet headers") {
    SUBCASE("Long header detection") {
        CHECK(is_long_header(0x80) == true);
        CHECK(is_long_header(0xC0) == true);
        CHECK(is_long_header(0x40) == false);
        CHECK(is_long_header(0x00) == false);
    }

    SUBCASE("Long header serialization/parsing") {
        LongHeader header;
        header.packet_type = LongPacketType::Initial;
        header.version = QUIC_VERSION_1;
        header.dest_cid = ConnectionId::generate(8);
        header.src_cid = ConnectionId::generate(8);
        header.pn_length = 1;

        auto encoded = header.serialize();
        CHECK(is_long_header(encoded[0]));

        auto parsed = LongHeader::parse(encoded.data(), encoded.size());
        CHECK(parsed.is_ok());
        CHECK(parsed.value().first.version == QUIC_VERSION_1);
        CHECK(parsed.value().first.dest_cid == header.dest_cid);
        CHECK(parsed.value().first.src_cid == header.src_cid);
    }

    SUBCASE("Short header serialization/parsing") {
        ShortHeader header;
        header.dest_cid = ConnectionId::generate(8);
        header.pn_length = 1;
        header.key_phase = false;
        header.spin_bit = false;

        auto encoded = header.serialize();
        CHECK(is_long_header(encoded[0]) == false);

        auto parsed = ShortHeader::parse(encoded.data(), encoded.size(), header.dest_cid.size());
        CHECK(parsed.is_ok());
        CHECK(parsed.value().first.dest_cid == header.dest_cid);
    }
}

// =============================================================================
// Loss Detection Tests
// =============================================================================

TEST_CASE("QUIC Loss Detection") {
    SUBCASE("RTT estimation - initial values") {
        RttEstimator rtt;
        rtt.init();

        CHECK(rtt.smoothed_rtt == constants::kInitialRtt);
        CHECK(rtt.rttvar == constants::kInitialRtt / 2);
        CHECK(rtt.has_sample == false);
    }

    SUBCASE("RTT estimation - first sample") {
        RttEstimator rtt;
        rtt.init();

        rtt.update(std::chrono::microseconds{100000}); // 100ms

        CHECK(rtt.has_sample == true);
        CHECK(rtt.smoothed_rtt == std::chrono::microseconds{100000});
        CHECK(rtt.latest_rtt == std::chrono::microseconds{100000});
        CHECK(rtt.min_rtt == std::chrono::microseconds{100000});
    }

    SUBCASE("RTT estimation - subsequent samples") {
        RttEstimator rtt;
        rtt.init();

        rtt.update(std::chrono::microseconds{100000});
        rtt.update(std::chrono::microseconds{120000});

        // Smoothed RTT should be between the two samples
        CHECK(rtt.smoothed_rtt > std::chrono::microseconds{100000});
        CHECK(rtt.smoothed_rtt < std::chrono::microseconds{120000});
        CHECK(rtt.min_rtt == std::chrono::microseconds{100000});
    }

    SUBCASE("PTO calculation") {
        RttEstimator rtt;
        rtt.init();

        auto pto = rtt.pto();
        // PTO = smoothed_rtt + max(4*rttvar, granularity) + max_ack_delay
        CHECK(pto > rtt.smoothed_rtt);
    }

    SUBCASE("Loss detection - packet tracking") {
        LossDetection ld;

        // Send some packets
        ld.on_packet_sent(PacketNumberSpace::ApplicationData, 0, 1200, true);
        ld.on_packet_sent(PacketNumberSpace::ApplicationData, 1, 1200, true);
        ld.on_packet_sent(PacketNumberSpace::ApplicationData, 2, 1200, true);

        CHECK(ld.bytes_in_flight() == 3600);
        CHECK(ld.unacked_count(PacketNumberSpace::ApplicationData) == 3);
    }

    SUBCASE("Loss detection - ACK processing") {
        LossDetection ld;

        ld.on_packet_sent(PacketNumberSpace::ApplicationData, 0, 1200, true);
        ld.on_packet_sent(PacketNumberSpace::ApplicationData, 1, 1200, true);

        // Acknowledge packet 1
        dp::Vector<std::pair<dp::u64, dp::u64>> ack_ranges = {{1, 1}};
        auto acked = ld.on_ack_received(PacketNumberSpace::ApplicationData, 1, 0, ack_ranges);

        CHECK(acked.size() == 1);
        CHECK(acked[0].packet_number == 1);
        CHECK(ld.bytes_in_flight() == 1200); // Only packet 0 remains
    }
}

// =============================================================================
// Congestion Control Tests
// =============================================================================

TEST_CASE("QUIC Congestion Control") {
    SUBCASE("Initial state") {
        CongestionController cc;

        CHECK(cc.congestion_window() == cc_constants::kInitialWindow);
        CHECK(cc.bytes_in_flight() == 0);
        CHECK(cc.state() == CongestionState::SlowStart);
        CHECK(cc.can_send(1200) == true);
    }

    SUBCASE("Slow start growth") {
        CongestionController cc;

        dp::u64 initial_cwnd = cc.congestion_window();

        // Simulate sending and ACK
        cc.on_packet_sent(1200);
        CHECK(cc.bytes_in_flight() == 1200);

        dp::Vector<SentPacketInfo> acked;
        SentPacketInfo pkt;
        pkt.bytes_sent = 1200;
        pkt.in_flight = true;
        acked.push_back(pkt);

        cc.on_packets_acked(acked);

        // In slow start, cwnd increases by bytes acked
        CHECK(cc.congestion_window() > initial_cwnd);
    }

    SUBCASE("Loss triggers recovery") {
        CongestionController cc;

        dp::u64 initial_cwnd = cc.congestion_window();

        dp::Vector<SentPacketInfo> lost;
        SentPacketInfo pkt;
        pkt.bytes_sent = 1200;
        pkt.in_flight = true;
        pkt.sent_time = std::chrono::steady_clock::now();
        lost.push_back(pkt);

        cc.on_packets_lost(lost);

        CHECK(cc.state() == CongestionState::Recovery);
        CHECK(cc.congestion_window() < initial_cwnd);
    }

    SUBCASE("Window exhaustion blocks sending") {
        CongestionController cc;

        // Fill the congestion window
        while (cc.can_send(1200)) {
            cc.on_packet_sent(1200);
        }

        CHECK(cc.can_send(1200) == false);
        CHECK(cc.available_window() < 1200);
    }
}

// =============================================================================
// ACK Manager Tests
// =============================================================================

TEST_CASE("QUIC ACK Manager") {
    SUBCASE("Packet tracking") {
        AckManager am;

        am.on_packet_received(PacketNumberSpace::ApplicationData, 0, true);
        am.on_packet_received(PacketNumberSpace::ApplicationData, 1, true);

        CHECK(am.state(PacketNumberSpace::ApplicationData).has_received() == true);
        CHECK(am.state(PacketNumberSpace::ApplicationData).largest_received() == 1);
    }

    SUBCASE("Should send ACK after threshold") {
        AckManager am;

        am.on_packet_received(PacketNumberSpace::ApplicationData, 0, true);
        CHECK(am.state(PacketNumberSpace::ApplicationData).should_send_ack() == false);

        am.on_packet_received(PacketNumberSpace::ApplicationData, 1, true);
        CHECK(am.state(PacketNumberSpace::ApplicationData).should_send_ack() == true);
    }

    SUBCASE("ACK frame generation") {
        AckManager am;

        am.on_packet_received(PacketNumberSpace::ApplicationData, 5, true);
        am.on_packet_received(PacketNumberSpace::ApplicationData, 6, true);
        am.on_packet_received(PacketNumberSpace::ApplicationData, 7, true);

        auto ack_result = am.generate_ack_frame(PacketNumberSpace::ApplicationData);
        CHECK(ack_result.is_ok());

        auto &ack = ack_result.value();
        CHECK(ack.largest_ack == 7);
        CHECK(ack.ack_ranges.size() >= 1);
    }

    SUBCASE("ACK range decoding") {
        AckFrame ack;
        ack.largest_ack = 10;
        ack.ack_delay = 0;
        ack.ack_ranges.push_back({0, 2}); // 8, 9, 10

        auto ranges = AckState::decode_ack_ranges(ack);
        CHECK(ranges.size() == 1);
        CHECK(ranges[0].first == 8);
        CHECK(ranges[0].second == 10);
    }
}

// =============================================================================
// Flow Control Tests
// =============================================================================

TEST_CASE("QUIC Flow Control") {
    SUBCASE("Connection-level flow control") {
        ConnectionFlowControl fc;
        fc.init(1000000, 500000); // Local max 1MB, peer max 500KB

        CHECK(fc.send_limit() == 500000);
        CHECK(fc.recv_limit() == 1000000);
        CHECK(fc.send_window() == 500000);
        CHECK(fc.is_send_blocked() == false);

        fc.on_data_sent(400000);
        CHECK(fc.send_window() == 100000);

        fc.on_data_sent(100000);
        CHECK(fc.is_send_blocked() == true);
    }

    SUBCASE("Stream limit management") {
        StreamLimitManager slm(true); // Client
        slm.init(100, 100, 50, 50);   // Local limits, peer limits

        CHECK(slm.can_create_stream(true) == true);  // Bidi
        CHECK(slm.can_create_stream(false) == true); // Uni

        for (int i = 0; i < 50; i++) {
            slm.on_stream_created(true);
        }
        CHECK(slm.can_create_stream(true) == false);
    }

    SUBCASE("Flow control manager") {
        FlowControlManager fcm(true); // Client

        fcm.init_from_transport_params(1000000, 500000, // Connection data limits
                                       262144, 262144, 262144, // Local stream limits
                                       262144, 262144, 262144, // Peer stream limits
                                       100, 100,               // Local stream counts
                                       100, 100);              // Peer stream counts

        CHECK(fcm.connection().send_limit() == 500000);
        CHECK(fcm.stream_limits().can_create_stream(true) == true);

        auto error = fcm.validate_send(100000);
        CHECK(error.empty());

        auto error2 = fcm.validate_send(1000000);
        CHECK(!error2.empty());
    }
}

// =============================================================================
// Connection Tests
// =============================================================================

TEST_CASE("QUIC Connection") {
    SUBCASE("Connection creation") {
        Connection client_conn(true);
        Connection server_conn(false);

        CHECK(client_conn.is_client() == true);
        CHECK(server_conn.is_client() == false);
        CHECK(client_conn.state() == ConnectionState::Idle);
    }

    SUBCASE("Connection ID generation") {
        auto cid = ConnectionId::generate(8);
        CHECK(cid.size() == 8);
        CHECK(!cid.empty());

        auto cid2 = ConnectionId::generate(8);
        CHECK(cid != cid2); // Should be different (random)
    }

    SUBCASE("Initial key derivation") {
        Connection conn(true);
        auto dcid = ConnectionId::generate(8);

        conn.set_local_cid(ConnectionId::generate(8));
        conn.set_remote_cid(dcid);
        conn.set_initial_keys(dcid);

        CHECK(conn.has_keys(PacketNumberSpace::Initial) == true);
    }

    SUBCASE("Handshake state transitions") {
        Connection conn(true);

        CHECK(conn.state() == ConnectionState::Idle);

        auto result = conn.start_handshake();
        CHECK(result.is_ok());
        CHECK(conn.state() == ConnectionState::Handshaking);

        conn.handshake_complete();
        CHECK(conn.state() == ConnectionState::Connected);
        CHECK(conn.is_connected() == true);
    }

    SUBCASE("Stream creation") {
        Connection conn(true);
        conn.start_handshake();

        // Set remote params to allow streams (simulating peer handshake)
        TransportParameters params;
        params.initial_max_streams_bidi = 100;
        params.initial_max_streams_uni = 100;
        conn.set_remote_params(params);

        conn.handshake_complete();

        auto stream_result = conn.create_stream(true);
        CHECK(stream_result.is_ok());

        auto stream_id = stream_result.value();
        CHECK(is_client_initiated(stream_id) == true);
        CHECK(is_bidirectional(stream_id) == true);
    }

    SUBCASE("Module accessors") {
        Connection conn(true);

        // Verify all modules are accessible
        auto &ld = conn.loss_detection();
        auto &cc = conn.congestion_control();
        auto &am = conn.ack_manager();
        auto &fc = conn.flow_control();
        auto &rtt = conn.rtt();

        CHECK(ld.pto_count() == 0);
        CHECK(cc.congestion_window() == cc_constants::kInitialWindow);
        CHECK(conn.can_send(1200) == true);
    }
}

// =============================================================================
// Stream Types Tests
// =============================================================================

TEST_CASE("QUIC Stream ID helpers") {
    SUBCASE("Client-initiated bidirectional") {
        CHECK(stream_type(0) == StreamType::ClientBidirectional);
        CHECK(stream_type(4) == StreamType::ClientBidirectional);
        CHECK(stream_type(8) == StreamType::ClientBidirectional);

        CHECK(is_client_initiated(0) == true);
        CHECK(is_bidirectional(0) == true);
    }

    SUBCASE("Server-initiated bidirectional") {
        CHECK(stream_type(1) == StreamType::ServerBidirectional);
        CHECK(stream_type(5) == StreamType::ServerBidirectional);

        CHECK(is_client_initiated(1) == false);
        CHECK(is_bidirectional(1) == true);
    }

    SUBCASE("Client-initiated unidirectional") {
        CHECK(stream_type(2) == StreamType::ClientUnidirectional);
        CHECK(stream_type(6) == StreamType::ClientUnidirectional);

        CHECK(is_client_initiated(2) == true);
        CHECK(is_bidirectional(2) == false);
        CHECK(is_unidirectional(2) == true);
    }

    SUBCASE("Server-initiated unidirectional") {
        CHECK(stream_type(3) == StreamType::ServerUnidirectional);
        CHECK(stream_type(7) == StreamType::ServerUnidirectional);

        CHECK(is_client_initiated(3) == false);
        CHECK(is_unidirectional(3) == true);
    }
}

// =============================================================================
// Crypto Tests
// =============================================================================

TEST_CASE("QUIC Crypto") {
    SUBCASE("Initial secret derivation") {
        auto dcid = ConnectionId::generate(8);
        auto [client_secret, server_secret] = derive_initial_secrets(dcid, QUIC_VERSION_1);

        CHECK(client_secret.size() == 32);
        CHECK(server_secret.size() == 32);
        CHECK(client_secret != server_secret);
    }

    SUBCASE("Key derivation from secret") {
        dp::Vector<dp::u8> secret(32, 0x42);
        auto keys = derive_keys(secret);

        CHECK(keys.key.size() > 0);
        CHECK(keys.iv.size() == AEAD_IV_LENGTH);
        CHECK(keys.hp_key.size() == HEADER_PROTECTION_KEY_LENGTH);
    }

    SUBCASE("Packet number encoding") {
        // packet_number_length returns 0-3 meaning 1-4 bytes
        // range = pn - largest_acked
        CHECK(packet_number_length(0, 0) == 0);     // range=0 < 128, 1 byte
        CHECK(packet_number_length(127, 0) == 0);   // range=127 < 128, 1 byte
        CHECK(packet_number_length(128, 0) == 1);   // range=128 >= 128, 2 bytes
        CHECK(packet_number_length(32767, 0) == 1); // range=32767 < 32768, 2 bytes
        CHECK(packet_number_length(32768, 0) == 2); // range=32768 >= 32768, 3 bytes

        // encode_packet_number(pn, pn_length) where pn_length is 0-3
        auto encoded = encode_packet_number(12345, 1); // 2 bytes
        CHECK(encoded.size() == 2);
    }
}

// =============================================================================
// Transport Parameters Tests
// =============================================================================

TEST_CASE("QUIC Transport Parameters") {
    SUBCASE("Default values") {
        TransportParameters params;

        CHECK(params.max_idle_timeout == 30000);
        CHECK(params.initial_max_data > 0);
        CHECK(params.initial_max_streams_bidi > 0);
    }

    SUBCASE("Serialization round-trip") {
        TransportParameters params;
        params.max_idle_timeout = 60000;
        params.initial_max_data = 2000000;
        params.initial_max_streams_bidi = 200;

        auto encoded = params.serialize();
        CHECK(encoded.size() > 0);

        auto parsed = TransportParameters::parse(encoded.data(), encoded.size());
        CHECK(parsed.is_ok());
        CHECK(parsed.value().max_idle_timeout == 60000);
        CHECK(parsed.value().initial_max_data == 2000000);
        CHECK(parsed.value().initial_max_streams_bidi == 200);
    }
}

// =============================================================================
// TLS Adapter Tests
// =============================================================================

TEST_CASE("QUIC TLS Adapter") {
    SUBCASE("Client creates ClientHello") {
        QuicTlsConfig config;
        config.server_name = "localhost";
        QuicTlsAdapter tls(true, config);

        CHECK(tls.state() == QuicTlsState::Start);

        auto ch_result = tls.create_client_hello();
        CHECK(ch_result.is_ok());

        auto ch = ch_result.value();
        CHECK(ch.size() > 0);

        // Verify it starts with handshake header (type=ClientHello=1)
        CHECK(ch[0] == 0x01);

        CHECK(tls.state() == QuicTlsState::WaitServerHello);
    }

    SUBCASE("Encryption levels") {
        QuicTlsConfig config;
        QuicTlsAdapter tls(true, config);

        CHECK(tls.current_level() == EncryptionLevel::Initial);
        CHECK(tls.is_complete() == false);
        CHECK(tls.is_error() == false);
    }
}

// =============================================================================
// QuicStream Endpoint Tests
// =============================================================================

TEST_CASE("QUIC QuicStream basic") {
    SUBCASE("QuicStream construction") {
        QuicConfig config;
        QuicStream stream(config);

        CHECK(stream.is_connected() == false);
    }

    SUBCASE("QuicStreamHandle") {
        auto conn = std::make_shared<Connection>(true);
        conn->start_handshake();

        TransportParameters params;
        params.initial_max_streams_bidi = 100;
        conn->set_remote_params(params);
        conn->handshake_complete();

        auto stream_result = conn->create_stream(true);
        REQUIRE(stream_result.is_ok());

        QuicStreamHandle handle(conn, stream_result.value());
        CHECK(handle.id() == stream_result.value());
    }
}

// =============================================================================
// Server TLS Adapter Tests
// =============================================================================

TEST_CASE("QUIC Server TLS Adapter") {
    SUBCASE("Server processes ClientHello") {
        // Create client TLS adapter and generate ClientHello
        QuicTlsConfig client_config;
        client_config.server_name = "localhost";
        QuicTlsAdapter client_tls(true, client_config);

        auto ch_result = client_tls.create_client_hello();
        REQUIRE(ch_result.is_ok());
        auto client_hello = ch_result.value();

        // Create server TLS adapter (no certificate for testing)
        QuicTlsConfig server_config;
        // In production you would provide certificate and private key
        QuicTlsAdapter server_tls(false, server_config);

        CHECK(server_tls.state() == QuicTlsState::Start);

        // Process ClientHello - this will generate ServerHello + other messages
        auto resp_result = server_tls.process_client_hello_and_respond(client_hello);
        REQUIRE(resp_result.is_ok());

        auto &messages = resp_result.value();
        // Should have: ServerHello, EncryptedExtensions, Certificate, CertificateVerify, Finished
        CHECK(messages.size() == 5);

        // After processing ClientHello, server should be waiting for client Finished
        CHECK(server_tls.state() == QuicTlsState::WaitClientFinished);

        // Should have handshake and application keys
        CHECK(server_tls.has_handshake_keys() == true);
        CHECK(server_tls.has_application_keys() == true);
    }

    SUBCASE("Server TLS state transitions") {
        QuicTlsConfig config;
        QuicTlsAdapter server_tls(false, config);

        CHECK(server_tls.state() == QuicTlsState::Start);
        CHECK(server_tls.current_level() == EncryptionLevel::Initial);
        CHECK(server_tls.is_complete() == false);
        CHECK(server_tls.is_error() == false);
    }
}

// =============================================================================
// 0-RTT / Session Resumption Tests
// =============================================================================

TEST_CASE("QUIC 0-RTT Session Tickets") {
    SUBCASE("NewSessionTicket message serialization") {
        tls::NewSessionTicket nst;
        nst.ticket_lifetime = 86400; // 1 day
        nst.ticket_age_add = 0x12345678;
        nst.ticket_nonce = {0x01, 0x02, 0x03, 0x04};
        nst.ticket = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE};

        auto serialized = nst.serialize();
        CHECK(!serialized.empty());

        // Parse it back
        auto header_result = tls::parse_handshake_header(serialized.data(), serialized.size());
        REQUIRE(header_result.is_ok());

        auto [msg_type, msg_length] = header_result.value();
        CHECK(msg_type == tls::HandshakeType::NewSessionTicket);

        auto parsed_result =
            tls::NewSessionTicket::parse(serialized.data() + tls::HANDSHAKE_HEADER_SIZE, msg_length);
        REQUIRE(parsed_result.is_ok());

        auto &parsed = parsed_result.value();
        CHECK(parsed.ticket_lifetime == 86400);
        CHECK(parsed.ticket_age_add == 0x12345678);
        CHECK(parsed.ticket_nonce == nst.ticket_nonce);
        CHECK(parsed.ticket == nst.ticket);
    }

    SUBCASE("Server creates NewSessionTicket") {
        // First complete a handshake
        QuicTlsConfig client_config;
        client_config.server_name = "localhost";
        client_config.skip_cert_verification = true;
        QuicTlsAdapter client_tls(true, client_config);

        QuicTlsConfig server_config;
        server_config.max_early_data_size = 16384;
        QuicTlsAdapter server_tls(false, server_config);

        // Client -> ClientHello
        auto ch_result = client_tls.create_client_hello();
        REQUIRE(ch_result.is_ok());

        // Server <- ClientHello, Server -> ServerHello + messages
        auto resp_result = server_tls.process_client_hello_and_respond(ch_result.value());
        REQUIRE(resp_result.is_ok());

        auto &server_msgs = resp_result.value();
        REQUIRE(server_msgs.size() == 5);

        // Client processes server messages
        auto sh_result = client_tls.process_server_hello(server_msgs[0]);
        REQUIRE(sh_result.is_ok());

        // Process remaining handshake messages (EE, Cert, CV, Finished)
        for (size_t i = 1; i < server_msgs.size(); i++) {
            auto result = client_tls.process_handshake_message(server_msgs[i]);
            REQUIRE(result.is_ok());
        }

        // Client creates Finished
        auto fin_result = client_tls.create_client_finished();
        REQUIRE(fin_result.is_ok());

        // Server processes client Finished
        auto srv_fin_result = server_tls.process_client_finished(fin_result.value());
        REQUIRE(srv_fin_result.is_ok());

        // Both sides connected
        CHECK(client_tls.is_complete() == true);
        CHECK(server_tls.is_complete() == true);

        // Server creates NewSessionTicket
        auto nst_result = server_tls.create_new_session_ticket();
        REQUIRE(nst_result.is_ok());

        auto nst_bytes = nst_result.value();
        CHECK(!nst_bytes.empty());

        // Client processes NewSessionTicket
        auto ticket_result = client_tls.process_new_session_ticket(nst_bytes);
        REQUIRE(ticket_result.is_ok());

        auto &stored = ticket_result.value();
        CHECK(stored.ticket_lifetime > 0);
        CHECK(!stored.ticket.empty());
        CHECK(!stored.resumption_master_secret.empty());
        CHECK(stored.is_valid() == true);

        // PSK can be computed
        auto psk = stored.compute_psk();
        CHECK(psk.size() == tls::HASH_LENGTH);
    }

    SUBCASE("Session ticket validity check") {
        tls::SessionTicket ticket;
        ticket.ticket_lifetime = 1; // 1 second
        ticket.timestamp_ms = static_cast<dp::u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                       std::chrono::steady_clock::now().time_since_epoch())
                                                       .count());

        CHECK(ticket.is_valid() == true);

        // After expiry, should be invalid
        // We can't easily test this without waiting, so just check the logic
    }

    SUBCASE("Client PSK extension serialization") {
        // Create a session ticket
        tls::SessionTicket stored;
        stored.ticket = {0x01, 0x02, 0x03, 0x04, 0x05};
        stored.ticket_lifetime = 86400;
        stored.ticket_age_add = 0xAABBCCDD;
        stored.ticket_nonce = {0x10, 0x20};
        stored.resumption_master_secret.resize(32, 0x42);
        stored.cipher_suite = tls::TLS_CHACHA20_POLY1305_SHA256;
        stored.timestamp_ms = static_cast<dp::u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                       std::chrono::steady_clock::now().time_since_epoch())
                                                       .count());

        // Create client with session ticket
        QuicTlsConfig config;
        config.session_ticket = stored;
        config.enable_early_data = true;

        QuicTlsAdapter client(true, config);

        CHECK(client.can_send_early_data() == true);

        // Create ClientHello with PSK
        auto ch_result = client.create_client_hello_with_psk();
        REQUIRE(ch_result.is_ok());

        auto &ch_bytes = ch_result.value();
        CHECK(!ch_bytes.empty());

        // Should have derived early data keys
        CHECK(client.has_early_data_keys() == true);
    }

    SUBCASE("Early data keys derivation") {
        // This tests the key derivation path for 0-RTT
        tls::KeySchedule ks;

        // Initialize with a dummy PSK
        dp::Vector<dp::u8> psk(32, 0x42);
        ks.init_with_psk(psk);

        CHECK(!ks.early_secret.empty());
        CHECK(ks.early_secret.size() == tls::HASH_LENGTH);

        // Derive early secrets from ClientHello hash
        dp::Vector<dp::u8> ch_hash(32, 0x11);
        ks.derive_early_secrets(ch_hash);

        CHECK(!ks.client_early_traffic_secret.empty());
        CHECK(ks.client_early_traffic_secret.size() == tls::HASH_LENGTH);

        // Derive traffic keys
        auto [key, iv] = tls::KeySchedule::derive_traffic_keys(ks.client_early_traffic_secret);
        CHECK(key.size() == 32);
        CHECK(iv.size() == 12);
    }

    SUBCASE("Anti-replay store") {
        AntiReplayStore store(1000); // 1 second window

        // First check should pass
        dp::Vector<dp::u8> hash1 = {0x01, 0x02, 0x03, 0x04};
        CHECK(store.check(hash1) == true);

        // Same hash should be detected as replay
        CHECK(store.check(hash1) == false);

        // Different hash should pass
        dp::Vector<dp::u8> hash2 = {0x05, 0x06, 0x07, 0x08};
        CHECK(store.check(hash2) == true);

        // Stats
        CHECK(store.size() == 2);
    }

    SUBCASE("Server early data acceptance") {
        // Create server TLS adapter
        QuicTlsConfig server_config;
        server_config.max_early_data_size = 16384;
        QuicTlsAdapter server(false, server_config);

        // Server hasn't received any early data offer
        CHECK(server.client_offered_early_data() == false);

        // Cannot accept early data without offer
        CHECK(server.accept_early_data() == false);
    }
}

TEST_CASE("QUIC 0-RTT Packets") {
    SUBCASE("Connection 0-RTT keys") {
        Connection conn(true);
        auto dcid = ConnectionId::generate(8);
        conn.set_local_cid(ConnectionId::generate(8));
        conn.set_remote_cid(dcid);

        // No 0-RTT keys initially
        CHECK(conn.has_0rtt_keys() == false);

        // Set some dummy 0-RTT keys
        QuicKeys keys;
        keys.key.resize(32, 0x42);
        keys.iv.resize(12, 0x43);
        keys.hp_key.resize(16, 0x44);

        conn.set_0rtt_keys(keys);
        CHECK(conn.has_0rtt_keys() == true);
    }

    SUBCASE("Build 0-RTT packet") {
        Connection conn(true);
        auto dcid = ConnectionId::generate(8);
        conn.set_local_cid(ConnectionId::generate(8));
        conn.set_remote_cid(dcid);
        conn.start_handshake();

        // Set 0-RTT keys
        QuicKeys keys;
        keys.key.resize(32, 0x42);
        keys.iv.resize(12, 0x43);
        keys.hp_key.resize(16, 0x44);
        conn.set_0rtt_keys(keys);

        // Build a 0-RTT packet with stream data
        StreamFrame frame;
        frame.stream_id = 0;
        frame.offset = 0;
        frame.data = {0x01, 0x02, 0x03, 0x04};
        frame.fin = false;

        auto frame_bytes = frame.serialize();
        auto packet_result = conn.build_0rtt_packet(frame_bytes);

        REQUIRE(packet_result.is_ok());
        auto &packet = packet_result.value();
        CHECK(!packet.empty());

        // Verify it's a long header with 0-RTT type
        CHECK(is_long_header(packet[0]) == true);
        auto type = static_cast<LongPacketType>((packet[0] & 0x30) >> 4);
        CHECK(type == LongPacketType::ZeroRTT);
    }

    SUBCASE("QuicConfig with session ticket") {
        // Create a dummy session ticket
        tls::SessionTicket ticket;
        ticket.ticket = {0x01, 0x02, 0x03};
        ticket.ticket_lifetime = 86400;
        ticket.ticket_age_add = 0x12345678;
        ticket.ticket_nonce = {0xAA, 0xBB};
        ticket.resumption_master_secret.resize(32, 0x42);
        ticket.cipher_suite = tls::TLS_CHACHA20_POLY1305_SHA256;
        ticket.timestamp_ms = static_cast<dp::u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                       std::chrono::steady_clock::now().time_since_epoch())
                                                       .count());

        QuicConfig config;
        config.session_ticket = ticket;
        config.enable_early_data = true;

        CHECK(config.session_ticket.has_value());
        CHECK(config.enable_early_data == true);
    }
}

// =============================================================================
// Connection Migration / Path Validation Tests
// =============================================================================

TEST_CASE("QUIC Path Validation") {
    SUBCASE("PATH_CHALLENGE frame serialization") {
        PathChallengeFrame frame;
        frame.data = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};

        auto serialized = frame.serialize();
        CHECK(serialized.size() == 9); // 1 byte type + 8 bytes data

        // Parse it back
        auto parse_result = PathChallengeFrame::parse(serialized.data(), serialized.size());
        REQUIRE(parse_result.is_ok());

        auto &[parsed, consumed] = parse_result.value();
        CHECK(consumed == 9);
        CHECK(parsed.data == frame.data);
    }

    SUBCASE("PATH_RESPONSE frame serialization") {
        PathResponseFrame frame;
        frame.data = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};

        auto serialized = frame.serialize();
        CHECK(serialized.size() == 9); // 1 byte type + 8 bytes data

        // Parse it back
        auto parse_result = PathResponseFrame::parse(serialized.data(), serialized.size());
        REQUIRE(parse_result.is_ok());

        auto &[parsed, consumed] = parse_result.value();
        CHECK(consumed == 9);
        CHECK(parsed.data == frame.data);
    }

    SUBCASE("Connection path challenge initiation") {
        Connection conn(true);
        auto dcid = ConnectionId::generate(8);
        conn.set_local_cid(ConnectionId::generate(8));
        conn.set_remote_cid(dcid);
        conn.start_handshake();

        // Initially path is validated
        CHECK(conn.is_path_validated() == true);

        // Initiate a challenge
        auto challenge = conn.initiate_path_challenge();
        CHECK(challenge.size() == 8);

        // Now path is not validated (waiting for response)
        CHECK(conn.is_path_validated() == false);
    }

    SUBCASE("Connection path response handling") {
        Connection conn(true);
        auto dcid = ConnectionId::generate(8);
        conn.set_local_cid(ConnectionId::generate(8));
        conn.set_remote_cid(dcid);
        conn.start_handshake();

        // No pending responses initially
        CHECK(conn.has_pending_path_responses() == false);
    }
}

TEST_CASE("QUIC Connection ID Management") {
    SUBCASE("NEW_CONNECTION_ID frame serialization") {
        NewConnectionIdFrame frame;
        frame.sequence_number = 5;
        frame.retire_prior_to = 2;
        frame.connection_id = ConnectionId::generate(8);
        frame.stateless_reset_token = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                       0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10};

        auto serialized = frame.serialize();
        CHECK(!serialized.empty());

        // Parse it back
        auto parse_result = NewConnectionIdFrame::parse(serialized.data(), serialized.size());
        REQUIRE(parse_result.is_ok());

        auto &[parsed, consumed] = parse_result.value();
        CHECK(parsed.sequence_number == 5);
        CHECK(parsed.retire_prior_to == 2);
        CHECK(parsed.connection_id == frame.connection_id);
        CHECK(parsed.stateless_reset_token == frame.stateless_reset_token);
    }

    SUBCASE("RETIRE_CONNECTION_ID frame serialization") {
        RetireConnectionIdFrame frame;
        frame.sequence_number = 3;

        auto serialized = frame.serialize();
        CHECK(!serialized.empty());

        // Parse it back
        auto parse_result = RetireConnectionIdFrame::parse(serialized.data(), serialized.size());
        REQUIRE(parse_result.is_ok());

        auto &[parsed, consumed] = parse_result.value();
        CHECK(parsed.sequence_number == 3);
    }

    SUBCASE("Connection issues new CIDs") {
        Connection conn(true);
        auto dcid = ConnectionId::generate(8);
        conn.set_local_cid(ConnectionId::generate(8));
        conn.set_remote_cid(dcid);
        conn.start_handshake();

        // Initial CID is sequence 0
        CHECK(conn.issued_cids_count() == 1);

        // Issue a new CID
        auto new_cid_frame = conn.issue_new_connection_id(0);
        CHECK(new_cid_frame.sequence_number == 1);
        CHECK(conn.issued_cids_count() == 2);

        // Issue another
        auto new_cid_frame2 = conn.issue_new_connection_id(0);
        CHECK(new_cid_frame2.sequence_number == 2);
        CHECK(conn.issued_cids_count() == 3);
    }

    SUBCASE("Connection processes peer CIDs") {
        Connection conn(true);
        auto dcid = ConnectionId::generate(8);
        conn.set_local_cid(ConnectionId::generate(8));
        conn.set_remote_cid(dcid);
        conn.start_handshake();

        // Initial peer CID
        CHECK(conn.available_peer_cids() == 1);

        // Receive NEW_CONNECTION_ID from peer
        NewConnectionIdFrame ncid;
        ncid.sequence_number = 1;
        ncid.retire_prior_to = 0;
        ncid.connection_id = ConnectionId::generate(8);
        ncid.stateless_reset_token.resize(16, 0x42);

        conn.process_new_connection_id(ncid);
        CHECK(conn.available_peer_cids() == 2);

        // Receive another with retire_prior_to = 1
        NewConnectionIdFrame ncid2;
        ncid2.sequence_number = 2;
        ncid2.retire_prior_to = 1;
        ncid2.connection_id = ConnectionId::generate(8);
        ncid2.stateless_reset_token.resize(16, 0x43);

        conn.process_new_connection_id(ncid2);
        // Old CID (seq 0) should be pending retirement
        CHECK(conn.has_pending_retire_cids() == true);
    }

    SUBCASE("Connection switches peer CID") {
        Connection conn(true);
        auto dcid = ConnectionId::generate(8);
        conn.set_local_cid(ConnectionId::generate(8));
        conn.set_remote_cid(dcid);
        conn.start_handshake();

        // Add a new peer CID
        NewConnectionIdFrame ncid;
        ncid.sequence_number = 1;
        ncid.retire_prior_to = 0;
        ncid.connection_id = ConnectionId::generate(8);
        ncid.stateless_reset_token.resize(16, 0x42);
        conn.process_new_connection_id(ncid);

        // Switch to new CID
        bool switched = conn.switch_to_peer_cid(1);
        CHECK(switched == true);
        CHECK(conn.remote_cid() == ncid.connection_id);

        // Try to switch to non-existent CID
        bool switched2 = conn.switch_to_peer_cid(99);
        CHECK(switched2 == false);
    }
}

TEST_CASE("QUIC NAT Rebinding") {
    SUBCASE("QuicStream NAT rebinding status") {
        QuicConfig config;
        QuicStream stream(config);

        // Initially no NAT rebinding
        CHECK(stream.nat_rebinding_detected() == false);
        CHECK(stream.path_validation_pending() == false);
    }

    SUBCASE("QuicStream path validation initiation") {
        QuicConfig config;
        QuicStream stream(config);

        // Cannot initiate path validation without connection
        auto result = stream.initiate_path_validation();
        CHECK(result.is_err());
    }
}

// =============================================================================
// HTTP/3 Tests
// =============================================================================

namespace http3 = netpipe::http3;

TEST_CASE("HTTP/3 Frame Types") {
    SUBCASE("DATA frame serialization/parsing") {
        http3::DataFrame frame;
        frame.data = {0x48, 0x65, 0x6C, 0x6C, 0x6F}; // "Hello"

        auto serialized = frame.serialize();
        CHECK(!serialized.empty());

        // First byte should be frame type (0x00 for DATA)
        CHECK(serialized[0] == 0x00);

        // Parse it back (skip frame type byte which is already consumed by caller)
        auto parse_result = http3::DataFrame::parse(serialized.data() + 1, serialized.size() - 1);
        REQUIRE(parse_result.is_ok());

        auto &[parsed, consumed] = parse_result.value();
        CHECK(parsed.data == frame.data);
    }

    SUBCASE("HEADERS frame serialization/parsing") {
        http3::HeadersFrame frame;
        frame.encoded_field_section = {0x00, 0x00, 0xC1, 0xC7}; // Some QPACK data

        auto serialized = frame.serialize();
        CHECK(!serialized.empty());

        // First byte should be frame type (0x01 for HEADERS)
        CHECK(serialized[0] == 0x01);

        // Parse it back
        auto parse_result = http3::HeadersFrame::parse(serialized.data() + 1, serialized.size() - 1);
        REQUIRE(parse_result.is_ok());

        auto &[parsed, consumed] = parse_result.value();
        CHECK(parsed.encoded_field_section == frame.encoded_field_section);
    }

    SUBCASE("SETTINGS frame serialization/parsing") {
        http3::SettingsFrame frame;
        frame.settings.qpack_max_table_capacity = 4096;
        frame.settings.max_field_section_size = 16384;
        frame.settings.qpack_blocked_streams = 100;

        auto serialized = frame.serialize();
        CHECK(!serialized.empty());

        // First byte should be frame type (0x04 for SETTINGS)
        CHECK(serialized[0] == 0x04);

        // Parse it back
        auto parse_result = http3::SettingsFrame::parse(serialized.data() + 1, serialized.size() - 1);
        REQUIRE(parse_result.is_ok());

        auto &[parsed, consumed] = parse_result.value();
        CHECK(parsed.settings.qpack_max_table_capacity == 4096);
        CHECK(parsed.settings.max_field_section_size == 16384);
        CHECK(parsed.settings.qpack_blocked_streams == 100);
    }

    SUBCASE("GOAWAY frame serialization/parsing") {
        http3::GoAwayFrame frame;
        frame.stream_id = 12;

        auto serialized = frame.serialize();
        CHECK(!serialized.empty());

        // First byte should be frame type (0x07 for GOAWAY)
        CHECK(serialized[0] == 0x07);

        // Parse it back
        auto parse_result = http3::GoAwayFrame::parse(serialized.data() + 1, serialized.size() - 1);
        REQUIRE(parse_result.is_ok());

        auto &[parsed, consumed] = parse_result.value();
        CHECK(parsed.stream_id == 12);
    }

    SUBCASE("Frame header parsing") {
        http3::DataFrame frame;
        frame.data = {0x01, 0x02, 0x03};

        auto serialized = frame.serialize();

        auto header_result = http3::parse_frame_header(serialized.data(), serialized.size());
        REQUIRE(header_result.is_ok());

        auto [frame_type, type_len] = header_result.value();
        CHECK(frame_type == http3::FrameType::Data);
        CHECK(type_len == 1);
    }
}

TEST_CASE("HTTP/3 Settings") {
    SUBCASE("Default settings") {
        http3::Settings settings;

        CHECK(settings.qpack_max_table_capacity == 0);
        CHECK(settings.max_field_section_size == 0);
        CHECK(settings.qpack_blocked_streams == 0);
    }

    SUBCASE("Settings serialization round-trip") {
        http3::Settings settings;
        settings.qpack_max_table_capacity = 1024;
        settings.max_field_section_size = 8192;
        settings.qpack_blocked_streams = 16;

        auto serialized = settings.serialize();
        CHECK(!serialized.empty());

        auto parsed_result = http3::Settings::parse(serialized.data(), serialized.size());
        REQUIRE(parsed_result.is_ok());

        auto &parsed = parsed_result.value();
        CHECK(parsed.qpack_max_table_capacity == 1024);
        CHECK(parsed.max_field_section_size == 8192);
        CHECK(parsed.qpack_blocked_streams == 16);
    }

    SUBCASE("Empty settings serialization") {
        http3::Settings settings;

        auto serialized = settings.serialize();
        CHECK(serialized.empty()); // All zero values, nothing to serialize
    }
}

TEST_CASE("HTTP/3 Request/Response") {
    SUBCASE("Request pseudo-headers") {
        http3::Request req;
        req.method = "GET";
        req.scheme = "https";
        req.authority = "example.com";
        req.path = "/api/test";

        auto pseudo = req.get_pseudo_headers();
        CHECK(pseudo.size() == 4);
        CHECK(pseudo[0].name == ":method");
        CHECK(pseudo[0].value == "GET");
        CHECK(pseudo[1].name == ":scheme");
        CHECK(pseudo[1].value == "https");
        CHECK(pseudo[2].name == ":authority");
        CHECK(pseudo[2].value == "example.com");
        CHECK(pseudo[3].name == ":path");
        CHECK(pseudo[3].value == "/api/test");
    }

    SUBCASE("Request all headers") {
        http3::Request req;
        req.method = "POST";
        req.scheme = "https";
        req.authority = "example.com";
        req.path = "/api/submit";
        req.headers.push_back({"content-type", "application/json"});
        req.headers.push_back({"user-agent", "netpipe/1.0"});

        auto all = req.get_all_headers();
        CHECK(all.size() == 6); // 4 pseudo + 2 regular
        CHECK(all[4].name == "content-type");
        CHECK(all[5].name == "user-agent");
    }

    SUBCASE("Response pseudo-headers") {
        http3::Response resp;
        resp.status = 200;

        auto pseudo = resp.get_pseudo_headers();
        CHECK(pseudo.size() == 1);
        CHECK(pseudo[0].name == ":status");
        CHECK(pseudo[0].value == "200");
    }

    SUBCASE("Response all headers") {
        http3::Response resp;
        resp.status = 404;
        resp.headers.push_back({"content-type", "text/html"});

        auto all = resp.get_all_headers();
        CHECK(all.size() == 2);
        CHECK(all[0].name == ":status");
        CHECK(all[0].value == "404");
        CHECK(all[1].name == "content-type");
    }
}

TEST_CASE("QPACK Encoding/Decoding") {
    SUBCASE("QPACK static table") {
        // Verify static table is properly defined
        CHECK(http3::QPACK_STATIC_TABLE_SIZE > 0);

        // Check some well-known entries
        CHECK(http3::QPACK_STATIC_TABLE[0].name == ":authority");
        CHECK(http3::QPACK_STATIC_TABLE[1].name == ":path");
        CHECK(http3::QPACK_STATIC_TABLE[1].value == "/");
    }

    SUBCASE("QPACK encoder - indexed static") {
        http3::QpackEncoder encoder;

        // Encode a header that fully matches static table (e.g., :path = /)
        http3::HeaderList headers;
        headers.push_back({":path", "/"});

        auto encoded = encoder.encode(headers);
        CHECK(!encoded.empty());

        // Should have: Required Insert Count (1 byte) + Delta Base (1 byte) + indexed reference
        CHECK(encoded.size() >= 3);

        // First two bytes are RIC and Delta Base (both 0)
        CHECK(encoded[0] == 0x00);
        CHECK(encoded[1] == 0x00);
    }

    SUBCASE("QPACK encoder - literal with name reference") {
        http3::QpackEncoder encoder;

        // Encode a header where name is in static table but value is different
        http3::HeaderList headers;
        headers.push_back({":path", "/api/test"});

        auto encoded = encoder.encode(headers);
        CHECK(!encoded.empty());
        CHECK(encoded.size() >= 3);
    }

    SUBCASE("QPACK encoder - literal") {
        http3::QpackEncoder encoder;

        // Encode a custom header not in static table
        http3::HeaderList headers;
        headers.push_back({"x-custom-header", "custom-value"});

        auto encoded = encoder.encode(headers);
        CHECK(!encoded.empty());
    }

    SUBCASE("QPACK encoder/decoder round-trip - simple") {
        http3::QpackEncoder encoder;
        http3::QpackDecoder decoder;

        http3::HeaderList headers;
        headers.push_back({":method", "GET"});
        headers.push_back({":path", "/"});

        auto encoded = encoder.encode(headers);
        CHECK(!encoded.empty());

        auto decoded_result = decoder.decode(encoded);
        REQUIRE(decoded_result.is_ok());

        auto &decoded = decoded_result.value();
        CHECK(decoded.size() == 2);
        CHECK(decoded[0].name == ":method");
        CHECK(decoded[0].value == "GET");
        CHECK(decoded[1].name == ":path");
        CHECK(decoded[1].value == "/");
    }

    SUBCASE("QPACK encoder/decoder round-trip - mixed") {
        http3::QpackEncoder encoder;
        http3::QpackDecoder decoder;

        http3::HeaderList headers;
        headers.push_back({":method", "POST"});
        headers.push_back({":scheme", "https"});
        headers.push_back({":authority", "example.com"});
        headers.push_back({":path", "/api/submit"});
        headers.push_back({"content-type", "application/json"});
        headers.push_back({"x-request-id", "abc123"});

        auto encoded = encoder.encode(headers);
        CHECK(!encoded.empty());

        auto decoded_result = decoder.decode(encoded);
        REQUIRE(decoded_result.is_ok());

        auto &decoded = decoded_result.value();
        CHECK(decoded.size() == 6);
        CHECK(decoded[0].name == ":method");
        CHECK(decoded[0].value == "POST");
        CHECK(decoded[4].name == "content-type");
        CHECK(decoded[4].value == "application/json");
        CHECK(decoded[5].name == "x-request-id");
        CHECK(decoded[5].value == "abc123");
    }

    SUBCASE("QPACK decoder - error on truncated data") {
        http3::QpackDecoder decoder;

        // Too short
        dp::Vector<dp::u8> data = {0x00};
        auto result = decoder.decode(data);
        CHECK(result.is_err());
    }

    SUBCASE("QPACK encoder - request headers") {
        http3::QpackEncoder encoder;
        http3::QpackDecoder decoder;

        http3::Request req;
        req.method = "GET";
        req.scheme = "https";
        req.authority = "example.com";
        req.path = "/index.html";
        req.headers.push_back({"accept", "*/*"});
        req.headers.push_back({"accept-encoding", "gzip, deflate, br"});

        auto all_headers = req.get_all_headers();
        auto encoded = encoder.encode(all_headers);

        auto decoded_result = decoder.decode(encoded);
        REQUIRE(decoded_result.is_ok());

        auto &decoded = decoded_result.value();
        CHECK(decoded.size() == 6);

        // Verify pseudo-headers
        CHECK(decoded[0].name == ":method");
        CHECK(decoded[0].value == "GET");
        CHECK(decoded[1].name == ":scheme");
        CHECK(decoded[1].value == "https");
        CHECK(decoded[2].name == ":authority");
        CHECK(decoded[2].value == "example.com");
        CHECK(decoded[3].name == ":path");
        CHECK(decoded[3].value == "/index.html");
    }

    SUBCASE("QPACK encoder - response headers") {
        http3::QpackEncoder encoder;
        http3::QpackDecoder decoder;

        http3::Response resp;
        resp.status = 200;
        resp.headers.push_back({"content-type", "text/html; charset=utf-8"});
        resp.headers.push_back({"cache-control", "no-cache"});

        auto all_headers = resp.get_all_headers();
        auto encoded = encoder.encode(all_headers);

        auto decoded_result = decoder.decode(encoded);
        REQUIRE(decoded_result.is_ok());

        auto &decoded = decoded_result.value();
        CHECK(decoded.size() == 3);
        CHECK(decoded[0].name == ":status");
        CHECK(decoded[0].value == "200");
        CHECK(decoded[1].name == "content-type");
        CHECK(decoded[2].name == "cache-control");
        CHECK(decoded[2].value == "no-cache");
    }
}

TEST_CASE("HTTP/3 Type Exports") {
    SUBCASE("Main namespace type aliases") {
        // Verify types are exported to netpipe namespace
        netpipe::Http3Request req;
        req.method = "GET";
        CHECK(req.method == "GET");

        netpipe::Http3Response resp;
        resp.status = 200;
        CHECK(resp.status == 200);

        netpipe::Http3Settings settings;
        settings.qpack_max_table_capacity = 1024;
        CHECK(settings.qpack_max_table_capacity == 1024);

        netpipe::Http3Connection conn(true);
        CHECK(conn.is_client() == true);
    }
}

TEST_CASE("HTTP/3 Connection") {
    SUBCASE("Client connection initialization") {
        http3::Connection conn(true);

        CHECK(conn.is_client() == true);
        CHECK(conn.state() == http3::ConnectionState::Idle);

        auto init_result = conn.initialize();
        REQUIRE(init_result.is_ok());

        auto &init_data = init_result.value();
        CHECK(!init_data.empty());
        CHECK(conn.state() == http3::ConnectionState::Connecting);

        // Should be a SETTINGS frame
        CHECK(init_data[0] == 0x04); // SETTINGS frame type
    }

    SUBCASE("Server connection initialization") {
        http3::Connection conn(false);

        CHECK(conn.is_client() == false);
        CHECK(conn.state() == http3::ConnectionState::Idle);

        auto init_result = conn.initialize();
        REQUIRE(init_result.is_ok());

        CHECK(conn.state() == http3::ConnectionState::Connecting);
    }

    SUBCASE("Settings exchange") {
        http3::Connection client(true);
        http3::Connection server(false);

        // Both initialize
        auto client_init = client.initialize().value();
        auto server_init = server.initialize().value();

        // Exchange settings
        auto client_result = client.process_control_data(server_init);
        REQUIRE(client_result.is_ok());
        CHECK(client.state() == http3::ConnectionState::Connected);

        auto server_result = server.process_control_data(client_init);
        REQUIRE(server_result.is_ok());
        CHECK(server.state() == http3::ConnectionState::Connected);
    }

    SUBCASE("Cannot create request before connected") {
        http3::Connection client(true);
        client.initialize();

        auto stream_result = client.create_request_stream();
        CHECK(stream_result.is_err());
    }

    SUBCASE("Server cannot create request streams") {
        http3::Connection client(true);
        http3::Connection server(false);

        auto client_init = client.initialize().value();
        auto server_init = server.initialize().value();
        client.process_control_data(server_init);
        server.process_control_data(client_init);

        auto stream_result = server.create_request_stream();
        CHECK(stream_result.is_err());
    }

    SUBCASE("Client creates request stream") {
        http3::Connection client(true);
        http3::Connection server(false);

        auto client_init = client.initialize().value();
        auto server_init = server.initialize().value();
        client.process_control_data(server_init);
        server.process_control_data(client_init);

        auto stream_result = client.create_request_stream();
        REQUIRE(stream_result.is_ok());

        auto stream_id = stream_result.value();
        CHECK(stream_id == 0); // First client-initiated bidi stream

        // Create another
        auto stream2_result = client.create_request_stream();
        REQUIRE(stream2_result.is_ok());
        CHECK(stream2_result.value() == 4); // Second client-initiated bidi stream
    }

    SUBCASE("Encode and decode request") {
        http3::Connection client(true);
        http3::Connection server(false);

        auto client_init = client.initialize().value();
        auto server_init = server.initialize().value();
        client.process_control_data(server_init);
        server.process_control_data(client_init);

        // Create request
        auto stream_id = client.create_request_stream().value();

        http3::Request req;
        req.method = "GET";
        req.scheme = "https";
        req.authority = "example.com";
        req.path = "/api/test";
        req.headers.push_back(http3::HeaderField("user-agent", "netpipe/1.0"));

        auto encoded_result = client.encode_request(stream_id, req);
        REQUIRE(encoded_result.is_ok());

        auto &encoded = encoded_result.value();
        CHECK(!encoded.empty());

        // Server processes the request
        auto process_result = server.process_request_stream(stream_id, encoded);
        REQUIRE(process_result.is_ok());

        // Mark stream as finished
        server.stream_finished(stream_id);

        // Server can retrieve request
        auto request_opt = server.get_request(stream_id);
        REQUIRE(request_opt.has_value());

        auto &received_req = request_opt.value();
        CHECK(received_req.method == "GET");
        CHECK(received_req.scheme == "https");
        CHECK(received_req.authority == "example.com");
        CHECK(received_req.path == "/api/test");
    }

    SUBCASE("Encode and decode response") {
        http3::Connection client(true);
        http3::Connection server(false);

        auto client_init = client.initialize().value();
        auto server_init = server.initialize().value();
        client.process_control_data(server_init);
        server.process_control_data(client_init);

        // Client sends request
        auto stream_id = client.create_request_stream().value();
        http3::Request req;
        req.method = "GET";
        req.scheme = "https";
        req.authority = "example.com";
        req.path = "/";
        auto req_data = client.encode_request(stream_id, req).value();
        server.process_request_stream(stream_id, req_data);

        // Server sends response
        http3::Response resp;
        resp.status = 200;
        resp.headers.push_back(http3::HeaderField("content-type", "text/html"));

        auto resp_data = server.encode_response(stream_id, resp).value();
        CHECK(!resp_data.empty());

        // Client processes response
        auto process_result = client.process_request_stream(stream_id, resp_data);
        REQUIRE(process_result.is_ok());

        auto response_opt = client.get_response(stream_id);
        REQUIRE(response_opt.has_value());

        auto &received_resp = response_opt.value();
        CHECK(received_resp.status == 200);
    }

    SUBCASE("Request with body") {
        http3::Connection client(true);
        http3::Connection server(false);

        auto client_init = client.initialize().value();
        auto server_init = server.initialize().value();
        client.process_control_data(server_init);
        server.process_control_data(client_init);

        auto stream_id = client.create_request_stream().value();

        http3::Request req;
        req.method = "POST";
        req.scheme = "https";
        req.authority = "example.com";
        req.path = "/api/submit";
        req.headers.push_back(http3::HeaderField("content-type", "application/json"));

        auto headers_data = client.encode_request(stream_id, req).value();

        dp::Vector<dp::u8> body = {'{', '"', 'k', 'e', 'y', '"', ':', '"', 'v', 'a', 'l', '"', '}'};
        auto body_data = client.encode_data(stream_id, body).value();

        // Server receives headers and body
        server.process_request_stream(stream_id, headers_data);
        server.process_request_stream(stream_id, body_data);

        auto request_opt = server.get_request(stream_id);
        REQUIRE(request_opt.has_value());

        auto &received = request_opt.value();
        CHECK(received.method == "POST");
        CHECK(received.body == body);
    }

    SUBCASE("GOAWAY frame") {
        http3::Connection client(true);
        http3::Connection server(false);

        auto client_init = client.initialize().value();
        auto server_init = server.initialize().value();
        client.process_control_data(server_init);
        server.process_control_data(client_init);

        // Server sends GOAWAY
        auto goaway_data = server.create_goaway(4);
        CHECK(!goaway_data.empty());
        CHECK(server.state() == http3::ConnectionState::GoingAway);

        // Client receives GOAWAY
        auto result = client.process_control_data(goaway_data);
        REQUIRE(result.is_ok());

        CHECK(client.goaway_received() == true);
        CHECK(client.goaway_stream_id() == 4);
        CHECK(client.state() == http3::ConnectionState::GoingAway);
    }

    SUBCASE("Active streams tracking") {
        http3::Connection client(true);
        http3::Connection server(false);

        auto client_init = client.initialize().value();
        auto server_init = server.initialize().value();
        client.process_control_data(server_init);
        server.process_control_data(client_init);

        auto stream1 = client.create_request_stream().value();
        auto stream2 = client.create_request_stream().value();

        auto active = client.active_streams();
        CHECK(active.size() == 2);

        client.close_stream(stream1);
        active = client.active_streams();
        CHECK(active.size() == 1);
        CHECK(active[0] == stream2);
    }

    SUBCASE("Cannot encode headers twice") {
        http3::Connection client(true);
        http3::Connection server(false);

        auto client_init = client.initialize().value();
        auto server_init = server.initialize().value();
        client.process_control_data(server_init);
        server.process_control_data(client_init);

        auto stream_id = client.create_request_stream().value();

        http3::Request req;
        req.method = "GET";
        req.path = "/";

        auto first = client.encode_request(stream_id, req);
        CHECK(first.is_ok());

        auto second = client.encode_request(stream_id, req);
        CHECK(second.is_err());
    }

    SUBCASE("Cannot send data before headers") {
        http3::Connection client(true);
        http3::Connection server(false);

        auto client_init = client.initialize().value();
        auto server_init = server.initialize().value();
        client.process_control_data(server_init);
        server.process_control_data(client_init);

        auto stream_id = client.create_request_stream().value();

        dp::Vector<dp::u8> body = {'t', 'e', 's', 't'};
        auto result = client.encode_data(stream_id, body);
        CHECK(result.is_err());
    }
}

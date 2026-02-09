#include <doctest/doctest.h>

#include <netpipe/security/tls/extensions.hpp>
#include <netpipe/security/tls/handshake.hpp>

TEST_CASE("TLS ALPN extension roundtrip") {
    netpipe::tls::AlpnExtension ext;
    ext.protocols = {"h2", "http/1.1"};

    auto wire = ext.serialize();
    CHECK(wire.type == netpipe::tls::ExtensionType::ApplicationLayerProtocolNegotiation);

    auto parsed = netpipe::tls::AlpnExtension::parse(wire.data);
    REQUIRE(parsed.is_ok());
    REQUIRE(parsed.value().protocols.size() == 2);
    CHECK(parsed.value().protocols[0] == "h2");
    CHECK(parsed.value().protocols[1] == "http/1.1");
}

TEST_CASE("TLS ALPN negotiation chooses server preference") {
    dp::Vector<dp::String> client = {"http/1.1", "h2"};
    dp::Vector<dp::String> server = {"h2", "http/1.1"};

    auto selected = netpipe::tls::AlpnExtension::negotiate(client, server);
    REQUIRE(selected.has_value());
    CHECK(selected.value() == "h2");
}

TEST_CASE("TLS handshake config carries ALPN offers") {
    netpipe::tls::HandshakeConfig cfg;
    cfg.alpn_protocols = {"h2", "http/1.1"};

    netpipe::tls::Handshake hs(netpipe::tls::Role::Client, cfg);
    auto ch_record = hs.create_client_hello();
    CHECK(ch_record.is_ok());
}

#include <doctest/doctest.h>

#include <netpipe/protocol/http_selector.hpp>

TEST_CASE("HTTP selector chooses HTTP/3 on QUIC") {
    netpipe::http::SelectorConfig config;
    netpipe::http::NegotiatedCapabilities caps;
    caps.quic_transport = true;

    auto selected = netpipe::http::select_version(config, caps);
    REQUIRE(selected.is_ok());
    CHECK(selected.value() == netpipe::http::Version::Http3);
}

TEST_CASE("HTTP selector uses ALPN to choose HTTP/2") {
    netpipe::http::SelectorConfig config;
    netpipe::http::NegotiatedCapabilities caps;
    caps.tls_active = true;
    caps.alpn_protocol = dp::String("h2");

    auto selected = netpipe::http::select_version(config, caps);
    REQUIRE(selected.is_ok());
    CHECK(selected.value() == netpipe::http::Version::Http2);
}

TEST_CASE("HTTP selector falls back to HTTP/1.1") {
    netpipe::http::SelectorConfig config;
    config.allow_http3 = false;
    config.allow_http2 = false;

    netpipe::http::NegotiatedCapabilities caps;
    auto selected = netpipe::http::select_version(config, caps);
    REQUIRE(selected.is_ok());
    CHECK(selected.value() == netpipe::http::Version::Http1);
}

TEST_CASE("HTTP selector enforces preferred version constraints") {
    netpipe::http::SelectorConfig config;
    config.preferred = netpipe::http::Version::Http2;

    netpipe::http::NegotiatedCapabilities caps;
    caps.tls_active = false;

    auto selected = netpipe::http::select_version(config, caps);
    CHECK(selected.is_err());
}

TEST_CASE("HTTP selector factory helpers follow selected version") {
    netpipe::http::ProtocolSelector selector;

    netpipe::http::NegotiatedCapabilities caps;
    caps.tls_active = true;
    caps.alpn_protocol = dp::String("h2");
    selector.set_capabilities(caps);

    auto h2_mgr = selector.create_http2_stream_manager();
    CHECK(h2_mgr.is_ok());

    auto h2_conn = selector.create_http2_connection(true);
    CHECK(h2_conn.is_ok());
    CHECK(h2_conn.value().is_client());

    auto h11_client = selector.create_http1_client();
    CHECK(h11_client.is_err());
}

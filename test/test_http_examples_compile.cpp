#include <doctest/doctest.h>

#include <netpipe/protocol/http_selector.hpp>

TEST_CASE("HTTP examples API compile smoke") {
    netpipe::http::ProtocolSelector selector;

    netpipe::http::NegotiatedCapabilities caps11;
    caps11.alpn_protocol = dp::String("http/1.1");
    selector.set_capabilities(caps11);
    auto h11 = selector.create_http11_client();
    CHECK(h11.is_ok());

    netpipe::http11::Request req;
    req.target = "/";
    auto encoded = h11.value().encode_request(req);
    CHECK(encoded.is_ok());

    netpipe::http::NegotiatedCapabilities caps2;
    caps2.tls_active = true;
    caps2.alpn_protocol = dp::String("h2");
    selector.set_capabilities(caps2);
    auto h2 = selector.create_http2_stream_manager();
    CHECK(h2.is_ok());
}

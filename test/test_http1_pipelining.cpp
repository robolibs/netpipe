#include <doctest/doctest.h>

#include <netpipe/protocol/http1/pipelining.hpp>

TEST_CASE("HTTP/1.1 server pipelining preserves request order") {
    netpipe::http1::ClientConnection raw_client;
    netpipe::http1::PipelinedServerConnection server;

    netpipe::http1::Request r1;
    r1.method = netpipe::http::Method::Get;
    r1.target = "/a";
    netpipe::http1::set_header(r1.headers, "Host", "localhost");

    netpipe::http1::Request r2;
    r2.method = netpipe::http::Method::Get;
    r2.target = "/b";
    netpipe::http1::set_header(r2.headers, "Host", "localhost");

    auto w1 = raw_client.encode_request(r1);
    auto w2 = raw_client.encode_request(r2);
    REQUIRE(w1.is_ok());
    REQUIRE(w2.is_ok());

    netpipe::Message combined = w1.value();
    combined.insert(combined.end(), w2.value().begin(), w2.value().end());

    REQUIRE(server.feed_data(combined).is_ok());
    REQUIRE(server.has_request());
    auto p1 = server.pop_request();
    REQUIRE(p1.is_ok());
    CHECK(p1.value().target == "/a");

    REQUIRE(server.has_request());
    auto p2 = server.pop_request();
    REQUIRE(p2.is_ok());
    CHECK(p2.value().target == "/b");
}

TEST_CASE("HTTP/1.1 pipelined responses map to request methods") {
    netpipe::http1::ClientConnection raw_client;
    netpipe::http1::PipelinedServerConnection server;
    netpipe::http1::PipelinedClientConnection client;

    netpipe::http1::Request head_req;
    head_req.method = netpipe::http::Method::Head;
    head_req.target = "/meta";
    netpipe::http1::set_header(head_req.headers, "Host", "localhost");

    netpipe::http1::Request get_req;
    get_req.method = netpipe::http::Method::Get;
    get_req.target = "/data";
    netpipe::http1::set_header(get_req.headers, "Host", "localhost");

    auto wr1 = raw_client.encode_request(head_req);
    auto wr2 = raw_client.encode_request(get_req);
    REQUIRE(wr1.is_ok());
    REQUIRE(wr2.is_ok());

    netpipe::Message piped = wr1.value();
    piped.insert(piped.end(), wr2.value().begin(), wr2.value().end());
    REQUIRE(server.feed_data(piped).is_ok());
    REQUIRE(server.pop_request().is_ok());
    REQUIRE(server.pop_request().is_ok());

    netpipe::http1::Response head_resp;
    head_resp.status_code = 200;
    head_resp.body = dp::Vector<dp::u8>{'x'};

    netpipe::http1::Response get_resp;
    get_resp.status_code = 200;
    get_resp.body = dp::Vector<dp::u8>{'o', 'k'};

    auto ws1 = server.encode_next_response(head_resp);
    auto ws2 = server.encode_next_response(get_resp);
    REQUIRE(ws1.is_ok());
    REQUIRE(ws2.is_ok());

    REQUIRE(client.encode_request(head_req).is_ok());
    REQUIRE(client.encode_request(get_req).is_ok());
    REQUIRE(client.feed_data(ws1.value()).is_ok());
    REQUIRE(client.feed_data(ws2.value()).is_ok());

    auto c1 = client.pop_response();
    auto c2 = client.pop_response();
    REQUIRE(c1.is_ok());
    REQUIRE(c2.is_ok());
    CHECK(c1.value().body.empty());
    CHECK(c2.value().body.size() == 2);
}

TEST_CASE("HTTP/1.1 upgrade path marks pipelined connection upgraded") {
    netpipe::http1::ClientConnection raw_client;
    netpipe::http1::PipelinedServerConnection server;
    netpipe::http1::PipelinedClientConnection client;

    netpipe::http1::Request up_req;
    up_req.method = netpipe::http::Method::Get;
    up_req.target = "/chat";
    netpipe::http1::set_header(up_req.headers, "Host", "localhost");
    netpipe::http1::set_header(up_req.headers, "Connection", "Upgrade");
    netpipe::http1::set_header(up_req.headers, "Upgrade", "websocket");

    auto wire_req = raw_client.encode_request(up_req);
    REQUIRE(wire_req.is_ok());
    REQUIRE(server.feed_data(wire_req.value()).is_ok());
    auto parsed_req = server.pop_request();
    REQUIRE(parsed_req.is_ok());
    CHECK(netpipe::http1::is_upgrade_request(parsed_req.value()));
    REQUIRE(netpipe::http1::requested_upgrade_protocol(parsed_req.value()).has_value());

    auto switch_resp = netpipe::http1::make_switching_protocols_response("websocket");
    auto wire_resp = server.encode_next_response(switch_resp);
    REQUIRE(wire_resp.is_ok());
    CHECK(server.upgraded());
    REQUIRE(server.upgraded_protocol().has_value());
    CHECK(server.upgraded_protocol().value() == "websocket");

    REQUIRE(client.encode_request(up_req).is_ok());
    REQUIRE(client.feed_data(wire_resp.value()).is_ok());
    auto parsed_resp = client.pop_response();
    REQUIRE(parsed_resp.is_ok());
    CHECK(netpipe::http1::is_switching_protocols_response(parsed_resp.value()));
    REQUIRE(netpipe::http1::negotiated_upgrade_protocol(parsed_resp.value()).has_value());
    CHECK(client.upgraded());
    CHECK(client.encode_request(up_req).is_err());
}

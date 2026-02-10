#include <doctest/doctest.h>

#include <netpipe/protocol/http1/connection.hpp>

TEST_CASE("HTTP/1.1 client/server connection roundtrip with content-length") {
    netpipe::http1::ClientConnection client;
    netpipe::http1::ServerConnection server;

    netpipe::http1::Request request;
    request.method = netpipe::http::Method::Post;
    request.target = "/echo";
    netpipe::http1::set_header(request.headers, "Host", "localhost");
    request.body = dp::Vector<dp::u8>{'h', 'i'};

    auto wire_request = client.encode_request(request);
    CHECK(wire_request.is_ok());

    auto decoded_request = server.decode_request(wire_request.value());
    CHECK(decoded_request.is_ok());
    CHECK(decoded_request.value().body.size() == 2);

    server.set_last_request_method(decoded_request.value().method);
    netpipe::http1::Response response;
    response.status_code = 200;
    response.reason = "OK";
    response.body = dp::Vector<dp::u8>{'o', 'k'};

    auto wire_response = server.encode_response(response);
    CHECK(wire_response.is_ok());

    auto decoded_response = client.decode_response(wire_response.value());
    CHECK(decoded_response.is_ok());
    CHECK(decoded_response.value().status_code == 200);
    CHECK(decoded_response.value().body == response.body);
}

TEST_CASE("HTTP/1.1 connection keep-alive reacts to Connection close") {
    netpipe::http1::ClientConnection client;
    netpipe::http1::ServerConnection server;

    netpipe::http1::Request request;
    request.method = netpipe::http::Method::Get;
    request.target = "/";
    netpipe::http1::set_header(request.headers, "Host", "localhost");
    auto wire_request = client.encode_request(request);
    CHECK(wire_request.is_ok());

    auto decoded_request = server.decode_request(wire_request.value());
    CHECK(decoded_request.is_ok());

    server.set_last_request_method(decoded_request.value().method);
    netpipe::http1::Response response;
    response.status_code = 200;
    netpipe::http1::set_header(response.headers, "Connection", "close");

    auto wire_response = server.encode_response(response);
    CHECK(wire_response.is_ok());

    auto decoded_response = client.decode_response(wire_response.value());
    CHECK(decoded_response.is_ok());
    CHECK(client.keep_alive() == false);
}

TEST_CASE("HTTP/1.1 connection carries chunked trailers") {
    netpipe::http1::ClientConnection client;
    netpipe::http1::ServerConnection server;

    netpipe::http1::Request request;
    request.method = netpipe::http::Method::Post;
    request.target = "/upload";
    netpipe::http1::set_header(request.headers, "Host", "localhost");
    netpipe::http1::set_header(request.headers, "Transfer-Encoding", "chunked");
    request.body = dp::Vector<dp::u8>{'a', 'b', 'c'};
    request.trailers = {{"X-Checksum", "abc"}};

    auto wire = client.encode_request(request);
    REQUIRE(wire.is_ok());
    auto decoded = server.decode_request(wire.value());
    REQUIRE(decoded.is_ok());
    REQUIRE(decoded.value().trailers.size() == 1);
    CHECK(decoded.value().trailers[0].name == "X-Checksum");
    CHECK(decoded.value().trailers[0].value == "abc");
}

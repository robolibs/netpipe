#include <doctest/doctest.h>

#include <netpipe/protocol/http11/parser.hpp>

TEST_CASE("HTTP/1.1 request-line parsing") {
    netpipe::http11::Request request;
    auto result = netpipe::http11::parse_request_line("GET /status HTTP/1.1", request);
    CHECK(result.is_ok());
    CHECK(request.target == "/status");
    CHECK(netpipe::http::to_string(request.method) == "GET");
}

TEST_CASE("HTTP/1.1 status-line parsing") {
    netpipe::http11::Response response;
    auto result = netpipe::http11::parse_status_line("HTTP/1.1 204 No Content", response);
    CHECK(result.is_ok());
    CHECK(response.status_code == 204);
    CHECK(response.reason == "No Content");
}

TEST_CASE("HTTP/1.1 header parsing and lookup") {
    auto header = netpipe::http11::parse_header_line("Content-Type: application/json");
    CHECK(header.is_ok());
    CHECK(header.value().name == "Content-Type");
    CHECK(header.value().value == "application/json");

    netpipe::http::HeaderList headers;
    headers.push_back(header.value());
    auto found = netpipe::http11::find_header_value(headers, "content-type");
    CHECK(found.has_value());
    CHECK(found.value() == "application/json");
}

TEST_CASE("HTTP/1.1 request head parser rejects obs-fold") {
    auto parsed = netpipe::http11::parse_request_head("GET / HTTP/1.1\r\n"
                                                      "Host: example.com\r\n"
                                                      " User-Agent: bad-fold\r\n"
                                                      "\r\n");
    CHECK(parsed.is_err());
}

TEST_CASE("HTTP/1.1 parse full request and response heads") {
    auto request = netpipe::http11::parse_request_head("POST /v1/items HTTP/1.1\r\n"
                                                       "Host: api.example.com\r\n"
                                                       "Content-Type: application/json\r\n"
                                                       "\r\n");
    CHECK(request.is_ok());
    CHECK(request.value().headers.size() == 2);

    auto response = netpipe::http11::parse_response_head("HTTP/1.1 200 OK\r\n"
                                                         "Content-Length: 0\r\n"
                                                         "Connection: keep-alive\r\n"
                                                         "\r\n");
    CHECK(response.is_ok());
    CHECK(response.value().status_code == 200);
    CHECK(response.value().headers.size() == 2);
}

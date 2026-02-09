#include <doctest/doctest.h>

#include <netpipe/protocol/http11/serialize.hpp>

TEST_CASE("HTTP/1.1 request serialization roundtrip") {
    netpipe::http11::Request request;
    request.method = netpipe::http::Method::Post;
    request.target = "/v1/items";
    netpipe::http11::set_header(request.headers, "Host", "api.example.com");
    netpipe::http11::set_content_type(request.headers, "application/json");

    auto serialized = netpipe::http11::serialize_request_head(request);
    CHECK(serialized.is_ok());

    auto parsed = netpipe::http11::parse_request_head(serialized.value());
    CHECK(parsed.is_ok());
    CHECK(parsed.value().target == request.target);
    CHECK(netpipe::http::to_string(parsed.value().method) == "POST");
    CHECK(parsed.value().headers.size() == 2);
}

TEST_CASE("HTTP/1.1 response serialization fills reason phrase") {
    netpipe::http11::Response response;
    response.status_code = 404;
    response.reason.clear();
    netpipe::http11::set_content_length(response.headers, 0);

    auto serialized = netpipe::http11::serialize_response_head(response);
    CHECK(serialized.is_ok());

    auto parsed = netpipe::http11::parse_response_head(serialized.value());
    CHECK(parsed.is_ok());
    CHECK(parsed.value().status_code == 404);
    CHECK(parsed.value().reason == "Not Found");
}

TEST_CASE("HTTP/1.1 header helper replaces case-insensitively") {
    netpipe::http::HeaderList headers;
    netpipe::http11::set_header(headers, "Content-Type", "text/plain");
    netpipe::http11::set_header(headers, "content-type", "application/json");

    CHECK(headers.size() == 1);
    CHECK(headers[0].value == "application/json");
}

#include <doctest/doctest.h>

#include <netpipe/protocol/http1/serialize.hpp>

TEST_CASE("HTTP/1.1 request serialization roundtrip") {
    netpipe::http1::Request request;
    request.method = netpipe::http::Method::Post;
    request.target = "/v1/items";
    netpipe::http1::set_header(request.headers, "Host", "api.example.com");
    netpipe::http1::set_content_type(request.headers, "application/json");

    auto serialized = netpipe::http1::serialize_request_head(request);
    CHECK(serialized.is_ok());

    auto parsed = netpipe::http1::parse_request_head(serialized.value());
    CHECK(parsed.is_ok());
    CHECK(parsed.value().target == request.target);
    CHECK(netpipe::http::to_string(parsed.value().method) == "POST");
    CHECK(parsed.value().headers.size() == 2);
}

TEST_CASE("HTTP/1.1 response serialization fills reason phrase") {
    netpipe::http1::Response response;
    response.status_code = 404;
    response.reason.clear();
    netpipe::http1::set_content_length(response.headers, 0);

    auto serialized = netpipe::http1::serialize_response_head(response);
    CHECK(serialized.is_ok());

    auto parsed = netpipe::http1::parse_response_head(serialized.value());
    CHECK(parsed.is_ok());
    CHECK(parsed.value().status_code == 404);
    CHECK(parsed.value().reason == "Not Found");
}

TEST_CASE("HTTP/1.1 header helper replaces case-insensitively") {
    netpipe::http::HeaderList headers;
    netpipe::http1::set_header(headers, "Content-Type", "text/plain");
    netpipe::http1::set_header(headers, "content-type", "application/json");

    CHECK(headers.size() == 1);
    CHECK(headers[0].value == "application/json");
}

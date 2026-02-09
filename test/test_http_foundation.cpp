#include <doctest/doctest.h>

#include <netpipe/protocol/http/common.hpp>
#include <netpipe/protocol/http11.hpp>
#include <netpipe/protocol/http2.hpp>

TEST_CASE("HTTP foundation method conversion") {
    auto method = netpipe::http::parse_method("POST");
    CHECK(method.is_ok());
    CHECK(netpipe::http::to_string(method.value()) == "POST");

    auto invalid = netpipe::http::parse_method("BREW");
    CHECK(invalid.is_err());
}

TEST_CASE("HTTP foundation status helpers") {
    CHECK(netpipe::http::validate_status_code(200).is_ok());
    CHECK(netpipe::http::validate_status_code(99).is_err());
    CHECK(netpipe::http::reason_phrase(404) == "Not Found");
}

TEST_CASE("HTTP11 scaffold validates basic request and response") {
    netpipe::http11::Request request;
    request.method = netpipe::http::Method::Get;
    request.target = "/health";

    CHECK(netpipe::http11::validate_request(request).is_ok());

    netpipe::http11::Response response;
    response.status_code = 204;
    response.reason = netpipe::http::reason_phrase(response.status_code);

    CHECK(netpipe::http11::validate_response(response).is_ok());
}

TEST_CASE("HTTP2 scaffold validates pseudo-header requirements") {
    netpipe::http2::Request request;
    request.authority = "api.example.com";
    request.path = "/v1/ping";

    CHECK(netpipe::http2::validate_request(request).is_ok());

    request.authority.clear();
    CHECK(netpipe::http2::validate_request(request).is_err());

    netpipe::http2::Response response;
    response.status_code = 200;
    CHECK(netpipe::http2::validate_response(response).is_ok());
}

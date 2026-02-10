#pragma once

#include <netpipe/protocol/http/common.hpp>

namespace netpipe::http11 {

    inline constexpr const char *VERSION = "HTTP/1.1";

    struct Request {
        http::Method method = http::Method::Get;
        dp::String target = "/";
        http::HeaderList headers;
        dp::Vector<dp::u8> body;
        http::HeaderList trailers;
    };

    struct Response {
        dp::u16 status_code = 200;
        dp::String reason = "OK";
        http::HeaderList headers;
        dp::Vector<dp::u8> body;
        http::HeaderList trailers;
    };

    inline dp::Result<void> validate_request(const Request &request) {
        if (request.target.empty()) {
            return dp::result::err(http::error::protocol_error("request target cannot be empty"));
        }
        return dp::result::ok();
    }

    inline dp::Result<void> validate_response(const Response &response) {
        auto status_result = http::validate_status_code(response.status_code);
        if (status_result.is_err()) {
            return dp::result::err(status_result.error());
        }
        return dp::result::ok();
    }

} // namespace netpipe::http11

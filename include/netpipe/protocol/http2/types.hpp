#pragma once

#include <netpipe/protocol/http/common.hpp>

namespace netpipe::http2 {

    inline constexpr const char *PREFACE = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

    enum class ConnectionState { Idle, PrefaceSent, PrefaceReceived, SettingsExchanged, Closed };

    struct Request {
        http::Method method = http::Method::Get;
        dp::String scheme = "https";
        dp::String authority;
        dp::String path = "/";
        http::HeaderList headers;
        dp::Vector<dp::u8> body;
    };

    struct Response {
        dp::u16 status_code = 200;
        http::HeaderList headers;
        dp::Vector<dp::u8> body;
    };

    inline dp::Result<void> validate_request(const Request &request) {
        if (request.authority.empty()) {
            return dp::result::err(http::error::protocol_error(":authority is required for HTTP/2 request"));
        }
        if (request.path.empty()) {
            return dp::result::err(http::error::protocol_error(":path is required for HTTP/2 request"));
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

} // namespace netpipe::http2

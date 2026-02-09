#pragma once

#include <netpipe/protocol/http11/parser.hpp>

namespace netpipe::http11 {

    inline void set_header(http::HeaderList &headers, const dp::String &name, const dp::String &value) {
        for (auto &header : headers) {
            if (header_name_equals(header.name, name)) {
                header.value = value;
                return;
            }
        }
        headers.push_back(http::HeaderField{name, value});
    }

    inline void set_content_type(http::HeaderList &headers, const dp::String &content_type) {
        set_header(headers, "Content-Type", content_type);
    }

    inline void set_content_length(http::HeaderList &headers, dp::usize content_length) {
        set_header(headers, "Content-Length", std::to_string(content_length).c_str());
    }

    inline dp::String serialize_headers(const http::HeaderList &headers) {
        dp::String out;
        for (const auto &header : headers) {
            out += header.name;
            out += ": ";
            out += header.value;
            out += "\r\n";
        }
        return out;
    }

    inline dp::Result<dp::String> serialize_request_head(const Request &request) {
        auto valid = validate_request(request);
        if (valid.is_err()) {
            return dp::result::err(valid.error());
        }

        dp::String out;
        out += http::to_string(request.method);
        out += " ";
        out += request.target;
        out += " ";
        out += VERSION;
        out += "\r\n";
        out += serialize_headers(request.headers);
        out += "\r\n";
        return dp::result::ok(std::move(out));
    }

    inline dp::Result<dp::String> serialize_response_head(const Response &response) {
        auto valid = validate_response(response);
        if (valid.is_err()) {
            return dp::result::err(valid.error());
        }

        dp::String reason = response.reason.empty() ? http::reason_phrase(response.status_code) : response.reason;

        dp::String out;
        out += VERSION;
        out += " ";
        out += std::to_string(response.status_code).c_str();
        out += " ";
        out += reason;
        out += "\r\n";
        out += serialize_headers(response.headers);
        out += "\r\n";
        return dp::result::ok(std::move(out));
    }

} // namespace netpipe::http11

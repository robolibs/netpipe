#pragma once

#include <datapod/datapod.hpp>

namespace netpipe::http {

    enum class Method { Get, Head, Post, Put, Delete, Connect, Options, Trace, Patch };

    struct HeaderField {
        dp::String name;
        dp::String value;

        HeaderField() = default;
        HeaderField(const dp::String &n, const dp::String &v) : name(n), value(v) {}
        HeaderField(dp::String &&n, dp::String &&v) : name(std::move(n)), value(std::move(v)) {}
    };

    using HeaderList = dp::Vector<HeaderField>;

    inline dp::String to_string(Method method) {
        switch (method) {
        case Method::Get:
            return "GET";
        case Method::Head:
            return "HEAD";
        case Method::Post:
            return "POST";
        case Method::Put:
            return "PUT";
        case Method::Delete:
            return "DELETE";
        case Method::Connect:
            return "CONNECT";
        case Method::Options:
            return "OPTIONS";
        case Method::Trace:
            return "TRACE";
        case Method::Patch:
            return "PATCH";
        }
        return "GET";
    }

    inline dp::Result<Method> parse_method(const dp::String &method) {
        if (method == "GET")
            return dp::result::ok(Method::Get);
        if (method == "HEAD")
            return dp::result::ok(Method::Head);
        if (method == "POST")
            return dp::result::ok(Method::Post);
        if (method == "PUT")
            return dp::result::ok(Method::Put);
        if (method == "DELETE")
            return dp::result::ok(Method::Delete);
        if (method == "CONNECT")
            return dp::result::ok(Method::Connect);
        if (method == "OPTIONS")
            return dp::result::ok(Method::Options);
        if (method == "TRACE")
            return dp::result::ok(Method::Trace);
        if (method == "PATCH")
            return dp::result::ok(Method::Patch);
        return dp::result::err(dp::Error::invalid_argument("unsupported HTTP method"));
    }

    inline dp::Result<void> validate_status_code(dp::u16 status_code) {
        if (status_code < 100 || status_code > 999) {
            return dp::result::err(dp::Error::invalid_argument("invalid HTTP status code"));
        }
        return dp::result::ok();
    }

    inline dp::String reason_phrase(dp::u16 status_code) {
        switch (status_code) {
        case 100:
            return "Continue";
        case 101:
            return "Switching Protocols";
        case 200:
            return "OK";
        case 201:
            return "Created";
        case 204:
            return "No Content";
        case 301:
            return "Moved Permanently";
        case 302:
            return "Found";
        case 304:
            return "Not Modified";
        case 400:
            return "Bad Request";
        case 401:
            return "Unauthorized";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 408:
            return "Request Timeout";
        case 409:
            return "Conflict";
        case 413:
            return "Payload Too Large";
        case 429:
            return "Too Many Requests";
        case 500:
            return "Internal Server Error";
        case 501:
            return "Not Implemented";
        case 502:
            return "Bad Gateway";
        case 503:
            return "Service Unavailable";
        case 504:
            return "Gateway Timeout";
        default:
            return "Unknown";
        }
    }

    namespace error {

        inline dp::Error protocol_error(const dp::String &detail) {
            return dp::Error::invalid_argument(dp::String("HTTP protocol error: ") + detail);
        }

        inline dp::Error invalid_header(const dp::String &name) {
            return dp::Error::invalid_argument(dp::String("invalid HTTP header: ") + name);
        }

        inline dp::Error unsupported_feature(const dp::String &feature) {
            return dp::Error::invalid_argument(dp::String("HTTP feature not implemented: ") + feature);
        }

        inline dp::Error state_error(const dp::String &detail) {
            return dp::Error::invalid_argument(dp::String("HTTP state error: ") + detail);
        }

    } // namespace error

} // namespace netpipe::http

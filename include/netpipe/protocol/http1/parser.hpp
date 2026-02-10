#pragma once

#include <cctype>

#include <netpipe/protocol/http1/types.hpp>

namespace netpipe::http1 {

    // RFC coverage notes (smoke-level, not full conformance):
    // - Request/status line tokenization and HTTP/1.1 version checks
    // - Header field-name validation via RFC7230 tchar subset
    // - CRLF-terminated header block parsing
    // - Optional lenient mode for LF normalization and obs-fold acceptance

    struct ParseOptions {
        bool strict = true;
        bool allow_obs_fold = false;
        bool accept_lf_line_endings = false;
    };

    dp::Result<Request> parse_request_head_with_options(const dp::String &head, const ParseOptions &options);
    dp::Result<Response> parse_response_head_with_options(const dp::String &head, const ParseOptions &options);

    inline dp::String header_name_to_lower(const dp::String &name) {
        dp::String out;
        out.reserve(name.size());
        for (dp::usize i = 0; i < name.size(); ++i) {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(name[i]))));
        }
        return out;
    }

    inline bool header_name_equals(const dp::String &lhs, const dp::String &rhs) {
        if (lhs.size() != rhs.size()) {
            return false;
        }

        for (dp::usize i = 0; i < lhs.size(); ++i) {
            auto a = static_cast<char>(std::tolower(static_cast<unsigned char>(lhs[i])));
            auto b = static_cast<char>(std::tolower(static_cast<unsigned char>(rhs[i])));
            if (a != b) {
                return false;
            }
        }

        return true;
    }

    inline dp::Optional<dp::String> find_header_value(const http::HeaderList &headers, const dp::String &name) {
        for (const auto &header : headers) {
            if (header_name_equals(header.name, name)) {
                return header.value;
            }
        }
        return dp::nullopt;
    }

    inline bool is_valid_header_name_char(char c) {
        // RFC 7230 tchar subset used for field-name validation
        if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
            return true;
        }
        switch (c) {
        case '!':
        case '#':
        case '$':
        case '%':
        case '&':
        case '\'':
        case '*':
        case '+':
        case '-':
        case '.':
        case '^':
        case '_':
        case '`':
        case '|':
        case '~':
            return true;
        default:
            return false;
        }
    }

    inline dp::String trim_ows(const dp::String &value) {
        dp::usize start = 0;
        dp::usize end = value.size();

        while (start < end && (value[start] == ' ' || value[start] == '\t')) {
            ++start;
        }
        while (end > start && (value[end - 1] == ' ' || value[end - 1] == '\t')) {
            --end;
        }

        return value.substr(start, end - start);
    }

    inline dp::Result<http::HeaderField> parse_header_line(const dp::String &line) {
        auto colon = line.find(':');
        if (colon == dp::String::npos || colon == 0) {
            return dp::result::err(http::error::invalid_header(line));
        }

        auto name = line.substr(0, colon);
        for (dp::usize i = 0; i < name.size(); ++i) {
            if (!is_valid_header_name_char(name[i])) {
                return dp::result::err(http::error::invalid_header(name));
            }
        }

        auto value = trim_ows(line.substr(colon + 1));
        return dp::result::ok(http::HeaderField{std::move(name), std::move(value)});
    }

    inline dp::Result<void> parse_request_line(const dp::String &line, Request &request) {
        auto first_sp = line.find(' ');
        if (first_sp == dp::String::npos) {
            return dp::result::err(http::error::protocol_error("malformed request-line"));
        }

        auto second_sp = line.find(' ', first_sp + 1);
        if (second_sp == dp::String::npos || second_sp == first_sp + 1) {
            return dp::result::err(http::error::protocol_error("malformed request-line"));
        }

        auto method_text = line.substr(0, first_sp);
        auto target = line.substr(first_sp + 1, second_sp - first_sp - 1);
        auto version = line.substr(second_sp + 1);

        if (version != VERSION) {
            return dp::result::err(http::error::protocol_error("unsupported HTTP version in request-line"));
        }

        auto method = http::parse_method(method_text);
        if (method.is_err()) {
            return dp::result::err(method.error());
        }

        request.method = method.value();
        request.target = std::move(target);
        return dp::result::ok();
    }

    inline dp::Result<void> parse_status_line(const dp::String &line, Response &response) {
        if (line.size() < 12) {
            return dp::result::err(http::error::protocol_error("malformed status-line"));
        }

        if (line.substr(0, 8) != VERSION) {
            return dp::result::err(http::error::protocol_error("unsupported HTTP version in status-line"));
        }

        if (line[8] != ' ') {
            return dp::result::err(http::error::protocol_error("malformed status-line"));
        }

        if (!std::isdigit(static_cast<unsigned char>(line[9])) || !std::isdigit(static_cast<unsigned char>(line[10])) ||
            !std::isdigit(static_cast<unsigned char>(line[11]))) {
            return dp::result::err(http::error::protocol_error("invalid status code"));
        }

        dp::u16 status = static_cast<dp::u16>((line[9] - '0') * 100 + (line[10] - '0') * 10 + (line[11] - '0'));
        auto status_result = http::validate_status_code(status);
        if (status_result.is_err()) {
            return dp::result::err(status_result.error());
        }

        response.status_code = status;
        if (line.size() > 12) {
            if (line[12] != ' ') {
                return dp::result::err(http::error::protocol_error("malformed status-line reason phrase"));
            }
            response.reason = line.substr(13);
        } else {
            response.reason = http::reason_phrase(status);
        }

        return dp::result::ok();
    }

    inline dp::Result<Request> parse_request_head(const dp::String &head) {
        return parse_request_head_with_options(head, ParseOptions{});
    }

    inline dp::Result<dp::String> canonicalize_head_line_endings(const dp::String &head, const ParseOptions &options) {
        if (!options.accept_lf_line_endings) {
            return dp::result::ok(head);
        }

        dp::String normalized;
        normalized.reserve(head.size() + 8);
        for (dp::usize i = 0; i < head.size(); ++i) {
            if (head[i] == '\n' && (i == 0 || head[i - 1] != '\r')) {
                normalized += "\r\n";
            } else {
                normalized.push_back(head[i]);
            }
        }
        return dp::result::ok(std::move(normalized));
    }

    inline dp::Result<Request> parse_request_head_with_options(const dp::String &head, const ParseOptions &options) {
        auto canonical = canonicalize_head_line_endings(head, options);
        if (canonical.is_err()) {
            return dp::result::err(canonical.error());
        }

        const auto &input = canonical.value();

        auto line_end = input.find("\r\n");
        if (line_end == dp::String::npos) {
            return dp::result::err(http::error::protocol_error("request head missing CRLF"));
        }

        Request request;
        auto request_line = input.substr(0, line_end);
        auto line_result = parse_request_line(request_line, request);
        if (line_result.is_err()) {
            return dp::result::err(line_result.error());
        }

        dp::usize pos = line_end + 2;
        while (pos < input.size()) {
            auto next = input.find("\r\n", pos);
            if (next == dp::String::npos) {
                return dp::result::err(http::error::protocol_error("header block missing CRLF terminator"));
            }

            if (next == pos) {
                return dp::result::ok(std::move(request));
            }

            auto header_line = input.substr(pos, next - pos);
            if (header_line[0] == ' ' || header_line[0] == '\t') {
                if (!options.allow_obs_fold || request.headers.empty()) {
                    return dp::result::err(http::error::protocol_error("obs-fold headers are not supported"));
                }
                request.headers.back().value += " ";
                request.headers.back().value += trim_ows(header_line);
                pos = next + 2;
                continue;
            }

            auto header = parse_header_line(header_line);
            if (header.is_err()) {
                return dp::result::err(header.error());
            }

            request.headers.push_back(std::move(header.value()));
            pos = next + 2;
        }

        return dp::result::err(http::error::protocol_error("header block not terminated"));
    }

    inline dp::Result<Response> parse_response_head(const dp::String &head) {
        return parse_response_head_with_options(head, ParseOptions{});
    }

    inline dp::Result<Response> parse_response_head_with_options(const dp::String &head, const ParseOptions &options) {
        auto canonical = canonicalize_head_line_endings(head, options);
        if (canonical.is_err()) {
            return dp::result::err(canonical.error());
        }

        const auto &input = canonical.value();

        auto line_end = input.find("\r\n");
        if (line_end == dp::String::npos) {
            return dp::result::err(http::error::protocol_error("response head missing CRLF"));
        }

        Response response;
        auto status_line = input.substr(0, line_end);
        auto line_result = parse_status_line(status_line, response);
        if (line_result.is_err()) {
            return dp::result::err(line_result.error());
        }

        dp::usize pos = line_end + 2;
        while (pos < input.size()) {
            auto next = input.find("\r\n", pos);
            if (next == dp::String::npos) {
                return dp::result::err(http::error::protocol_error("header block missing CRLF terminator"));
            }

            if (next == pos) {
                return dp::result::ok(std::move(response));
            }

            auto header_line = input.substr(pos, next - pos);
            if (header_line[0] == ' ' || header_line[0] == '\t') {
                if (!options.allow_obs_fold || response.headers.empty()) {
                    return dp::result::err(http::error::protocol_error("obs-fold headers are not supported"));
                }
                response.headers.back().value += " ";
                response.headers.back().value += trim_ows(header_line);
                pos = next + 2;
                continue;
            }

            auto header = parse_header_line(header_line);
            if (header.is_err()) {
                return dp::result::err(header.error());
            }

            response.headers.push_back(std::move(header.value()));
            pos = next + 2;
        }

        return dp::result::err(http::error::protocol_error("header block not terminated"));
    }

} // namespace netpipe::http1

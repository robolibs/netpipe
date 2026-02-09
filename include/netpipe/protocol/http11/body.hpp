#pragma once

#include <netpipe/protocol/http11/serialize.hpp>

namespace netpipe::http11 {

    enum class BodyKind { None, ContentLength, Chunked };

    inline BodyKind body_kind_from_headers(const http::HeaderList &headers) {
        auto te = find_header_value(headers, "transfer-encoding");
        if (te.has_value()) {
            auto lower = header_name_to_lower(te.value());
            if (lower.find("chunked") != dp::String::npos) {
                return BodyKind::Chunked;
            }
        }

        auto cl = find_header_value(headers, "content-length");
        if (cl.has_value()) {
            return BodyKind::ContentLength;
        }

        return BodyKind::None;
    }

    inline bool response_must_not_have_body(dp::u16 status_code, http::Method request_method = http::Method::Get) {
        if (request_method == http::Method::Head) {
            return true;
        }
        if (status_code >= 100 && status_code < 200) {
            return true;
        }
        if (status_code == 204 || status_code == 304) {
            return true;
        }
        return false;
    }

    inline bool should_keep_alive(const http::HeaderList &headers) {
        auto conn = find_header_value(headers, "connection");
        if (!conn.has_value()) {
            return true;
        }

        auto lower = header_name_to_lower(conn.value());
        if (lower.find("close") != dp::String::npos) {
            return false;
        }
        if (lower.find("keep-alive") != dp::String::npos) {
            return true;
        }
        return true;
    }

    inline dp::Result<dp::usize> parse_content_length(const http::HeaderList &headers) {
        auto cl = find_header_value(headers, "content-length");
        if (!cl.has_value()) {
            return dp::result::ok(dp::usize(0));
        }

        if (cl.value().empty()) {
            return dp::result::err(http::error::protocol_error("empty Content-Length header"));
        }

        dp::usize value = 0;
        for (dp::usize i = 0; i < cl.value().size(); ++i) {
            char c = cl.value()[i];
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                return dp::result::err(http::error::protocol_error("invalid Content-Length header"));
            }
            value = value * 10 + static_cast<dp::usize>(c - '0');
        }

        return dp::result::ok(value);
    }

    inline dp::Result<dp::Vector<dp::u8>> decode_content_length_body(const http::HeaderList &headers,
                                                                     const dp::Vector<dp::u8> &payload) {
        auto length_result = parse_content_length(headers);
        if (length_result.is_err()) {
            return dp::result::err(length_result.error());
        }

        auto expected = length_result.value();
        if (payload.size() < expected) {
            return dp::result::err(http::error::protocol_error("incomplete body for Content-Length"));
        }

        return dp::result::ok(dp::Vector<dp::u8>(payload.begin(), payload.begin() + expected));
    }

    inline dp::Result<dp::Vector<dp::u8>> encode_chunked_body(const dp::Vector<dp::u8> &body) {
        dp::String out;
        out += std::to_string(body.size()).c_str();
        out += "\r\n";
        out.append(reinterpret_cast<const char *>(body.data()), body.size());
        out += "\r\n0\r\n\r\n";
        return dp::result::ok(dp::Vector<dp::u8>(out.begin(), out.end()));
    }

    inline dp::Result<dp::Vector<dp::u8>> decode_chunked_body(const dp::Vector<dp::u8> &payload) {
        dp::Vector<dp::u8> out;
        dp::usize pos = 0;

        while (pos < payload.size()) {
            dp::usize line_end = pos;
            while (line_end + 1 < payload.size() && !(payload[line_end] == '\r' && payload[line_end + 1] == '\n')) {
                ++line_end;
            }
            if (line_end + 1 >= payload.size()) {
                return dp::result::err(http::error::protocol_error("invalid chunk size line"));
            }

            dp::String size_text(reinterpret_cast<const char *>(payload.data() + pos), line_end - pos);
            if (size_text.empty()) {
                return dp::result::err(http::error::protocol_error("empty chunk size"));
            }

            dp::usize chunk_size = 0;
            for (dp::usize i = 0; i < size_text.size(); ++i) {
                char c = size_text[i];
                chunk_size *= 16;
                if (c >= '0' && c <= '9') {
                    chunk_size += static_cast<dp::usize>(c - '0');
                } else if (c >= 'a' && c <= 'f') {
                    chunk_size += static_cast<dp::usize>(10 + c - 'a');
                } else if (c >= 'A' && c <= 'F') {
                    chunk_size += static_cast<dp::usize>(10 + c - 'A');
                } else {
                    return dp::result::err(http::error::protocol_error("invalid chunk size"));
                }
            }

            pos = line_end + 2;

            if (chunk_size == 0) {
                if (pos + 2 > payload.size() || payload[pos] != '\r' || payload[pos + 1] != '\n') {
                    return dp::result::err(http::error::protocol_error("invalid final chunk terminator"));
                }
                return dp::result::ok(std::move(out));
            }

            if (pos + chunk_size + 2 > payload.size()) {
                return dp::result::err(http::error::protocol_error("incomplete chunk data"));
            }

            out.insert(out.end(), payload.begin() + pos, payload.begin() + pos + chunk_size);
            pos += chunk_size;
            if (payload[pos] != '\r' || payload[pos + 1] != '\n') {
                return dp::result::err(http::error::protocol_error("missing chunk CRLF terminator"));
            }
            pos += 2;
        }

        return dp::result::err(http::error::protocol_error("unterminated chunked body"));
    }

} // namespace netpipe::http11

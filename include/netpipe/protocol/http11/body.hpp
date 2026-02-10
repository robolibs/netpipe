#pragma once

#include <algorithm>

#include <netpipe/protocol/http11/serialize.hpp>

namespace netpipe::http11 {

    enum class BodyKind { None, ContentLength, Chunked };

    struct ChunkedBody {
        dp::Vector<dp::u8> body;
        http::HeaderList trailers;
    };

    inline dp::String to_hex(dp::usize value) {
        if (value == 0) {
            return "0";
        }

        dp::String out;
        while (value > 0) {
            dp::u8 nibble = static_cast<dp::u8>(value & 0xF);
            out.push_back(static_cast<char>(nibble < 10 ? ('0' + nibble) : ('a' + (nibble - 10))));
            value >>= 4;
        }
        std::reverse(out.begin(), out.end());
        return out;
    }

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

    inline dp::Result<dp::Vector<dp::u8>> encode_chunked_body(const dp::Vector<dp::u8> &body,
                                                              const http::HeaderList &trailers = {}) {
        dp::String out;
        out += to_hex(body.size());
        out += "\r\n";
        out.append(reinterpret_cast<const char *>(body.data()), body.size());
        out += "\r\n0\r\n";

        for (const auto &trailer : trailers) {
            out += trailer.name;
            out += ": ";
            out += trailer.value;
            out += "\r\n";
        }

        out += "\r\n";
        return dp::result::ok(dp::Vector<dp::u8>(out.begin(), out.end()));
    }

    inline dp::Result<ChunkedBody> decode_chunked_body_ex(const dp::Vector<dp::u8> &payload) {
        ChunkedBody out;
        dp::usize pos = 0;

        while (pos < payload.size()) {
            dp::usize line_end = pos;
            while (line_end + 1 < payload.size() && !(payload[line_end] == '\r' && payload[line_end + 1] == '\n')) {
                ++line_end;
            }
            if (line_end + 1 >= payload.size()) {
                return dp::result::err(http::error::protocol_error("incomplete chunk size line"));
            }

            dp::String size_text(reinterpret_cast<const char *>(payload.data() + pos), line_end - pos);
            if (size_text.empty()) {
                return dp::result::err(http::error::protocol_error("empty chunk size"));
            }

            auto semi = size_text.find(';');
            if (semi != dp::String::npos) {
                size_text = size_text.substr(0, semi);
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
                while (true) {
                    if (pos + 2 > payload.size()) {
                        return dp::result::err(http::error::protocol_error("incomplete chunk trailer section"));
                    }

                    if (payload[pos] == '\r' && payload[pos + 1] == '\n') {
                        return dp::result::ok(std::move(out));
                    }

                    dp::usize trailer_end = pos;
                    while (trailer_end + 1 < payload.size() &&
                           !(payload[trailer_end] == '\r' && payload[trailer_end + 1] == '\n')) {
                        ++trailer_end;
                    }
                    if (trailer_end + 1 >= payload.size()) {
                        return dp::result::err(http::error::protocol_error("incomplete chunk trailer line"));
                    }

                    dp::String trailer_line(reinterpret_cast<const char *>(payload.data() + pos), trailer_end - pos);
                    auto trailer = parse_header_line(trailer_line);
                    if (trailer.is_err()) {
                        return dp::result::err(trailer.error());
                    }
                    out.trailers.push_back(std::move(trailer.value()));
                    pos = trailer_end + 2;
                }
            }

            if (pos + chunk_size + 2 > payload.size()) {
                return dp::result::err(http::error::protocol_error("incomplete chunk data"));
            }

            out.body.insert(out.body.end(), payload.begin() + pos, payload.begin() + pos + chunk_size);
            pos += chunk_size;
            if (payload[pos] != '\r' || payload[pos + 1] != '\n') {
                return dp::result::err(http::error::protocol_error("missing chunk CRLF terminator"));
            }
            pos += 2;
        }

        return dp::result::err(http::error::protocol_error("unterminated chunked body"));
    }

    inline dp::Result<dp::Vector<dp::u8>> decode_chunked_body(const dp::Vector<dp::u8> &payload) {
        auto parsed = decode_chunked_body_ex(payload);
        if (parsed.is_err()) {
            return dp::result::err(parsed.error());
        }
        return dp::result::ok(std::move(parsed.value().body));
    }

} // namespace netpipe::http11

#pragma once

#include <netpipe/core/common.hpp>
#include <netpipe/protocol/http11/body.hpp>

namespace netpipe::http11 {

    inline dp::Optional<dp::usize> find_header_terminator(const netpipe::Message &buffer, const ParseOptions &options) {
        for (dp::usize i = 0; i + 3 < buffer.size(); ++i) {
            if (buffer[i] == '\r' && buffer[i + 1] == '\n' && buffer[i + 2] == '\r' && buffer[i + 3] == '\n') {
                return i + 4;
            }
        }

        if (options.accept_lf_line_endings) {
            for (dp::usize i = 0; i + 1 < buffer.size(); ++i) {
                if (buffer[i] == '\n' && buffer[i + 1] == '\n') {
                    return i + 2;
                }
            }
        }

        return dp::nullopt;
    }

    inline dp::Result<dp::usize> parse_chunk_size_hex(const dp::String &line) {
        dp::String size_token = line;
        auto semi = line.find(';');
        if (semi != dp::String::npos) {
            size_token = line.substr(0, semi);
        }

        if (size_token.empty()) {
            return dp::result::err(http::error::protocol_error("empty chunk size"));
        }

        dp::usize chunk_size = 0;
        for (dp::usize i = 0; i < size_token.size(); ++i) {
            char c = size_token[i];
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

        return dp::result::ok(chunk_size);
    }

    inline dp::Result<dp::Optional<dp::usize>> find_complete_chunked_message(const netpipe::Message &payload) {
        dp::usize pos = 0;

        while (pos < payload.size()) {
            dp::usize line_end = pos;
            while (line_end + 1 < payload.size() && !(payload[line_end] == '\r' && payload[line_end + 1] == '\n')) {
                ++line_end;
            }
            if (line_end + 1 >= payload.size()) {
                return dp::result::ok(dp::Optional<dp::usize>{});
            }

            dp::String line(reinterpret_cast<const char *>(payload.data() + pos), line_end - pos);
            auto chunk_size = parse_chunk_size_hex(line);
            if (chunk_size.is_err()) {
                return dp::result::err(chunk_size.error());
            }

            pos = line_end + 2;
            if (chunk_size.value() == 0) {
                while (true) {
                    if (pos + 1 >= payload.size()) {
                        return dp::result::ok(dp::Optional<dp::usize>{});
                    }
                    if (payload[pos] == '\r' && payload[pos + 1] == '\n') {
                        return dp::result::ok(dp::Optional<dp::usize>(pos + 2));
                    }

                    dp::usize trailer_end = pos;
                    while (trailer_end + 1 < payload.size() &&
                           !(payload[trailer_end] == '\r' && payload[trailer_end + 1] == '\n')) {
                        ++trailer_end;
                    }
                    if (trailer_end + 1 >= payload.size()) {
                        return dp::result::ok(dp::Optional<dp::usize>{});
                    }
                    pos = trailer_end + 2;
                }
            }

            if (pos + chunk_size.value() + 2 > payload.size()) {
                return dp::result::ok(dp::Optional<dp::usize>{});
            }

            pos += chunk_size.value();
            if (payload[pos] != '\r' || payload[pos + 1] != '\n') {
                return dp::result::err(http::error::protocol_error("missing chunk CRLF terminator"));
            }
            pos += 2;
        }

        return dp::result::ok(dp::Optional<dp::usize>{});
    }

    class IncrementalRequestParser {
      public:
        explicit IncrementalRequestParser(ParseOptions options = {}) : options_(std::move(options)) {}

        void feed(const netpipe::Message &chunk) { buffer_.insert(buffer_.end(), chunk.begin(), chunk.end()); }

        void feed(netpipe::Message &&chunk) {
            buffer_.insert(buffer_.end(), std::make_move_iterator(chunk.begin()), std::make_move_iterator(chunk.end()));
        }

        dp::Result<dp::Optional<Request>> try_parse() {
            auto head_end = find_header_terminator(buffer_, options_);
            if (!head_end.has_value()) {
                return dp::result::ok(dp::Optional<Request>{});
            }

            dp::String head(reinterpret_cast<const char *>(buffer_.data()), head_end.value());
            auto req = parse_request_head_with_options(head, options_);
            if (req.is_err()) {
                return dp::result::err(req.error());
            }

            Request out = req.value();
            netpipe::Message remaining(buffer_.begin() + head_end.value(), buffer_.end());

            auto kind = body_kind_from_headers(out.headers);
            dp::usize consumed_body = 0;
            if (kind == BodyKind::ContentLength) {
                auto length = parse_content_length(out.headers);
                if (length.is_err()) {
                    return dp::result::err(length.error());
                }
                if (remaining.size() < length.value()) {
                    return dp::result::ok(dp::Optional<Request>{});
                }
                out.body = dp::Vector<dp::u8>(remaining.begin(), remaining.begin() + length.value());
                consumed_body = length.value();
            } else if (kind == BodyKind::Chunked) {
                auto complete = find_complete_chunked_message(remaining);
                if (complete.is_err()) {
                    return dp::result::err(complete.error());
                }
                if (!complete.value().has_value()) {
                    return dp::result::ok(dp::Optional<Request>{});
                }

                netpipe::Message chunked_payload(remaining.begin(), remaining.begin() + complete.value().value());
                auto decoded = decode_chunked_body_ex(chunked_payload);
                if (decoded.is_err()) {
                    return dp::result::err(decoded.error());
                }
                out.body = std::move(decoded.value().body);
                out.trailers = std::move(decoded.value().trailers);
                consumed_body = complete.value().value();
            }

            buffer_.erase(buffer_.begin(), buffer_.begin() + head_end.value() + consumed_body);
            return dp::result::ok(dp::Optional<Request>(std::move(out)));
        }

      private:
        ParseOptions options_;
        netpipe::Message buffer_;
    };

    class IncrementalResponseParser {
      public:
        explicit IncrementalResponseParser(ParseOptions options = {}) : options_(std::move(options)) {}

        void feed(const netpipe::Message &chunk) { buffer_.insert(buffer_.end(), chunk.begin(), chunk.end()); }

        void set_last_request_method(http::Method method) { last_request_method_ = method; }

        dp::Result<dp::Optional<Response>> try_parse() {
            auto head_end = find_header_terminator(buffer_, options_);
            if (!head_end.has_value()) {
                return dp::result::ok(dp::Optional<Response>{});
            }

            dp::String head(reinterpret_cast<const char *>(buffer_.data()), head_end.value());
            auto res = parse_response_head_with_options(head, options_);
            if (res.is_err()) {
                return dp::result::err(res.error());
            }

            Response out = res.value();
            netpipe::Message remaining(buffer_.begin() + head_end.value(), buffer_.end());

            dp::usize consumed_body = 0;
            if (!response_must_not_have_body(out.status_code, last_request_method_)) {
                auto kind = body_kind_from_headers(out.headers);
                if (kind == BodyKind::ContentLength) {
                    auto length = parse_content_length(out.headers);
                    if (length.is_err()) {
                        return dp::result::err(length.error());
                    }
                    if (remaining.size() < length.value()) {
                        return dp::result::ok(dp::Optional<Response>{});
                    }
                    out.body = dp::Vector<dp::u8>(remaining.begin(), remaining.begin() + length.value());
                    consumed_body = length.value();
                } else if (kind == BodyKind::Chunked) {
                    auto complete = find_complete_chunked_message(remaining);
                    if (complete.is_err()) {
                        return dp::result::err(complete.error());
                    }
                    if (!complete.value().has_value()) {
                        return dp::result::ok(dp::Optional<Response>{});
                    }

                    netpipe::Message chunked_payload(remaining.begin(), remaining.begin() + complete.value().value());
                    auto decoded = decode_chunked_body_ex(chunked_payload);
                    if (decoded.is_err()) {
                        return dp::result::err(decoded.error());
                    }
                    out.body = std::move(decoded.value().body);
                    out.trailers = std::move(decoded.value().trailers);
                    consumed_body = complete.value().value();
                } else {
                    out.body = remaining;
                    consumed_body = remaining.size();
                }
            }

            buffer_.erase(buffer_.begin(), buffer_.begin() + head_end.value() + consumed_body);
            return dp::result::ok(dp::Optional<Response>(std::move(out)));
        }

      private:
        ParseOptions options_;
        http::Method last_request_method_ = http::Method::Get;
        netpipe::Message buffer_;
    };

} // namespace netpipe::http11

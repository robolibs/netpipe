#pragma once

#include <netpipe/core/common.hpp>
#include <netpipe/protocol/http11/body.hpp>

namespace netpipe::http11 {

    inline dp::Optional<dp::usize> find_head_end(const netpipe::Message &payload) {
        for (dp::usize i = 0; i + 3 < payload.size(); ++i) {
            if (payload[i] == '\r' && payload[i + 1] == '\n' && payload[i + 2] == '\r' && payload[i + 3] == '\n') {
                return i + 4;
            }
        }
        return dp::nullopt;
    }

    inline netpipe::Message to_message(const dp::String &head) { return netpipe::Message(head.begin(), head.end()); }

    class ClientConnection {
      public:
        dp::Result<netpipe::Message> encode_request(const Request &request) {
            Request copy = request;

            if (copy.body.empty()) {
                set_content_length(copy.headers, 0);
            } else if (body_kind_from_headers(copy.headers) != BodyKind::Chunked) {
                set_content_length(copy.headers, copy.body.size());
            }

            auto head = serialize_request_head(copy);
            if (head.is_err()) {
                return dp::result::err(head.error());
            }

            auto message = to_message(head.value());

            if (!copy.body.empty()) {
                if (body_kind_from_headers(copy.headers) == BodyKind::Chunked) {
                    auto encoded = encode_chunked_body(copy.body);
                    if (encoded.is_err()) {
                        return dp::result::err(encoded.error());
                    }
                    message.insert(message.end(), encoded.value().begin(), encoded.value().end());
                } else {
                    message.insert(message.end(), copy.body.begin(), copy.body.end());
                }
            }

            last_request_method_ = copy.method;
            keep_alive_ = should_keep_alive(copy.headers);
            return dp::result::ok(std::move(message));
        }

        dp::Result<Response> decode_response(const netpipe::Message &payload) {
            auto split = find_head_end(payload);
            if (!split.has_value()) {
                return dp::result::err(http::error::protocol_error("response message missing header terminator"));
            }

            dp::String head(reinterpret_cast<const char *>(payload.data()), split.value());
            auto parsed = parse_response_head(head);
            if (parsed.is_err()) {
                return dp::result::err(parsed.error());
            }

            Response response = parsed.value();
            netpipe::Message body(payload.begin() + split.value(), payload.end());

            if (!response_must_not_have_body(response.status_code, last_request_method_)) {
                auto kind = body_kind_from_headers(response.headers);
                if (kind == BodyKind::ContentLength) {
                    auto decoded = decode_content_length_body(response.headers, body);
                    if (decoded.is_err()) {
                        return dp::result::err(decoded.error());
                    }
                    response.body = std::move(decoded.value());
                } else if (kind == BodyKind::Chunked) {
                    auto decoded = decode_chunked_body(body);
                    if (decoded.is_err()) {
                        return dp::result::err(decoded.error());
                    }
                    response.body = std::move(decoded.value());
                } else {
                    response.body = std::move(body);
                }
            }

            keep_alive_ = keep_alive_ && should_keep_alive(response.headers);
            return dp::result::ok(std::move(response));
        }

        bool keep_alive() const { return keep_alive_; }

      private:
        http::Method last_request_method_ = http::Method::Get;
        bool keep_alive_ = true;
    };

    class ServerConnection {
      public:
        dp::Result<Request> decode_request(const netpipe::Message &payload) {
            auto split = find_head_end(payload);
            if (!split.has_value()) {
                return dp::result::err(http::error::protocol_error("request message missing header terminator"));
            }

            dp::String head(reinterpret_cast<const char *>(payload.data()), split.value());
            auto parsed = parse_request_head(head);
            if (parsed.is_err()) {
                return dp::result::err(parsed.error());
            }

            Request request = parsed.value();
            netpipe::Message body(payload.begin() + split.value(), payload.end());

            auto kind = body_kind_from_headers(request.headers);
            if (kind == BodyKind::ContentLength) {
                auto decoded = decode_content_length_body(request.headers, body);
                if (decoded.is_err()) {
                    return dp::result::err(decoded.error());
                }
                request.body = std::move(decoded.value());
            } else if (kind == BodyKind::Chunked) {
                auto decoded = decode_chunked_body(body);
                if (decoded.is_err()) {
                    return dp::result::err(decoded.error());
                }
                request.body = std::move(decoded.value());
            } else {
                request.body = std::move(body);
            }

            keep_alive_ = should_keep_alive(request.headers);
            return dp::result::ok(std::move(request));
        }

        dp::Result<netpipe::Message> encode_response(const Response &response) {
            Response copy = response;

            if (response_must_not_have_body(copy.status_code, last_request_method_)) {
                copy.body.clear();
                set_content_length(copy.headers, 0);
            } else if (copy.body.empty()) {
                set_content_length(copy.headers, 0);
            } else if (body_kind_from_headers(copy.headers) != BodyKind::Chunked) {
                set_content_length(copy.headers, copy.body.size());
            }

            auto head = serialize_response_head(copy);
            if (head.is_err()) {
                return dp::result::err(head.error());
            }

            auto message = to_message(head.value());
            if (!copy.body.empty()) {
                if (body_kind_from_headers(copy.headers) == BodyKind::Chunked) {
                    auto encoded = encode_chunked_body(copy.body);
                    if (encoded.is_err()) {
                        return dp::result::err(encoded.error());
                    }
                    message.insert(message.end(), encoded.value().begin(), encoded.value().end());
                } else {
                    message.insert(message.end(), copy.body.begin(), copy.body.end());
                }
            }

            keep_alive_ = keep_alive_ && should_keep_alive(copy.headers);
            return dp::result::ok(std::move(message));
        }

        void set_last_request_method(http::Method method) { last_request_method_ = method; }
        bool keep_alive() const { return keep_alive_; }

      private:
        http::Method last_request_method_ = http::Method::Get;
        bool keep_alive_ = true;
    };

} // namespace netpipe::http11

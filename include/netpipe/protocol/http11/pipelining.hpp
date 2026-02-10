#pragma once

#include <netpipe/protocol/http11/connection.hpp>
#include <netpipe/protocol/http11/incremental.hpp>

namespace netpipe::http11 {

    inline bool header_value_contains_token(const dp::String &value, const dp::String &token) {
        auto lower_value = header_name_to_lower(value);
        auto lower_token = header_name_to_lower(token);
        return lower_value.find(lower_token) != dp::String::npos;
    }

    inline bool is_upgrade_request(const Request &request) {
        auto connection = find_header_value(request.headers, "connection");
        auto upgrade = find_header_value(request.headers, "upgrade");
        if (!connection.has_value() || !upgrade.has_value()) {
            return false;
        }
        return header_value_contains_token(connection.value(), "upgrade") && !upgrade.value().empty();
    }

    inline dp::Optional<dp::String> requested_upgrade_protocol(const Request &request) {
        if (!is_upgrade_request(request)) {
            return dp::nullopt;
        }
        auto upgrade = find_header_value(request.headers, "upgrade");
        if (!upgrade.has_value() || upgrade.value().empty()) {
            return dp::nullopt;
        }
        return trim_ows(upgrade.value());
    }

    inline bool is_switching_protocols_response(const Response &response) { return response.status_code == 101; }

    inline dp::Optional<dp::String> negotiated_upgrade_protocol(const Response &response) {
        if (!is_switching_protocols_response(response)) {
            return dp::nullopt;
        }
        auto connection = find_header_value(response.headers, "connection");
        auto upgrade = find_header_value(response.headers, "upgrade");
        if (!connection.has_value() || !upgrade.has_value()) {
            return dp::nullopt;
        }
        if (!header_value_contains_token(connection.value(), "upgrade")) {
            return dp::nullopt;
        }
        return trim_ows(upgrade.value());
    }

    inline Response make_switching_protocols_response(const dp::String &protocol) {
        Response response;
        response.status_code = 101;
        response.reason = "Switching Protocols";
        set_header(response.headers, "Connection", "Upgrade");
        set_header(response.headers, "Upgrade", protocol);
        response.body.clear();
        return response;
    }

    class PipelinedServerConnection {
      public:
        explicit PipelinedServerConnection(ParseOptions options = {}) : parser_(std::move(options)) {}

        dp::Result<void> feed_data(const netpipe::Message &chunk) {
            if (upgraded_) {
                return dp::result::err(http::error::state_error("connection is upgraded; HTTP pipelining disabled"));
            }

            parser_.feed(chunk);
            while (true) {
                auto parsed = parser_.try_parse();
                if (parsed.is_err()) {
                    return dp::result::err(parsed.error());
                }
                if (!parsed.value().has_value()) {
                    break;
                }

                request_queue_.push_back(parsed.value().value());
                pending_response_methods_.push_back(parsed.value().value().method);
            }
            return dp::result::ok();
        }

        bool has_request() const { return !request_queue_.empty(); }

        dp::Result<Request> pop_request() {
            if (request_queue_.empty()) {
                return dp::result::err(dp::Error::not_found("no pipelined HTTP/1.1 request available"));
            }

            Request out = std::move(request_queue_.front());
            request_queue_.erase(request_queue_.begin());
            return dp::result::ok(std::move(out));
        }

        dp::Result<netpipe::Message> encode_next_response(const Response &response) {
            if (pending_response_methods_.empty()) {
                return dp::result::err(http::error::state_error("no pending pipelined request for response"));
            }

            auto method = pending_response_methods_.front();
            pending_response_methods_.erase(pending_response_methods_.begin());
            base_.set_last_request_method(method);

            if (upgraded_) {
                return dp::result::err(http::error::state_error("connection is upgraded; cannot encode HTTP response"));
            }

            auto wire = base_.encode_response(response);
            if (wire.is_err()) {
                return dp::result::err(wire.error());
            }

            if (is_switching_protocols_response(response)) {
                auto protocol = negotiated_upgrade_protocol(response);
                if (protocol.has_value()) {
                    upgraded_ = true;
                    upgraded_protocol_ = protocol;
                }
            }

            return wire;
        }

        bool upgraded() const { return upgraded_; }
        dp::Optional<dp::String> upgraded_protocol() const { return upgraded_protocol_; }

      private:
        IncrementalRequestParser parser_;
        ServerConnection base_;
        dp::Vector<Request> request_queue_;
        dp::Vector<http::Method> pending_response_methods_;
        bool upgraded_ = false;
        dp::Optional<dp::String> upgraded_protocol_;
    };

    class PipelinedClientConnection {
      public:
        explicit PipelinedClientConnection(ParseOptions options = {}) : parser_(std::move(options)) {}

        dp::Result<netpipe::Message> encode_request(const Request &request) {
            if (upgraded_) {
                return dp::result::err(
                    http::error::state_error("connection is upgraded; HTTP request encoding disabled"));
            }

            auto wire = base_.encode_request(request);
            if (wire.is_err()) {
                return dp::result::err(wire.error());
            }

            pending_request_methods_.push_back(request.method);
            return wire;
        }

        dp::Result<void> feed_data(const netpipe::Message &chunk) {
            if (upgraded_) {
                return dp::result::err(
                    http::error::state_error("connection is upgraded; HTTP response parsing disabled"));
            }

            parser_.feed(chunk);
            while (true) {
                if (!pending_request_methods_.empty()) {
                    parser_.set_last_request_method(pending_request_methods_.front());
                }

                auto parsed = parser_.try_parse();
                if (parsed.is_err()) {
                    return dp::result::err(parsed.error());
                }
                if (!parsed.value().has_value()) {
                    break;
                }

                response_queue_.push_back(parsed.value().value());
                if (!pending_request_methods_.empty()) {
                    pending_request_methods_.erase(pending_request_methods_.begin());
                }

                if (is_switching_protocols_response(parsed.value().value())) {
                    auto protocol = negotiated_upgrade_protocol(parsed.value().value());
                    if (protocol.has_value()) {
                        upgraded_ = true;
                        upgraded_protocol_ = protocol;
                        break;
                    }
                }
            }

            return dp::result::ok();
        }

        bool has_response() const { return !response_queue_.empty(); }

        dp::Result<Response> pop_response() {
            if (response_queue_.empty()) {
                return dp::result::err(dp::Error::not_found("no pipelined HTTP/1.1 response available"));
            }

            Response out = std::move(response_queue_.front());
            response_queue_.erase(response_queue_.begin());
            return dp::result::ok(std::move(out));
        }

        bool upgraded() const { return upgraded_; }
        dp::Optional<dp::String> upgraded_protocol() const { return upgraded_protocol_; }

      private:
        IncrementalResponseParser parser_;
        ClientConnection base_;
        dp::Vector<http::Method> pending_request_methods_;
        dp::Vector<Response> response_queue_;
        bool upgraded_ = false;
        dp::Optional<dp::String> upgraded_protocol_;
    };

} // namespace netpipe::http11

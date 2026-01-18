#pragma once

#include <algorithm>
#include <chrono>
#include <functional>

#include <datapod/datapod.hpp>
#include <echo/echo.hpp>
#include <netpipe/protocol/http3/frame.hpp>
#include <netpipe/protocol/http3/qpack.hpp>
#include <netpipe/protocol/http3/types.hpp>

namespace netpipe::http3 {

    // HTTP/3 Stream types
    // Control stream: Unidirectional, carries SETTINGS, GOAWAY, etc.
    // QPACK Encoder stream: Unidirectional, carries dynamic table updates
    // QPACK Decoder stream: Unidirectional, carries acknowledgments
    // Request streams: Bidirectional, carries HEADERS + DATA

    // HTTP/3 Connection state
    enum class ConnectionState {
        Idle,
        Connecting, // Waiting for peer SETTINGS
        Connected,  // SETTINGS exchanged
        GoingAway,  // GOAWAY sent or received
        Draining,   // Connection closing, waiting to send final frames
        Closed
    };

    // HTTP/3 Stream state
    enum class Http3StreamState { Idle, Open, HalfClosedLocal, HalfClosedRemote, Closed, Reset };

    // Represents an HTTP/3 request stream
    struct RequestStream {
        dp::u64 stream_id = 0;
        Http3StreamState state = Http3StreamState::Idle;
        Request request;
        Response response;
        bool headers_sent = false;
        bool headers_received = false;
        bool body_complete = false;
        bool trailers_sent = false;
        bool trailers_received = false;
        bool data_started = false; // Have we started sending/receiving DATA frames?
        bool reset_sent = false;
        bool reset_received = false;
        ErrorCode reset_error = ErrorCode::NoError;

        // Flow control tracking
        dp::u64 bytes_sent = 0;
        dp::u64 bytes_received = 0;
        dp::u64 send_limit = 0;    // 0 = no limit set yet
        dp::u64 receive_limit = 0; // 0 = no limit set yet

        // Stream priority (RFC 9218)
        Priority priority;
    };

    // HTTP/3 Connection
    // Manages control streams, QPACK streams, and request streams
    class Connection {
      public:
        explicit Connection(bool is_client) : is_client_(is_client) {
            // Initialize local settings
            local_settings_.qpack_max_table_capacity = 0; // No dynamic table
            local_settings_.max_field_section_size = 16384;
            local_settings_.qpack_blocked_streams = 0;

            // Initialize activity time
            last_activity_time_ms_ = current_time_ms_();
        }

        // Get connection state
        ConnectionState state() const { return state_; }
        bool is_client() const { return is_client_; }
        bool is_connected() const { return state_ == ConnectionState::Connected; }

        // Settings
        const Settings &local_settings() const { return local_settings_; }
        const Settings &peer_settings() const { return peer_settings_; }

        void set_local_settings(const Settings &settings) { local_settings_ = settings; }

        // Enable Extended CONNECT protocol (RFC 9220)
        void enable_extended_connect(bool enable = true) { local_settings_.enable_connect_protocol = enable; }

        // Check if peer supports Extended CONNECT
        bool peer_supports_extended_connect() const { return peer_settings_.enable_connect_protocol; }

        // Initialize connection - returns data to send on control stream
        dp::Res<dp::Vector<dp::u8>> initialize() {
            if (state_ != ConnectionState::Idle) {
                return dp::result::err(dp::Error::invalid_argument("connection already initialized"));
            }

            // Create SETTINGS frame
            SettingsFrame settings_frame;
            settings_frame.settings = local_settings_;

            state_ = ConnectionState::Connecting;
            settings_sent_ = true;

            return dp::result::ok(settings_frame.serialize());
        }

        // Process data received on control stream
        dp::Res<void> process_control_data(const dp::Vector<dp::u8> &data) {
            if (data.empty()) {
                return dp::result::ok();
            }

            dp::usize offset = 0;

            while (offset < data.size()) {
                // Parse frame header
                auto header_result = parse_frame_header(data.data() + offset, data.size() - offset);
                if (header_result.is_err()) {
                    return dp::result::err(header_result.error());
                }

                auto [frame_type, type_len] = header_result.value();
                offset += type_len;

                switch (frame_type) {
                case FrameType::Settings: {
                    if (settings_received_) {
                        return dp::result::err(dp::Error::invalid_argument("duplicate SETTINGS frame"));
                    }

                    auto settings_result = SettingsFrame::parse(data.data() + offset, data.size() - offset);
                    if (settings_result.is_err()) {
                        return dp::result::err(settings_result.error());
                    }

                    auto [frame, consumed] = settings_result.value();

                    // Validate received settings
                    auto validate_result = Settings::validate(frame.settings);
                    if (validate_result.is_err()) {
                        return dp::result::err(validate_result.error());
                    }

                    peer_settings_ = frame.settings;
                    settings_received_ = true;
                    offset += consumed;

                    // Configure QPACK based on peer settings
                    apply_peer_settings();

                    // Transition to connected if both settings exchanged
                    if (settings_sent_ && settings_received_) {
                        state_ = ConnectionState::Connected;
                    }
                    break;
                }

                case FrameType::GoAway: {
                    auto goaway_result = GoAwayFrame::parse(data.data() + offset, data.size() - offset);
                    if (goaway_result.is_err()) {
                        return dp::result::err(goaway_result.error());
                    }

                    auto [frame, consumed] = goaway_result.value();
                    goaway_stream_id_ = frame.stream_id;
                    goaway_received_ = true;
                    state_ = ConnectionState::GoingAway;
                    offset += consumed;
                    break;
                }

                default:
                    // Unknown frame type on control stream - skip
                    // We need to parse the length to skip the frame
                    auto len_result = quic::varint_decode(data.data() + offset, data.size() - offset);
                    if (len_result.is_err()) {
                        return dp::result::err(len_result.error());
                    }
                    auto [length, len_bytes] = len_result.value();
                    offset += len_bytes + length;
                    break;
                }
            }

            return dp::result::ok();
        }

        // Create a new request stream
        dp::Res<dp::u64> create_request_stream() {
            if (!is_client_) {
                return dp::result::err(dp::Error::invalid_argument("only clients can create request streams"));
            }

            if (state_ != ConnectionState::Connected) {
                return dp::result::err(dp::Error::invalid_argument("connection not ready"));
            }

            // Client-initiated bidirectional streams: 0, 4, 8, 12, ...
            dp::u64 stream_id = next_request_stream_id_;
            next_request_stream_id_ += 4;

            RequestStream stream;
            stream.stream_id = stream_id;
            stream.state = Http3StreamState::Open;
            request_streams_[stream_id] = stream;

            return dp::result::ok(stream_id);
        }

        // Encode request headers for sending
        dp::Res<dp::Vector<dp::u8>> encode_request(dp::u64 stream_id, const Request &request) {
            auto it = request_streams_.find(stream_id);
            if (it == request_streams_.end()) {
                return dp::result::err(dp::Error::invalid_argument("unknown stream"));
            }

            if (it->second.headers_sent) {
                return dp::result::err(dp::Error::invalid_argument("headers already sent"));
            }

            // Store request
            it->second.request = request;

            // Encode headers with QPACK
            auto all_headers = request.get_all_headers();
            auto encoded_headers = encoder_.encode(all_headers);

            // Check field section size against peer's limit
            if (exceeds_field_section_limit(encoded_headers.size())) {
                return dp::result::err(dp::Error::invalid_argument("field section exceeds peer's max size"));
            }

            // Create HEADERS frame
            HeadersFrame headers_frame;
            headers_frame.encoded_field_section = encoded_headers;

            it->second.headers_sent = true;

            return dp::result::ok(headers_frame.serialize());
        }

        // Encode request body data
        dp::Res<dp::Vector<dp::u8>> encode_data(dp::u64 stream_id, const dp::Vector<dp::u8> &body) {
            auto it = request_streams_.find(stream_id);
            if (it == request_streams_.end()) {
                return dp::result::err(dp::Error::invalid_argument("unknown stream"));
            }

            if (!it->second.headers_sent) {
                return dp::result::err(dp::Error::invalid_argument("headers not sent"));
            }

            DataFrame data_frame;
            data_frame.data = body;

            return dp::result::ok(data_frame.serialize());
        }

        // Process data received on a request stream
        dp::Res<void> process_request_stream(dp::u64 stream_id, const dp::Vector<dp::u8> &data) {
            // Find or create stream
            auto it = request_streams_.find(stream_id);
            if (it == request_streams_.end()) {
                // New incoming stream (server receiving request)
                RequestStream stream;
                stream.stream_id = stream_id;
                stream.state = Http3StreamState::Open;
                request_streams_[stream_id] = stream;
                it = request_streams_.find(stream_id);
            }

            dp::usize offset = 0;

            while (offset < data.size()) {
                // Parse frame header
                auto header_result = parse_frame_header(data.data() + offset, data.size() - offset);
                if (header_result.is_err()) {
                    return dp::result::err(header_result.error());
                }

                auto [frame_type, type_len] = header_result.value();
                offset += type_len;

                switch (frame_type) {
                case FrameType::Headers: {
                    auto headers_result = HeadersFrame::parse(data.data() + offset, data.size() - offset);
                    if (headers_result.is_err()) {
                        return dp::result::err(headers_result.error());
                    }

                    auto [frame, consumed] = headers_result.value();
                    offset += consumed;

                    // Check field section size against our local limit
                    if (local_settings_.max_field_section_size > 0 &&
                        frame.encoded_field_section.size() > local_settings_.max_field_section_size) {
                        return dp::result::err(dp::Error::invalid_argument("received field section exceeds max size"));
                    }

                    // Decode headers with QPACK
                    auto decoded_result = decoder_.decode(frame.encoded_field_section);
                    if (decoded_result.is_err()) {
                        return dp::result::err(decoded_result.error());
                    }

                    auto &headers = decoded_result.value();

                    // Determine if this is initial headers or trailers
                    if (!it->second.headers_received) {
                        // Initial headers
                        parse_headers(it->second, headers);
                        it->second.headers_received = true;
                    } else if (it->second.data_started && !it->second.trailers_received) {
                        // This is a trailer section (HEADERS after DATA)
                        // Validate: no pseudo-headers allowed in trailers
                        for (const auto &h : headers) {
                            if (!h.name.empty() && h.name[0] == ':') {
                                return dp::result::err(
                                    dp::Error::invalid_argument("pseudo-headers not allowed in trailers"));
                            }
                        }
                        // Store trailers
                        if (is_client_) {
                            it->second.response.trailers = headers;
                        } else {
                            it->second.request.trailers = headers;
                        }
                        it->second.trailers_received = true;
                        echo::debug("HTTP/3 stream ", stream_id, " received trailers with ", headers.size(), " fields");
                    } else {
                        // Unexpected HEADERS frame
                        return dp::result::err(dp::Error::invalid_argument("unexpected HEADERS frame"));
                    }
                    break;
                }

                case FrameType::Data: {
                    auto data_result = DataFrame::parse(data.data() + offset, data.size() - offset);
                    if (data_result.is_err()) {
                        return dp::result::err(data_result.error());
                    }

                    auto [frame, consumed] = data_result.value();
                    offset += consumed;

                    // Mark that data has started (for trailer detection)
                    it->second.data_started = true;

                    // Append to body
                    if (is_client_) {
                        it->second.response.body.insert(it->second.response.body.end(), frame.data.begin(),
                                                        frame.data.end());
                    } else {
                        it->second.request.body.insert(it->second.request.body.end(), frame.data.begin(),
                                                       frame.data.end());
                    }
                    break;
                }

                default:
                    // Unknown frame - skip
                    auto len_result = quic::varint_decode(data.data() + offset, data.size() - offset);
                    if (len_result.is_err()) {
                        return dp::result::err(len_result.error());
                    }
                    auto [length, len_bytes] = len_result.value();
                    offset += len_bytes + length;
                    break;
                }
            }

            return dp::result::ok();
        }

        // Mark stream as complete (FIN received)
        void stream_finished(dp::u64 stream_id) {
            auto it = request_streams_.find(stream_id);
            if (it != request_streams_.end()) {
                it->second.body_complete = true;
                it->second.state = Http3StreamState::HalfClosedRemote;
            }
        }

        // Get request for a stream (server side)
        dp::Optional<Request> get_request(dp::u64 stream_id) const {
            auto it = request_streams_.find(stream_id);
            if (it != request_streams_.end() && it->second.headers_received) {
                return it->second.request;
            }
            return dp::nullopt;
        }

        // Get response for a stream (client side)
        dp::Optional<Response> get_response(dp::u64 stream_id) const {
            auto it = request_streams_.find(stream_id);
            if (it != request_streams_.end() && it->second.headers_received) {
                return it->second.response;
            }
            return dp::nullopt;
        }

        // Encode response headers (server side)
        dp::Res<dp::Vector<dp::u8>> encode_response(dp::u64 stream_id, const Response &response) {
            auto it = request_streams_.find(stream_id);
            if (it == request_streams_.end()) {
                return dp::result::err(dp::Error::invalid_argument("unknown stream"));
            }

            // Store response
            it->second.response = response;

            // Encode headers with QPACK
            auto all_headers = response.get_all_headers();
            auto encoded_headers = encoder_.encode(all_headers);

            // Check field section size against peer's limit
            if (exceeds_field_section_limit(encoded_headers.size())) {
                return dp::result::err(dp::Error::invalid_argument("field section exceeds peer's max size"));
            }

            // Create HEADERS frame
            HeadersFrame headers_frame;
            headers_frame.encoded_field_section = encoded_headers;

            it->second.headers_sent = true;

            return dp::result::ok(headers_frame.serialize());
        }

        // Encode trailer headers for a stream
        // Trailers are sent after all DATA frames
        dp::Res<dp::Vector<dp::u8>> encode_trailers(dp::u64 stream_id, const HeaderList &trailers) {
            auto it = request_streams_.find(stream_id);
            if (it == request_streams_.end()) {
                return dp::result::err(dp::Error::invalid_argument("unknown stream"));
            }

            if (!it->second.headers_sent) {
                return dp::result::err(dp::Error::invalid_argument("headers not sent"));
            }

            if (it->second.trailers_sent) {
                return dp::result::err(dp::Error::invalid_argument("trailers already sent"));
            }

            // Validate trailer headers - no pseudo-headers allowed in trailers
            for (const auto &h : trailers) {
                if (!h.name.empty() && h.name[0] == ':') {
                    return dp::result::err(dp::Error::invalid_argument("pseudo-headers not allowed in trailers"));
                }
            }

            // Encode trailers with QPACK
            auto encoded_trailers = encoder_.encode(trailers);

            // Check field section size against peer's limit
            if (exceeds_field_section_limit(encoded_trailers.size())) {
                return dp::result::err(dp::Error::invalid_argument("trailer section exceeds peer's max size"));
            }

            // Create HEADERS frame for trailers
            HeadersFrame trailer_frame;
            trailer_frame.encoded_field_section = encoded_trailers;

            it->second.trailers_sent = true;

            // Store trailers in the appropriate structure
            if (is_client_) {
                it->second.request.trailers = trailers;
            } else {
                it->second.response.trailers = trailers;
            }

            return dp::result::ok(trailer_frame.serialize());
        }

        // Check if trailers were received for a stream
        bool trailers_received(dp::u64 stream_id) const {
            auto it = request_streams_.find(stream_id);
            if (it == request_streams_.end()) {
                return false;
            }
            return it->second.trailers_received;
        }

        // Get trailers for a request (server side)
        dp::Optional<HeaderList> get_request_trailers(dp::u64 stream_id) const {
            auto it = request_streams_.find(stream_id);
            if (it == request_streams_.end() || !it->second.trailers_received) {
                return dp::nullopt;
            }
            return it->second.request.trailers;
        }

        // Get trailers for a response (client side)
        dp::Optional<HeaderList> get_response_trailers(dp::u64 stream_id) const {
            auto it = request_streams_.find(stream_id);
            if (it == request_streams_.end() || !it->second.trailers_received) {
                return dp::nullopt;
            }
            return it->second.response.trailers;
        }

        // Create GOAWAY frame
        dp::Vector<dp::u8> create_goaway(dp::u64 last_stream_id) {
            GoAwayFrame frame;
            frame.stream_id = last_stream_id;
            goaway_sent_ = true;
            state_ = ConnectionState::GoingAway;
            return frame.serialize();
        }

        // Check if GOAWAY was received
        bool goaway_received() const { return goaway_received_; }
        dp::u64 goaway_stream_id() const { return goaway_stream_id_; }

        // Get all active stream IDs
        dp::Vector<dp::u64> active_streams() const {
            dp::Vector<dp::u64> result;
            for (const auto &[id, stream] : request_streams_) {
                if (stream.state != Http3StreamState::Closed) {
                    result.push_back(id);
                }
            }
            return result;
        }

        // Close a stream
        void close_stream(dp::u64 stream_id) {
            auto it = request_streams_.find(stream_id);
            if (it != request_streams_.end()) {
                it->second.state = Http3StreamState::Closed;
            }
        }

        // =================================================================
        // Stream Priority (RFC 9218)
        // =================================================================

        // Set stream priority
        void set_stream_priority(dp::u64 stream_id, const Priority &priority) {
            auto it = request_streams_.find(stream_id);
            if (it != request_streams_.end()) {
                it->second.priority = priority;
                echo::trace("Stream ", stream_id, " priority set to u=", (int)priority.urgency,
                            ", i=", priority.incremental);
            }
        }

        // Get stream priority
        dp::Optional<Priority> get_stream_priority(dp::u64 stream_id) const {
            auto it = request_streams_.find(stream_id);
            if (it == request_streams_.end()) {
                return dp::nullopt;
            }
            return it->second.priority;
        }

        // Get streams sorted by priority (highest priority first)
        dp::Vector<dp::u64> get_streams_by_priority() const {
            dp::Vector<std::pair<dp::u64, Priority>> streams_with_priority;
            for (const auto &[id, stream] : request_streams_) {
                if (stream.state != Http3StreamState::Closed && stream.state != Http3StreamState::Reset) {
                    streams_with_priority.push_back({id, stream.priority});
                }
            }

            // Sort by priority (lower urgency value = higher priority)
            std::sort(streams_with_priority.begin(), streams_with_priority.end(),
                      [](const auto &a, const auto &b) { return a.second < b.second; });

            dp::Vector<dp::u64> result;
            for (const auto &[id, _] : streams_with_priority) {
                result.push_back(id);
            }
            return result;
        }

        // Set priority from Priority header field value
        dp::Res<void> set_stream_priority_from_header(dp::u64 stream_id, const dp::String &value) {
            auto it = request_streams_.find(stream_id);
            if (it == request_streams_.end()) {
                return dp::result::err(dp::Error::not_found("stream not found"));
            }
            auto priority_result = Priority::parse(value);
            if (priority_result.is_err()) {
                return dp::result::err(priority_result.error());
            }
            it->second.priority = priority_result.value();
            echo::trace("Stream ", stream_id, " priority set from header to u=", (int)priority_result.value().urgency,
                        ", i=", priority_result.value().incremental);
            return dp::result::ok();
        }

        // ==========================================================================
        // Server Push (RFC 9114 Section 4.6)
        // ==========================================================================

        // Client: Set the maximum push ID the server can use
        // Returns MAX_PUSH_ID frame data to send on control stream
        dp::Res<dp::Vector<dp::u8>> send_max_push_id(dp::u64 push_id) {
            if (!is_client_) {
                return dp::result::err(dp::Error::invalid_argument("only clients can send MAX_PUSH_ID"));
            }
            if (max_push_id_sent_ && push_id < max_push_id_) {
                return dp::result::err(dp::Error::invalid_argument("MAX_PUSH_ID cannot decrease"));
            }

            max_push_id_ = push_id;
            max_push_id_sent_ = true;

            MaxPushIdFrame frame;
            frame.push_id = push_id;
            echo::debug("Client sending MAX_PUSH_ID: ", push_id);
            return dp::result::ok(frame.serialize());
        }

        // Client: Cancel a push
        // Returns CANCEL_PUSH frame data to send on control stream
        dp::Res<dp::Vector<dp::u8>> send_cancel_push(dp::u64 push_id) {
            if (!is_client_) {
                return dp::result::err(dp::Error::invalid_argument("only clients can send CANCEL_PUSH"));
            }
            if (push_id > max_push_id_) {
                return dp::result::err(dp::Error::invalid_argument("push_id exceeds MAX_PUSH_ID"));
            }

            cancelled_pushes_.insert(push_id);

            CancelPushFrame frame;
            frame.push_id = push_id;
            echo::debug("Client cancelling push: ", push_id);
            return dp::result::ok(frame.serialize());
        }

        // Get the current MAX_PUSH_ID value
        dp::u64 max_push_id() const { return max_push_id_; }

        // Check if MAX_PUSH_ID has been set
        bool has_max_push_id() const { return max_push_id_sent_; }

        // Server: Check if server can initiate pushes
        bool can_push() const {
            if (is_client_)
                return false;
            return max_push_id_sent_ && next_push_id_ <= max_push_id_;
        }

        // Server: Get the number of remaining push slots
        dp::u64 remaining_push_capacity() const {
            if (!can_push())
                return 0;
            return max_push_id_ - next_push_id_ + 1;
        }

        // Server: Create a PUSH_PROMISE frame
        // associated_stream_id: The request stream that triggered this push
        // Returns (push_id, PUSH_PROMISE frame data)
        dp::Res<std::pair<dp::u64, dp::Vector<dp::u8>>> create_push_promise(dp::u64 associated_stream_id,
                                                                            const Request &promised_request) {

            if (is_client_) {
                return dp::result::err(dp::Error::invalid_argument("only servers can create push promises"));
            }
            if (!max_push_id_sent_) {
                return dp::result::err(dp::Error::invalid_argument("client has not sent MAX_PUSH_ID"));
            }
            if (next_push_id_ > max_push_id_) {
                return dp::result::err(dp::Error::invalid_argument("no push capacity available"));
            }

            // Check associated stream exists
            auto it = request_streams_.find(associated_stream_id);
            if (it == request_streams_.end()) {
                return dp::result::err(dp::Error::not_found("associated stream not found"));
            }

            dp::u64 push_id = next_push_id_++;
            push_promises_[push_id] = promised_request;

            // Encode the promised request headers
            auto all_headers = promised_request.get_all_headers();
            auto encoded = encoder_.encode(all_headers);

            PushPromiseFrame frame;
            frame.push_id = push_id;
            frame.encoded_field_section = encoded;

            echo::debug("Server creating PUSH_PROMISE: push_id=", push_id, " on stream=", associated_stream_id);
            return dp::result::ok(std::make_pair(push_id, frame.serialize()));
        }

        // Check if a push has been cancelled by client
        bool is_push_cancelled(dp::u64 push_id) const {
            return cancelled_pushes_.find(push_id) != cancelled_pushes_.end();
        }

        // Get a promised request by push ID
        dp::Optional<Request> get_push_promise(dp::u64 push_id) const {
            auto it = push_promises_.find(push_id);
            if (it == push_promises_.end()) {
                return dp::nullopt;
            }
            return it->second;
        }

        // Get all active push promises
        dp::Vector<dp::u64> get_active_push_ids() const {
            dp::Vector<dp::u64> ids;
            for (const auto &[id, _] : push_promises_) {
                if (!is_push_cancelled(id)) {
                    ids.push_back(id);
                }
            }
            return ids;
        }

        // Process MAX_PUSH_ID frame received on control stream (server side)
        dp::Res<void> handle_max_push_id(dp::u64 push_id) {
            if (is_client_) {
                return dp::result::err(dp::Error::invalid_argument("client should not receive MAX_PUSH_ID"));
            }
            if (max_push_id_sent_ && push_id < max_push_id_) {
                return dp::result::err(dp::Error::invalid_argument("MAX_PUSH_ID cannot decrease"));
            }

            max_push_id_ = push_id;
            max_push_id_sent_ = true;
            echo::debug("Server received MAX_PUSH_ID: ", push_id);
            return dp::result::ok();
        }

        // Process CANCEL_PUSH frame received on control stream (server side)
        dp::Res<void> handle_cancel_push(dp::u64 push_id) {
            if (is_client_) {
                return dp::result::err(dp::Error::invalid_argument("client should not receive CANCEL_PUSH"));
            }
            if (push_id > max_push_id_) {
                return dp::result::err(dp::Error::invalid_argument("CANCEL_PUSH for unknown push_id"));
            }

            cancelled_pushes_.insert(push_id);
            echo::debug("Server received CANCEL_PUSH: ", push_id);
            return dp::result::ok();
        }

        // Client: Process a received PUSH_PROMISE frame
        dp::Res<Request> handle_push_promise(dp::u64 push_id, const dp::Vector<dp::u8> &encoded_headers) {
            if (!is_client_) {
                return dp::result::err(dp::Error::invalid_argument("server should not receive PUSH_PROMISE"));
            }
            if (!max_push_id_sent_) {
                return dp::result::err(
                    dp::Error::invalid_argument("received PUSH_PROMISE without sending MAX_PUSH_ID"));
            }
            if (push_id > max_push_id_) {
                return dp::result::err(dp::Error::invalid_argument("push_id exceeds MAX_PUSH_ID"));
            }
            if (push_promises_.find(push_id) != push_promises_.end()) {
                return dp::result::err(dp::Error::invalid_argument("duplicate push_id"));
            }

            // Decode headers
            auto decoded = decoder_.decode(encoded_headers);
            if (decoded.is_err()) {
                return dp::result::err(decoded.error());
            }

            Request req;
            for (const auto &h : decoded.value()) {
                if (h.name == ":method") {
                    req.method = h.value;
                } else if (h.name == ":scheme") {
                    req.scheme = h.value;
                } else if (h.name == ":authority") {
                    req.authority = h.value;
                } else if (h.name == ":path") {
                    req.path = h.value;
                } else if (h.name == ":protocol") {
                    req.protocol = h.value;
                } else if (h.name[0] != ':') {
                    req.headers.push_back(h);
                }
            }

            push_promises_[push_id] = req;
            echo::debug("Client received PUSH_PROMISE: push_id=", push_id, " method=", req.method.c_str(),
                        " path=", req.path.c_str());
            return dp::result::ok(std::move(req));
        }

        // Reset a stream with an error code
        // Returns the error code to use with QUIC RESET_STREAM
        dp::Res<dp::u64> reset_stream(dp::u64 stream_id, ErrorCode error = ErrorCode::RequestCancelled) {
            auto it = request_streams_.find(stream_id);
            if (it == request_streams_.end()) {
                return dp::result::err(dp::Error::invalid_argument("unknown stream"));
            }

            if (it->second.state == Http3StreamState::Reset || it->second.state == Http3StreamState::Closed) {
                return dp::result::err(dp::Error::invalid_argument("stream already closed or reset"));
            }

            it->second.state = Http3StreamState::Reset;
            it->second.reset_sent = true;
            it->second.reset_error = error;

            echo::debug("HTTP/3 stream ", stream_id, " reset with error ", static_cast<dp::u64>(error));

            // Return the HTTP/3 error code for use with QUIC RESET_STREAM
            return dp::result::ok(static_cast<dp::u64>(error));
        }

        // Handle incoming stream reset notification from QUIC
        void handle_stream_reset(dp::u64 stream_id, dp::u64 error_code) {
            auto it = request_streams_.find(stream_id);
            if (it == request_streams_.end()) {
                // Unknown stream - create one to track the reset
                RequestStream stream;
                stream.stream_id = stream_id;
                stream.state = Http3StreamState::Reset;
                stream.reset_received = true;
                stream.reset_error = static_cast<ErrorCode>(error_code);
                request_streams_[stream_id] = stream;
                return;
            }

            it->second.state = Http3StreamState::Reset;
            it->second.reset_received = true;
            it->second.reset_error = static_cast<ErrorCode>(error_code);

            echo::debug("HTTP/3 stream ", stream_id, " received reset with error ", error_code);
        }

        // Check if stream was reset
        bool is_stream_reset(dp::u64 stream_id) const {
            auto it = request_streams_.find(stream_id);
            if (it == request_streams_.end()) {
                return false;
            }
            return it->second.state == Http3StreamState::Reset;
        }

        // Get reset error code for a stream
        dp::Optional<ErrorCode> get_stream_reset_error(dp::u64 stream_id) const {
            auto it = request_streams_.find(stream_id);
            if (it == request_streams_.end() || it->second.state != Http3StreamState::Reset) {
                return dp::nullopt;
            }
            return it->second.reset_error;
        }

        // Get stream state
        dp::Optional<Http3StreamState> get_stream_state(dp::u64 stream_id) const {
            auto it = request_streams_.find(stream_id);
            if (it == request_streams_.end()) {
                return dp::nullopt;
            }
            return it->second.state;
        }

        // =================================================================
        // Flow Control
        // =================================================================

        // Update stream send limit (from QUIC MAX_STREAM_DATA)
        void update_stream_send_limit(dp::u64 stream_id, dp::u64 limit) {
            auto it = request_streams_.find(stream_id);
            if (it != request_streams_.end()) {
                it->second.send_limit = limit;
                echo::trace("HTTP/3 stream ", stream_id, " send limit updated to ", limit);
            }
        }

        // Update connection-level send limit (from QUIC MAX_DATA)
        void update_connection_send_limit(dp::u64 limit) {
            connection_send_limit_ = limit;
            echo::trace("HTTP/3 connection send limit updated to ", limit);
        }

        // Check if we can send data on a stream
        bool can_send(dp::u64 stream_id, dp::u64 bytes) const {
            auto it = request_streams_.find(stream_id);
            if (it == request_streams_.end()) {
                return false;
            }

            // Check stream-level limit (if set)
            if (it->second.send_limit > 0) {
                if (it->second.bytes_sent + bytes > it->second.send_limit) {
                    return false;
                }
            }

            // Check connection-level limit (if set)
            if (connection_send_limit_ > 0) {
                if (connection_bytes_sent_ + bytes > connection_send_limit_) {
                    return false;
                }
            }

            return true;
        }

        // Get available send window for a stream
        dp::u64 available_send_window(dp::u64 stream_id) const {
            auto it = request_streams_.find(stream_id);
            if (it == request_streams_.end()) {
                return 0;
            }

            dp::u64 stream_window = dp::u64(-1); // Max if no limit
            dp::u64 connection_window = dp::u64(-1);

            if (it->second.send_limit > 0 && it->second.send_limit > it->second.bytes_sent) {
                stream_window = it->second.send_limit - it->second.bytes_sent;
            } else if (it->second.send_limit > 0) {
                stream_window = 0;
            }

            if (connection_send_limit_ > 0 && connection_send_limit_ > connection_bytes_sent_) {
                connection_window = connection_send_limit_ - connection_bytes_sent_;
            } else if (connection_send_limit_ > 0) {
                connection_window = 0;
            }

            return std::min(stream_window, connection_window);
        }

        // Record bytes sent (to be called after sending data)
        void record_bytes_sent(dp::u64 stream_id, dp::u64 bytes) {
            auto it = request_streams_.find(stream_id);
            if (it != request_streams_.end()) {
                it->second.bytes_sent += bytes;
                connection_bytes_sent_ += bytes;
            }
        }

        // Record bytes received (to be called after receiving data)
        void record_bytes_received(dp::u64 stream_id, dp::u64 bytes) {
            auto it = request_streams_.find(stream_id);
            if (it != request_streams_.end()) {
                it->second.bytes_received += bytes;
                connection_bytes_received_ += bytes;
            }
        }

        // Get bytes sent on a stream
        dp::u64 stream_bytes_sent(dp::u64 stream_id) const {
            auto it = request_streams_.find(stream_id);
            if (it == request_streams_.end()) {
                return 0;
            }
            return it->second.bytes_sent;
        }

        // Get bytes received on a stream
        dp::u64 stream_bytes_received(dp::u64 stream_id) const {
            auto it = request_streams_.find(stream_id);
            if (it == request_streams_.end()) {
                return 0;
            }
            return it->second.bytes_received;
        }

        // Get connection-level bytes sent
        dp::u64 total_bytes_sent() const { return connection_bytes_sent_; }

        // Get connection-level bytes received
        dp::u64 total_bytes_received() const { return connection_bytes_received_; }

        // Check if field section size exceeds limit
        bool exceeds_field_section_limit(dp::usize size) const {
            if (peer_settings_.max_field_section_size == 0) {
                return false; // No limit
            }
            return size > peer_settings_.max_field_section_size;
        }

        // =================================================================
        // Connection Timeout and Draining
        // =================================================================

        // Set idle timeout (in milliseconds, 0 = no timeout)
        void set_idle_timeout(dp::u64 timeout_ms) { idle_timeout_ms_ = timeout_ms; }

        // Set draining timeout (in milliseconds)
        void set_draining_timeout(dp::u64 timeout_ms) { draining_timeout_ms_ = timeout_ms; }

        // Get configured idle timeout
        dp::u64 idle_timeout() const { return idle_timeout_ms_; }

        // Get configured draining timeout
        dp::u64 draining_timeout() const { return draining_timeout_ms_; }

        // Record activity (resets idle timer)
        void record_activity() { last_activity_time_ms_ = current_time_ms_(); }

        // Check if idle timeout has expired
        bool check_idle_timeout() const {
            if (idle_timeout_ms_ == 0) {
                return false; // No timeout configured
            }
            if (state_ == ConnectionState::Closed || state_ == ConnectionState::Draining) {
                return false; // Already closing
            }
            dp::u64 now = current_time_ms_();
            return (now - last_activity_time_ms_) >= idle_timeout_ms_;
        }

        // Start draining the connection
        // Call this after sending GOAWAY or when initiating graceful shutdown
        void start_draining() {
            if (state_ == ConnectionState::Closed) {
                return;
            }
            state_ = ConnectionState::Draining;
            draining_start_time_ms_ = current_time_ms_();
            echo::debug("HTTP/3 connection entering draining state");
        }

        // Check if connection is in draining state
        bool is_draining() const { return state_ == ConnectionState::Draining; }

        // Check if draining period is complete
        bool is_draining_complete() const {
            if (state_ != ConnectionState::Draining) {
                return false;
            }
            dp::u64 now = current_time_ms_();
            return (now - draining_start_time_ms_) >= draining_timeout_ms_;
        }

        // Get time remaining in draining period (in milliseconds)
        dp::u64 draining_time_remaining() const {
            if (state_ != ConnectionState::Draining) {
                return 0;
            }
            dp::u64 now = current_time_ms_();
            dp::u64 elapsed = now - draining_start_time_ms_;
            if (elapsed >= draining_timeout_ms_) {
                return 0;
            }
            return draining_timeout_ms_ - elapsed;
        }

        // Complete the draining and close the connection
        void complete_draining() {
            if (state_ == ConnectionState::Draining) {
                state_ = ConnectionState::Closed;
                echo::debug("HTTP/3 connection draining complete, now closed");
            }
        }

        // Close connection immediately
        void close() { state_ = ConnectionState::Closed; }

        // Check if any streams are still active (not closed/reset)
        bool has_active_streams() const {
            for (const auto &[id, stream] : request_streams_) {
                if (stream.state != Http3StreamState::Closed && stream.state != Http3StreamState::Reset) {
                    return true;
                }
            }
            return false;
        }

        // Get time since last activity (in milliseconds)
        dp::u64 time_since_activity() const {
            dp::u64 now = current_time_ms_();
            return now - last_activity_time_ms_;
        }

        // Set custom time provider (for testing)
        void set_time_provider(std::function<dp::u64()> provider) { current_time_ms_ = std::move(provider); }

        // Encode data with flow control check
        dp::Res<dp::Vector<dp::u8>> encode_data_with_flow_control(dp::u64 stream_id, const dp::Vector<dp::u8> &body) {
            auto it = request_streams_.find(stream_id);
            if (it == request_streams_.end()) {
                return dp::result::err(dp::Error::invalid_argument("unknown stream"));
            }

            if (!it->second.headers_sent) {
                return dp::result::err(dp::Error::invalid_argument("headers not sent"));
            }

            // Check flow control
            if (!can_send(stream_id, body.size())) {
                return dp::result::err(dp::Error::invalid_argument("flow control limit exceeded"));
            }

            DataFrame data_frame;
            data_frame.data = body;

            auto serialized = data_frame.serialize();

            // Record sent bytes (just the body, not the frame overhead)
            record_bytes_sent(stream_id, body.size());

            return dp::result::ok(serialized);
        }

      private:
        // Apply peer settings to configure encoder
        void apply_peer_settings() {
            // Configure encoder's dynamic table capacity based on peer's advertised limit
            if (peer_settings_.qpack_max_table_capacity > 0) {
                // We can use dynamic table up to peer's limit
                // But we shouldn't use more than our own configured limit
                dp::u64 capacity =
                    std::min(peer_settings_.qpack_max_table_capacity, local_settings_.qpack_max_table_capacity);
                if (capacity > 0) {
                    encoder_.set_dynamic_table_capacity(capacity);
                    echo::debug("HTTP/3 encoder dynamic table capacity set to ", capacity);
                }
            }

            // The decoder's capacity is set based on what we advertise
            if (local_settings_.qpack_max_table_capacity > 0) {
                decoder_.set_dynamic_table_capacity(local_settings_.qpack_max_table_capacity);
                echo::debug("HTTP/3 decoder dynamic table capacity set to ", local_settings_.qpack_max_table_capacity);
            }
        }

        void parse_headers(RequestStream &stream, const HeaderList &headers) {
            for (const auto &h : headers) {
                if (h.name == ":method") {
                    stream.request.method = h.value;
                } else if (h.name == ":scheme") {
                    stream.request.scheme = h.value;
                } else if (h.name == ":authority") {
                    stream.request.authority = h.value;
                } else if (h.name == ":path") {
                    stream.request.path = h.value;
                } else if (h.name == ":protocol") {
                    // Extended CONNECT (RFC 9220)
                    stream.request.protocol = h.value;
                } else if (h.name == ":status") {
                    stream.response.status = static_cast<dp::u32>(std::stoul(std::string(h.value.c_str())));
                } else {
                    // Regular header
                    if (is_client_) {
                        stream.response.headers.push_back(h);
                    } else {
                        stream.request.headers.push_back(h);
                    }
                }
            }
        }

        bool is_client_;
        ConnectionState state_ = ConnectionState::Idle;

        // Settings
        Settings local_settings_;
        Settings peer_settings_;
        bool settings_sent_ = false;
        bool settings_received_ = false;

        // GOAWAY
        bool goaway_sent_ = false;
        bool goaway_received_ = false;
        dp::u64 goaway_stream_id_ = 0;

        // Request streams
        dp::u64 next_request_stream_id_ = 0; // Client: 0, 4, 8, ...
        dp::Map<dp::u64, RequestStream> request_streams_;

        // Server Push (RFC 9114 Section 4.6)
        dp::u64 max_push_id_ = 0;                 // Client sets this via MAX_PUSH_ID
        dp::u64 next_push_id_ = 0;                // Server's next push ID to use
        bool max_push_id_sent_ = false;           // Has client sent MAX_PUSH_ID?
        dp::Map<dp::u64, Request> push_promises_; // Push ID -> promised request
        dp::Set<dp::u64> cancelled_pushes_;       // Push IDs cancelled by client

        // QPACK
        QpackEncoder encoder_;
        QpackDecoder decoder_;

        // Flow control
        dp::u64 connection_send_limit_ = 0;
        dp::u64 connection_bytes_sent_ = 0;
        dp::u64 connection_bytes_received_ = 0;

        // Timeout and draining
        dp::u64 idle_timeout_ms_ = 0;        // 0 = no timeout
        dp::u64 draining_timeout_ms_ = 3000; // Default 3 seconds
        dp::u64 last_activity_time_ms_ = 0;
        dp::u64 draining_start_time_ms_ = 0;

        // Time provider (can be overridden for testing)
        std::function<dp::u64()> current_time_ms_ = []() {
            auto now = std::chrono::steady_clock::now();
            return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        };
    };

} // namespace netpipe::http3

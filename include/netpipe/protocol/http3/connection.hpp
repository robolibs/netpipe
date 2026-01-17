#pragma once

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
        Closed
    };

    // HTTP/3 Stream state
    enum class Http3StreamState { Idle, Open, HalfClosedLocal, HalfClosedRemote, Closed };

    // Represents an HTTP/3 request stream
    struct RequestStream {
        dp::u64 stream_id = 0;
        Http3StreamState state = Http3StreamState::Idle;
        Request request;
        Response response;
        bool headers_sent = false;
        bool headers_received = false;
        bool body_complete = false;
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
        }

        // Get connection state
        ConnectionState state() const { return state_; }
        bool is_client() const { return is_client_; }
        bool is_connected() const { return state_ == ConnectionState::Connected; }

        // Settings
        const Settings &local_settings() const { return local_settings_; }
        const Settings &peer_settings() const { return peer_settings_; }

        void set_local_settings(const Settings &settings) { local_settings_ = settings; }

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
                    peer_settings_ = frame.settings;
                    settings_received_ = true;
                    offset += consumed;

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

                    // Decode headers with QPACK
                    auto decoded_result = decoder_.decode(frame.encoded_field_section);
                    if (decoded_result.is_err()) {
                        return dp::result::err(decoded_result.error());
                    }

                    auto &headers = decoded_result.value();

                    // Parse into Request or Response based on pseudo-headers
                    if (!it->second.headers_received) {
                        parse_headers(it->second, headers);
                        it->second.headers_received = true;
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

            // Create HEADERS frame
            HeadersFrame headers_frame;
            headers_frame.encoded_field_section = encoded_headers;

            it->second.headers_sent = true;

            return dp::result::ok(headers_frame.serialize());
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

      private:
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
        std::map<dp::u64, RequestStream> request_streams_;

        // QPACK
        QpackEncoder encoder_;
        QpackDecoder decoder_;
    };

} // namespace netpipe::http3

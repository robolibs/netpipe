#pragma once

#include <netpipe/protocol/http2/frame.hpp>

namespace netpipe::http2 {

    enum class ErrorCode : dp::u32 {
        NoError = 0x0,
        ProtocolError = 0x1,
        InternalError = 0x2,
        FlowControlError = 0x3,
        SettingsTimeout = 0x4,
        StreamClosed = 0x5,
        FrameSizeError = 0x6,
        RefusedStream = 0x7,
        Cancel = 0x8,
        CompressionError = 0x9,
        ConnectError = 0xA,
        EnhanceYourCalm = 0xB,
        InadequateSecurity = 0xC,
        Http11Required = 0xD,
    };

    struct GoAway {
        dp::u32 last_stream_id = 0;
        ErrorCode error_code = ErrorCode::NoError;
        dp::Vector<dp::u8> debug_data;
    };

    inline Frame make_rst_stream(dp::u32 stream_id, ErrorCode error_code) {
        Frame frame;
        frame.header.type = FrameType::RstStream;
        frame.header.stream_id = stream_id;
        frame.payload = {
            static_cast<dp::u8>((static_cast<dp::u32>(error_code) >> 24) & 0xFF),
            static_cast<dp::u8>((static_cast<dp::u32>(error_code) >> 16) & 0xFF),
            static_cast<dp::u8>((static_cast<dp::u32>(error_code) >> 8) & 0xFF),
            static_cast<dp::u8>(static_cast<dp::u32>(error_code) & 0xFF),
        };
        return frame;
    }

    inline dp::Result<ErrorCode> parse_rst_stream(const Frame &frame) {
        if (frame.header.type != FrameType::RstStream) {
            return dp::result::err(dp::Error::invalid_argument("expected RST_STREAM frame"));
        }
        if (frame.header.stream_id == 0) {
            return dp::result::err(dp::Error::invalid_argument("RST_STREAM must use non-zero stream id"));
        }
        if (frame.payload.size() != 4) {
            return dp::result::err(dp::Error::invalid_argument("RST_STREAM payload must be 4 bytes"));
        }

        dp::u32 code = (static_cast<dp::u32>(frame.payload[0]) << 24) | (static_cast<dp::u32>(frame.payload[1]) << 16) |
                       (static_cast<dp::u32>(frame.payload[2]) << 8) | static_cast<dp::u32>(frame.payload[3]);
        return dp::result::ok(static_cast<ErrorCode>(code));
    }

    inline Frame make_goaway(const GoAway &goaway) {
        Frame frame;
        frame.header.type = FrameType::GoAway;
        frame.header.stream_id = 0;
        frame.payload = {
            static_cast<dp::u8>((goaway.last_stream_id >> 24) & 0x7F),
            static_cast<dp::u8>((goaway.last_stream_id >> 16) & 0xFF),
            static_cast<dp::u8>((goaway.last_stream_id >> 8) & 0xFF),
            static_cast<dp::u8>(goaway.last_stream_id & 0xFF),
            static_cast<dp::u8>((static_cast<dp::u32>(goaway.error_code) >> 24) & 0xFF),
            static_cast<dp::u8>((static_cast<dp::u32>(goaway.error_code) >> 16) & 0xFF),
            static_cast<dp::u8>((static_cast<dp::u32>(goaway.error_code) >> 8) & 0xFF),
            static_cast<dp::u8>(static_cast<dp::u32>(goaway.error_code) & 0xFF),
        };
        frame.payload.insert(frame.payload.end(), goaway.debug_data.begin(), goaway.debug_data.end());
        return frame;
    }

    inline dp::Result<GoAway> parse_goaway(const Frame &frame) {
        if (frame.header.type != FrameType::GoAway) {
            return dp::result::err(dp::Error::invalid_argument("expected GOAWAY frame"));
        }
        if (frame.header.stream_id != 0) {
            return dp::result::err(dp::Error::invalid_argument("GOAWAY must use stream 0"));
        }
        if (frame.payload.size() < 8) {
            return dp::result::err(dp::Error::invalid_argument("GOAWAY payload too short"));
        }

        GoAway out;
        out.last_stream_id = (static_cast<dp::u32>(frame.payload[0] & 0x7F) << 24) |
                             (static_cast<dp::u32>(frame.payload[1]) << 16) |
                             (static_cast<dp::u32>(frame.payload[2]) << 8) | static_cast<dp::u32>(frame.payload[3]);
        dp::u32 code = (static_cast<dp::u32>(frame.payload[4]) << 24) | (static_cast<dp::u32>(frame.payload[5]) << 16) |
                       (static_cast<dp::u32>(frame.payload[6]) << 8) | static_cast<dp::u32>(frame.payload[7]);
        out.error_code = static_cast<ErrorCode>(code);
        out.debug_data = dp::Vector<dp::u8>(frame.payload.begin() + 8, frame.payload.end());
        return dp::result::ok(std::move(out));
    }

    class ShutdownManager {
      public:
        void on_stream_opened(dp::u32 stream_id) {
            active_streams_.insert(stream_id);
            if (stream_id > last_opened_stream_id_) {
                last_opened_stream_id_ = stream_id;
            }
        }

        void on_stream_closed(dp::u32 stream_id) { active_streams_.erase(stream_id); }

        bool is_stream_active(dp::u32 stream_id) const {
            return active_streams_.find(stream_id) != active_streams_.end();
        }

        dp::Result<Frame> start_shutdown(ErrorCode code, const dp::Vector<dp::u8> &debug_data = {}) {
            if (goaway_sent_) {
                return dp::result::err(dp::Error::invalid_argument("GOAWAY already sent"));
            }

            GoAway goaway;
            goaway.last_stream_id = last_opened_stream_id_;
            goaway.error_code = code;
            goaway.debug_data = debug_data;

            goaway_sent_ = true;
            draining_ = true;
            return dp::result::ok(make_goaway(goaway));
        }

        dp::Result<void> process_incoming(const Frame &frame) {
            if (frame.header.type == FrameType::GoAway) {
                auto parsed = parse_goaway(frame);
                if (parsed.is_err()) {
                    return dp::result::err(parsed.error());
                }
                peer_goaway_ = parsed.value();
                draining_ = true;
                return dp::result::ok();
            }

            if (frame.header.type == FrameType::RstStream) {
                auto parsed = parse_rst_stream(frame);
                if (parsed.is_err()) {
                    return dp::result::err(parsed.error());
                }
                (void)parsed;
                on_stream_closed(frame.header.stream_id);
                return dp::result::ok();
            }

            return dp::result::ok();
        }

        bool can_open_new_stream(dp::u32 stream_id) const {
            if (!draining_) {
                return true;
            }
            if (!peer_goaway_.has_value()) {
                return false;
            }
            return stream_id <= peer_goaway_.value().last_stream_id;
        }

        bool draining() const { return draining_; }
        bool goaway_sent() const { return goaway_sent_; }
        dp::usize active_stream_count() const { return active_streams_.size(); }
        const dp::Optional<GoAway> &peer_goaway() const { return peer_goaway_; }

      private:
        dp::Set<dp::u32> active_streams_;
        dp::u32 last_opened_stream_id_ = 0;
        bool goaway_sent_ = false;
        bool draining_ = false;
        dp::Optional<GoAway> peer_goaway_;
    };

} // namespace netpipe::http2

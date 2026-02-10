#pragma once

#include <netpipe/protocol/http2/types.hpp>

namespace netpipe::http2 {

    // RFC coverage notes (smoke-level, not full conformance):
    // - 9-byte frame header parse/serialize and 24-bit length bounds
    // - Stream ID 31-bit masking rules
    // - Control-frame stream-0 constraints
    // - Stream-frame non-zero stream-id constraints

    enum class FrameType : dp::u8 {
        Data = 0x0,
        Headers = 0x1,
        Priority = 0x2,
        RstStream = 0x3,
        Settings = 0x4,
        PushPromise = 0x5,
        Ping = 0x6,
        GoAway = 0x7,
        WindowUpdate = 0x8,
        Continuation = 0x9
    };

    struct FrameHeader {
        dp::u32 length = 0; // 24-bit on wire
        FrameType type = FrameType::Data;
        dp::u8 flags = 0;
        dp::u32 stream_id = 0; // 31-bit on wire
    };

    struct Frame {
        FrameHeader header;
        dp::Vector<dp::u8> payload;
    };

    inline dp::Result<void> validate_frame_header(const FrameHeader &header) {
        if (header.length > 0x00FFFFFF) {
            return dp::result::err(dp::Error::invalid_argument("HTTP/2 frame length exceeds 24-bit limit"));
        }
        if (header.stream_id > 0x7FFFFFFF) {
            return dp::result::err(dp::Error::invalid_argument("HTTP/2 stream id exceeds 31-bit limit"));
        }

        switch (header.type) {
        case FrameType::Settings:
        case FrameType::Ping:
        case FrameType::GoAway:
            if (header.stream_id != 0) {
                return dp::result::err(dp::Error::invalid_argument("HTTP/2 control frame requires stream 0"));
            }
            break;
        default:
            break;
        }

        if ((header.type == FrameType::Data || header.type == FrameType::Headers ||
             header.type == FrameType::Priority || header.type == FrameType::RstStream ||
             header.type == FrameType::PushPromise || header.type == FrameType::WindowUpdate ||
             header.type == FrameType::Continuation) &&
            header.stream_id == 0) {
            return dp::result::err(dp::Error::invalid_argument("HTTP/2 stream frame requires non-zero stream id"));
        }

        return dp::result::ok();
    }

    inline dp::Result<void> validate_frame_payload(const FrameHeader &header, const dp::Vector<dp::u8> &payload) {
        if (payload.size() != header.length) {
            return dp::result::err(dp::Error::invalid_argument("HTTP/2 frame header length mismatch payload size"));
        }

        switch (header.type) {
        case FrameType::Ping:
            if (payload.size() != 8) {
                return dp::result::err(dp::Error::invalid_argument("HTTP/2 PING payload must be 8 bytes"));
            }
            break;

        case FrameType::Priority:
            if (payload.size() != 5) {
                return dp::result::err(dp::Error::invalid_argument("HTTP/2 PRIORITY payload must be 5 bytes"));
            }
            break;

        case FrameType::RstStream:
            if (payload.size() != 4) {
                return dp::result::err(dp::Error::invalid_argument("HTTP/2 RST_STREAM payload must be 4 bytes"));
            }
            break;

        case FrameType::Settings:
            if ((header.flags & 0x1) != 0) {
                if (!payload.empty()) {
                    return dp::result::err(dp::Error::invalid_argument("HTTP/2 SETTINGS ACK payload must be empty"));
                }
            } else if ((payload.size() % 6) != 0) {
                return dp::result::err(
                    dp::Error::invalid_argument("HTTP/2 SETTINGS payload must be multiple of 6 bytes"));
            }
            break;

        case FrameType::GoAway:
            if (payload.size() < 8) {
                return dp::result::err(dp::Error::invalid_argument("HTTP/2 GOAWAY payload must be at least 8 bytes"));
            }
            break;

        case FrameType::WindowUpdate:
            if (payload.size() != 4) {
                return dp::result::err(dp::Error::invalid_argument("HTTP/2 WINDOW_UPDATE payload must be 4 bytes"));
            }
            if (payload.size() == 4) {
                dp::u32 increment = (static_cast<dp::u32>(payload[0] & 0x7F) << 24) |
                                    (static_cast<dp::u32>(payload[1]) << 16) | (static_cast<dp::u32>(payload[2]) << 8) |
                                    static_cast<dp::u32>(payload[3]);
                if (increment == 0) {
                    return dp::result::err(
                        dp::Error::invalid_argument("HTTP/2 WINDOW_UPDATE increment must be non-zero"));
                }
            }
            break;

        case FrameType::Headers:
            if ((header.flags & 0x8) != 0 && payload.empty()) {
                return dp::result::err(
                    dp::Error::invalid_argument("HTTP/2 HEADERS PADDED flag requires pad length byte"));
            }
            if ((header.flags & 0x20) != 0) {
                dp::usize min_len = ((header.flags & 0x8) != 0) ? 6 : 5;
                if (payload.size() < min_len) {
                    return dp::result::err(
                        dp::Error::invalid_argument("HTTP/2 HEADERS PRIORITY flag requires 5-byte priority section"));
                }
            }
            break;

        case FrameType::PushPromise:
            if ((header.flags & 0x8) != 0 && payload.empty()) {
                return dp::result::err(
                    dp::Error::invalid_argument("HTTP/2 PUSH_PROMISE PADDED flag requires pad length byte"));
            }
            if (payload.size() < (((header.flags & 0x8) != 0) ? 5 : 4)) {
                return dp::result::err(
                    dp::Error::invalid_argument("HTTP/2 PUSH_PROMISE payload too short for promised stream id"));
            }
            break;

        case FrameType::Continuation:
        case FrameType::Data:
            if ((header.flags & 0x8) != 0 && payload.empty()) {
                return dp::result::err(dp::Error::invalid_argument("HTTP/2 PADDED frame requires pad length byte"));
            }
            break;

        default:
            break;
        }

        return dp::result::ok();
    }

    inline dp::Result<void> validate_frame(const Frame &frame) {
        auto header_valid = validate_frame_header(frame.header);
        if (header_valid.is_err()) {
            return dp::result::err(header_valid.error());
        }
        return validate_frame_payload(frame.header, frame.payload);
    }

    inline dp::Vector<dp::u8> serialize_frame_header(const FrameHeader &header) {
        dp::Vector<dp::u8> out;
        out.reserve(9);

        out.push_back(static_cast<dp::u8>((header.length >> 16) & 0xFF));
        out.push_back(static_cast<dp::u8>((header.length >> 8) & 0xFF));
        out.push_back(static_cast<dp::u8>(header.length & 0xFF));
        out.push_back(static_cast<dp::u8>(header.type));
        out.push_back(header.flags);

        dp::u32 stream_id = header.stream_id & 0x7FFFFFFF;
        out.push_back(static_cast<dp::u8>((stream_id >> 24) & 0x7F));
        out.push_back(static_cast<dp::u8>((stream_id >> 16) & 0xFF));
        out.push_back(static_cast<dp::u8>((stream_id >> 8) & 0xFF));
        out.push_back(static_cast<dp::u8>(stream_id & 0xFF));

        return out;
    }

    inline dp::Result<std::pair<FrameHeader, dp::usize>> parse_frame_header(const dp::u8 *data, dp::usize size) {
        if (size < 9) {
            return dp::result::err(dp::Error::invalid_argument("HTTP/2 frame header too short"));
        }

        FrameHeader header;
        header.length = (static_cast<dp::u32>(data[0]) << 16) | (static_cast<dp::u32>(data[1]) << 8) |
                        static_cast<dp::u32>(data[2]);
        header.type = static_cast<FrameType>(data[3]);
        header.flags = data[4];
        header.stream_id = (static_cast<dp::u32>(data[5] & 0x7F) << 24) | (static_cast<dp::u32>(data[6]) << 16) |
                           (static_cast<dp::u32>(data[7]) << 8) | static_cast<dp::u32>(data[8]);

        auto valid = validate_frame_header(header);
        if (valid.is_err()) {
            return dp::result::err(valid.error());
        }

        return dp::result::ok(std::make_pair(header, dp::usize(9)));
    }

    inline dp::Result<dp::Vector<dp::u8>> serialize_frame(const Frame &frame) {
        FrameHeader header = frame.header;
        header.length = static_cast<dp::u32>(frame.payload.size());

        Frame normalized = frame;
        normalized.header = header;
        auto valid = validate_frame(normalized);
        if (valid.is_err()) {
            return dp::result::err(valid.error());
        }

        auto out = serialize_frame_header(header);
        out.insert(out.end(), frame.payload.begin(), frame.payload.end());
        return dp::result::ok(std::move(out));
    }

    inline dp::Result<std::pair<Frame, dp::usize>> parse_frame(const dp::u8 *data, dp::usize size) {
        auto header_result = parse_frame_header(data, size);
        if (header_result.is_err()) {
            return dp::result::err(header_result.error());
        }

        auto [header, consumed] = header_result.value();
        if (size < consumed + header.length) {
            return dp::result::err(dp::Error::invalid_argument("HTTP/2 frame payload truncated"));
        }

        Frame frame;
        frame.header = header;
        frame.payload = dp::Vector<dp::u8>(data + consumed, data + consumed + header.length);

        auto valid = validate_frame(frame);
        if (valid.is_err()) {
            return dp::result::err(valid.error());
        }

        return dp::result::ok(std::make_pair(std::move(frame), consumed + header.length));
    }

} // namespace netpipe::http2

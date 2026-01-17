#pragma once

#include <datapod/datapod.hpp>
#include <echo/echo.hpp>
#include <netpipe/http3/types.hpp>
#include <netpipe/quic/varint.hpp>

namespace netpipe::http3 {

    // Base HTTP/3 Frame
    struct Frame {
        virtual ~Frame() = default;
        virtual FrameType type() const = 0;
        virtual dp::Vector<dp::u8> serialize() const = 0;
    };

    // DATA Frame (0x00)
    // Contains request/response body data
    struct DataFrame : Frame {
        dp::Vector<dp::u8> data;

        FrameType type() const override { return FrameType::Data; }

        dp::Vector<dp::u8> serialize() const override {
            dp::Vector<dp::u8> result;

            // Frame type
            auto type_bytes = quic::varint_encode(static_cast<dp::u64>(FrameType::Data));
            result.insert(result.end(), type_bytes.begin(), type_bytes.end());

            // Length
            auto len_bytes = quic::varint_encode(data.size());
            result.insert(result.end(), len_bytes.begin(), len_bytes.end());

            // Data
            result.insert(result.end(), data.begin(), data.end());

            return result;
        }

        static dp::Res<std::pair<DataFrame, dp::usize>> parse(const dp::u8 *data, dp::usize size) {
            dp::usize offset = 0;

            // Type already consumed by caller
            // Parse length
            auto len_result = quic::varint_decode(data + offset, size - offset);
            if (len_result.is_err()) {
                return dp::result::err(len_result.error());
            }
            auto [length, len_bytes] = len_result.value();
            offset += len_bytes;

            if (offset + length > size) {
                return dp::result::err(dp::Error::invalid_argument("DATA frame truncated"));
            }

            DataFrame frame;
            frame.data = dp::Vector<dp::u8>(data + offset, data + offset + length);
            offset += length;

            return dp::result::ok(std::make_pair(std::move(frame), offset));
        }
    };

    // HEADERS Frame (0x01)
    // Contains encoded header block
    struct HeadersFrame : Frame {
        dp::Vector<dp::u8> encoded_field_section;

        FrameType type() const override { return FrameType::Headers; }

        dp::Vector<dp::u8> serialize() const override {
            dp::Vector<dp::u8> result;

            // Frame type
            auto type_bytes = quic::varint_encode(static_cast<dp::u64>(FrameType::Headers));
            result.insert(result.end(), type_bytes.begin(), type_bytes.end());

            // Length
            auto len_bytes = quic::varint_encode(encoded_field_section.size());
            result.insert(result.end(), len_bytes.begin(), len_bytes.end());

            // Encoded field section
            result.insert(result.end(), encoded_field_section.begin(), encoded_field_section.end());

            return result;
        }

        static dp::Res<std::pair<HeadersFrame, dp::usize>> parse(const dp::u8 *data, dp::usize size) {
            dp::usize offset = 0;

            // Parse length
            auto len_result = quic::varint_decode(data + offset, size - offset);
            if (len_result.is_err()) {
                return dp::result::err(len_result.error());
            }
            auto [length, len_bytes] = len_result.value();
            offset += len_bytes;

            if (offset + length > size) {
                return dp::result::err(dp::Error::invalid_argument("HEADERS frame truncated"));
            }

            HeadersFrame frame;
            frame.encoded_field_section = dp::Vector<dp::u8>(data + offset, data + offset + length);
            offset += length;

            return dp::result::ok(std::make_pair(std::move(frame), offset));
        }
    };

    // SETTINGS Frame (0x04)
    struct SettingsFrame : Frame {
        Settings settings;

        FrameType type() const override { return FrameType::Settings; }

        dp::Vector<dp::u8> serialize() const override {
            dp::Vector<dp::u8> result;

            // Frame type
            auto type_bytes = quic::varint_encode(static_cast<dp::u64>(FrameType::Settings));
            result.insert(result.end(), type_bytes.begin(), type_bytes.end());

            // Serialize settings
            auto settings_data = settings.serialize();

            // Length
            auto len_bytes = quic::varint_encode(settings_data.size());
            result.insert(result.end(), len_bytes.begin(), len_bytes.end());

            // Settings data
            result.insert(result.end(), settings_data.begin(), settings_data.end());

            return result;
        }

        static dp::Res<std::pair<SettingsFrame, dp::usize>> parse(const dp::u8 *data, dp::usize size) {
            dp::usize offset = 0;

            // Parse length
            auto len_result = quic::varint_decode(data + offset, size - offset);
            if (len_result.is_err()) {
                return dp::result::err(len_result.error());
            }
            auto [length, len_bytes] = len_result.value();
            offset += len_bytes;

            if (offset + length > size) {
                return dp::result::err(dp::Error::invalid_argument("SETTINGS frame truncated"));
            }

            SettingsFrame frame;
            auto settings_result = Settings::parse(data + offset, length);
            if (settings_result.is_err()) {
                return dp::result::err(settings_result.error());
            }
            frame.settings = settings_result.value();
            offset += length;

            return dp::result::ok(std::make_pair(std::move(frame), offset));
        }
    };

    // GOAWAY Frame (0x07)
    struct GoAwayFrame : Frame {
        dp::u64 stream_id = 0;

        FrameType type() const override { return FrameType::GoAway; }

        dp::Vector<dp::u8> serialize() const override {
            dp::Vector<dp::u8> result;

            // Frame type
            auto type_bytes = quic::varint_encode(static_cast<dp::u64>(FrameType::GoAway));
            result.insert(result.end(), type_bytes.begin(), type_bytes.end());

            // Stream ID
            auto id_bytes = quic::varint_encode(stream_id);

            // Length
            auto len_bytes = quic::varint_encode(id_bytes.size());
            result.insert(result.end(), len_bytes.begin(), len_bytes.end());

            // Stream ID
            result.insert(result.end(), id_bytes.begin(), id_bytes.end());

            return result;
        }

        static dp::Res<std::pair<GoAwayFrame, dp::usize>> parse(const dp::u8 *data, dp::usize size) {
            dp::usize offset = 0;

            // Parse length
            auto len_result = quic::varint_decode(data + offset, size - offset);
            if (len_result.is_err()) {
                return dp::result::err(len_result.error());
            }
            auto [length, len_bytes] = len_result.value();
            offset += len_bytes;

            if (offset + length > size) {
                return dp::result::err(dp::Error::invalid_argument("GOAWAY frame truncated"));
            }

            // Parse stream ID
            auto id_result = quic::varint_decode(data + offset, length);
            if (id_result.is_err()) {
                return dp::result::err(id_result.error());
            }

            GoAwayFrame frame;
            frame.stream_id = id_result.value().first;
            offset += length;

            return dp::result::ok(std::make_pair(std::move(frame), offset));
        }
    };

    // CANCEL_PUSH Frame (0x03)
    struct CancelPushFrame : Frame {
        dp::u64 push_id = 0;

        FrameType type() const override { return FrameType::CancelPush; }

        dp::Vector<dp::u8> serialize() const override {
            dp::Vector<dp::u8> result;

            auto type_bytes = quic::varint_encode(static_cast<dp::u64>(FrameType::CancelPush));
            result.insert(result.end(), type_bytes.begin(), type_bytes.end());

            auto id_bytes = quic::varint_encode(push_id);
            auto len_bytes = quic::varint_encode(id_bytes.size());
            result.insert(result.end(), len_bytes.begin(), len_bytes.end());
            result.insert(result.end(), id_bytes.begin(), id_bytes.end());

            return result;
        }
    };

    // MAX_PUSH_ID Frame (0x0D)
    struct MaxPushIdFrame : Frame {
        dp::u64 push_id = 0;

        FrameType type() const override { return FrameType::MaxPushId; }

        dp::Vector<dp::u8> serialize() const override {
            dp::Vector<dp::u8> result;

            auto type_bytes = quic::varint_encode(static_cast<dp::u64>(FrameType::MaxPushId));
            result.insert(result.end(), type_bytes.begin(), type_bytes.end());

            auto id_bytes = quic::varint_encode(push_id);
            auto len_bytes = quic::varint_encode(id_bytes.size());
            result.insert(result.end(), len_bytes.begin(), len_bytes.end());
            result.insert(result.end(), id_bytes.begin(), id_bytes.end());

            return result;
        }
    };

    // Parse any HTTP/3 frame
    inline dp::Res<std::pair<FrameType, dp::usize>> parse_frame_header(const dp::u8 *data, dp::usize size) {
        if (size < 1) {
            return dp::result::err(dp::Error::invalid_argument("empty frame"));
        }

        auto type_result = quic::varint_decode(data, size);
        if (type_result.is_err()) {
            return dp::result::err(type_result.error());
        }

        auto [type_val, type_len] = type_result.value();
        return dp::result::ok(std::make_pair(static_cast<FrameType>(type_val), type_len));
    }

} // namespace netpipe::http3

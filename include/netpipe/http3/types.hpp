#pragma once

#include <datapod/datapod.hpp>
#include <echo/echo.hpp>
#include <netpipe/quic/varint.hpp>

namespace netpipe::http3 {

    // HTTP/3 Frame Types (RFC 9114)
    enum class FrameType : dp::u64 {
        Data = 0x00,
        Headers = 0x01,
        CancelPush = 0x03,
        Settings = 0x04,
        PushPromise = 0x05,
        GoAway = 0x07,
        MaxPushId = 0x0D,
        // Reserved frame types for grease
    };

    // HTTP/3 Error Codes (RFC 9114)
    enum class ErrorCode : dp::u64 {
        NoError = 0x100,
        GeneralProtocolError = 0x101,
        InternalError = 0x102,
        StreamCreationError = 0x103,
        ClosedCriticalStream = 0x104,
        FrameUnexpected = 0x105,
        FrameError = 0x106,
        ExcessiveLoad = 0x107,
        IdError = 0x108,
        SettingsError = 0x109,
        MissingSettings = 0x10A,
        RequestRejected = 0x10B,
        RequestCancelled = 0x10C,
        RequestIncomplete = 0x10D,
        MessageError = 0x10E,
        ConnectError = 0x10F,
        VersionFallback = 0x110,
    };

    // HTTP/3 Stream Types
    enum class StreamType : dp::u64 {
        Control = 0x00,
        Push = 0x01,
        QpackEncoder = 0x02,
        QpackDecoder = 0x03,
    };

    // HTTP/3 Settings IDs
    enum class SettingsId : dp::u64 {
        QpackMaxTableCapacity = 0x01,
        MaxFieldSectionSize = 0x06,
        QpackBlockedStreams = 0x07,
    };

    // HTTP/3 Settings
    struct Settings {
        dp::u64 qpack_max_table_capacity = 0;
        dp::u64 max_field_section_size = 0;
        dp::u64 qpack_blocked_streams = 0;

        dp::Vector<dp::u8> serialize() const {
            dp::Vector<dp::u8> data;

            // QPACK_MAX_TABLE_CAPACITY
            if (qpack_max_table_capacity > 0) {
                auto id = quic::varint_encode(static_cast<dp::u64>(SettingsId::QpackMaxTableCapacity));
                auto val = quic::varint_encode(qpack_max_table_capacity);
                data.insert(data.end(), id.begin(), id.end());
                data.insert(data.end(), val.begin(), val.end());
            }

            // MAX_FIELD_SECTION_SIZE
            if (max_field_section_size > 0) {
                auto id = quic::varint_encode(static_cast<dp::u64>(SettingsId::MaxFieldSectionSize));
                auto val = quic::varint_encode(max_field_section_size);
                data.insert(data.end(), id.begin(), id.end());
                data.insert(data.end(), val.begin(), val.end());
            }

            // QPACK_BLOCKED_STREAMS
            if (qpack_blocked_streams > 0) {
                auto id = quic::varint_encode(static_cast<dp::u64>(SettingsId::QpackBlockedStreams));
                auto val = quic::varint_encode(qpack_blocked_streams);
                data.insert(data.end(), id.begin(), id.end());
                data.insert(data.end(), val.begin(), val.end());
            }

            return data;
        }

        static dp::Res<Settings> parse(const dp::u8 *data, dp::usize size) {
            Settings settings;
            dp::usize offset = 0;

            while (offset < size) {
                auto id_result = quic::varint_decode(data + offset, size - offset);
                if (id_result.is_err()) {
                    return dp::result::err(id_result.error());
                }
                auto [id, id_len] = id_result.value();
                offset += id_len;

                auto val_result = quic::varint_decode(data + offset, size - offset);
                if (val_result.is_err()) {
                    return dp::result::err(val_result.error());
                }
                auto [val, val_len] = val_result.value();
                offset += val_len;

                switch (static_cast<SettingsId>(id)) {
                case SettingsId::QpackMaxTableCapacity:
                    settings.qpack_max_table_capacity = val;
                    break;
                case SettingsId::MaxFieldSectionSize:
                    settings.max_field_section_size = val;
                    break;
                case SettingsId::QpackBlockedStreams:
                    settings.qpack_blocked_streams = val;
                    break;
                default:
                    // Unknown setting, skip
                    break;
                }
            }

            return dp::result::ok(std::move(settings));
        }
    };

    // HTTP Header Field
    struct HeaderField {
        dp::String name;
        dp::String value;

        HeaderField() = default;
        HeaderField(const dp::String &n, const dp::String &v) : name(n), value(v) {}
    };

    // HTTP Request/Response headers
    using HeaderList = dp::Vector<HeaderField>;

    // HTTP/3 Request
    struct Request {
        dp::String method;
        dp::String scheme;
        dp::String authority;
        dp::String path;
        HeaderList headers;
        dp::Vector<dp::u8> body;

        // Get pseudo-headers as HeaderList for encoding
        HeaderList get_pseudo_headers() const {
            HeaderList pseudo;
            pseudo.push_back(HeaderField(":method", method));
            pseudo.push_back(HeaderField(":scheme", scheme));
            pseudo.push_back(HeaderField(":authority", authority));
            pseudo.push_back(HeaderField(":path", path));
            return pseudo;
        }

        // Get all headers (pseudo + regular)
        HeaderList get_all_headers() const {
            auto all = get_pseudo_headers();
            all.insert(all.end(), headers.begin(), headers.end());
            return all;
        }
    };

    // HTTP/3 Response
    struct Response {
        dp::u32 status = 0;
        HeaderList headers;
        dp::Vector<dp::u8> body;

        // Get pseudo-headers
        HeaderList get_pseudo_headers() const {
            HeaderList pseudo;
            auto status_str = std::to_string(status);
            pseudo.push_back(HeaderField(":status", dp::String(status_str.c_str())));
            return pseudo;
        }

        // Get all headers
        HeaderList get_all_headers() const {
            auto all = get_pseudo_headers();
            all.insert(all.end(), headers.begin(), headers.end());
            return all;
        }
    };

} // namespace netpipe::http3

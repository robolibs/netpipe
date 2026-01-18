#pragma once

#include <datapod/datapod.hpp>
#include <echo/echo.hpp>
#include <netpipe/transport/stream/quic/varint.hpp>

namespace netpipe::http3 {

    // HTTP/3 Frame Types (RFC 9114)
    enum class FrameType : dp::u64 {
        Data = 0x00,
        Headers = 0x01,
        CancelPush = 0x03,
        Settings = 0x04,
        PushPromise = 0x05,
        PriorityUpdate = 0x0F, // RFC 9218 Extensible Priorities
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
        EnableConnectProtocol = 0x08, // RFC 9220 Extended CONNECT
    };

    // HTTP/3 Settings
    struct Settings {
        dp::u64 qpack_max_table_capacity = 0;
        dp::u64 max_field_section_size = 0;
        dp::u64 qpack_blocked_streams = 0;
        bool enable_connect_protocol = false; // RFC 9220 Extended CONNECT

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

            // ENABLE_CONNECT_PROTOCOL (RFC 9220)
            if (enable_connect_protocol) {
                auto id = quic::varint_encode(static_cast<dp::u64>(SettingsId::EnableConnectProtocol));
                auto val = quic::varint_encode(1); // Value is 1 when enabled
                data.insert(data.end(), id.begin(), id.end());
                data.insert(data.end(), val.begin(), val.end());
            }

            return data;
        }

        // Check if a setting ID is a GREASE value (must be ignored)
        static bool is_grease_setting(dp::u64 id) {
            // GREASE settings: values of the form 0x1f * N + 0x21 for non-negative N
            // This produces: 0x21, 0x40, 0x5f, 0x7e, 0x9d, ...
            if (id < 0x21)
                return false;
            return ((id - 0x21) % 0x1f) == 0;
        }

        static dp::Res<Settings> parse(const dp::u8 *data, dp::usize size) {
            Settings settings;
            dp::usize offset = 0;

            // Track which settings we've seen to detect duplicates
            bool seen_qpack_max_table_capacity = false;
            bool seen_max_field_section_size = false;
            bool seen_qpack_blocked_streams = false;
            bool seen_enable_connect_protocol = false;

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

                // GREASE settings must be ignored
                if (is_grease_setting(id)) {
                    continue;
                }

                switch (static_cast<SettingsId>(id)) {
                case SettingsId::QpackMaxTableCapacity:
                    if (seen_qpack_max_table_capacity) {
                        return dp::result::err(
                            dp::Error::invalid_argument("duplicate QPACK_MAX_TABLE_CAPACITY setting"));
                    }
                    settings.qpack_max_table_capacity = val;
                    seen_qpack_max_table_capacity = true;
                    break;
                case SettingsId::MaxFieldSectionSize:
                    if (seen_max_field_section_size) {
                        return dp::result::err(dp::Error::invalid_argument("duplicate MAX_FIELD_SECTION_SIZE setting"));
                    }
                    settings.max_field_section_size = val;
                    seen_max_field_section_size = true;
                    break;
                case SettingsId::QpackBlockedStreams:
                    if (seen_qpack_blocked_streams) {
                        return dp::result::err(dp::Error::invalid_argument("duplicate QPACK_BLOCKED_STREAMS setting"));
                    }
                    settings.qpack_blocked_streams = val;
                    seen_qpack_blocked_streams = true;
                    break;
                case SettingsId::EnableConnectProtocol:
                    if (seen_enable_connect_protocol) {
                        return dp::result::err(
                            dp::Error::invalid_argument("duplicate ENABLE_CONNECT_PROTOCOL setting"));
                    }
                    settings.enable_connect_protocol = (val != 0);
                    seen_enable_connect_protocol = true;
                    break;
                default:
                    // Unknown setting, skip (RFC 9114 requires ignoring unknown settings)
                    break;
                }
            }

            return dp::result::ok(std::move(settings));
        }

        // Validate settings against RFC constraints
        static dp::Res<void> validate(const Settings &settings) {
            // QPACK_MAX_TABLE_CAPACITY: no specific constraint other than varint range
            // MAX_FIELD_SECTION_SIZE: no specific constraint other than varint range
            // QPACK_BLOCKED_STREAMS: no specific constraint other than varint range
            // All values are valid as long as they fit in a varint (already guaranteed by parsing)
            (void)settings;
            return dp::result::ok();
        }
    };

    // Extensible Priority (RFC 9218)
    // Used for stream prioritization in HTTP/3
    struct Priority {
        dp::u8 urgency = 3;       // 0 (highest) to 7 (lowest), default 3
        bool incremental = false; // Whether resource can be processed incrementally

        // Default constructor
        Priority() = default;
        Priority(dp::u8 u, bool i) : urgency(std::min(u, dp::u8(7))), incremental(i) {}

        // Serialize to Structured Field format: "u=3, i"
        dp::String serialize() const {
            dp::String result = "u=";
            result += std::to_string(urgency);
            if (incremental) {
                result += ", i";
            }
            return result;
        }

        // Parse from Structured Field format: "u=3, i" or "u=3"
        static dp::Res<Priority> parse(const dp::String &value) {
            Priority prio;
            dp::usize pos = 0;

            // Skip whitespace
            while (pos < value.size() && (value[pos] == ' ' || value[pos] == '\t')) {
                pos++;
            }

            // Parse parameters
            while (pos < value.size()) {
                // Skip whitespace and commas
                while (pos < value.size() && (value[pos] == ' ' || value[pos] == '\t' || value[pos] == ',')) {
                    pos++;
                }
                if (pos >= value.size())
                    break;

                // Check for "u=" (urgency)
                if (pos + 2 <= value.size() && value[pos] == 'u' && value[pos + 1] == '=') {
                    pos += 2;
                    if (pos < value.size() && value[pos] >= '0' && value[pos] <= '7') {
                        prio.urgency = value[pos] - '0';
                        pos++;
                    }
                }
                // Check for "i" (incremental)
                else if (value[pos] == 'i') {
                    prio.incremental = true;
                    pos++;
                    // Check for "=?1" format
                    if (pos + 2 <= value.size() && value[pos] == '=' && value[pos + 1] == '?') {
                        pos += 2;
                        if (pos < value.size()) {
                            prio.incremental = (value[pos] == '1');
                            pos++;
                        }
                    }
                } else {
                    // Skip unknown parameter
                    while (pos < value.size() && value[pos] != ',' && value[pos] != ' ') {
                        pos++;
                    }
                }
            }

            return dp::result::ok(prio);
        }

        // Comparison for scheduling
        bool operator<(const Priority &other) const {
            // Lower urgency value = higher priority
            if (urgency != other.urgency) {
                return urgency < other.urgency;
            }
            // Non-incremental has priority over incremental for same urgency
            return !incremental && other.incremental;
        }

        bool operator==(const Priority &other) const {
            return urgency == other.urgency && incremental == other.incremental;
        }
    };

    // HTTP Header Field
    struct HeaderField {
        dp::String name;
        dp::String value;

        HeaderField() = default;
        HeaderField(const dp::String &n, const dp::String &v) : name(n), value(v) {}
        HeaderField(dp::String &&n, dp::String &&v) : name(std::move(n)), value(std::move(v)) {}
        HeaderField(const dp::String &n, dp::String &&v) : name(n), value(std::move(v)) {}
        HeaderField(dp::String &&n, const dp::String &v) : name(std::move(n)), value(v) {}

        // Explicitly default the move/copy operations
        HeaderField(const HeaderField &) = default;
        HeaderField(HeaderField &&) = default;
        HeaderField &operator=(const HeaderField &) = default;
        HeaderField &operator=(HeaderField &&) = default;
    };

    // HTTP Request/Response headers
    using HeaderList = dp::Vector<HeaderField>;

    // HTTP/3 Request
    struct Request {
        dp::String method;
        dp::String scheme;
        dp::String authority;
        dp::String path;
        dp::String protocol; // For Extended CONNECT (RFC 9220), e.g., "websocket"
        HeaderList headers;
        dp::Vector<dp::u8> body;
        HeaderList trailers; // Trailer headers sent after body

        // Check if this is an Extended CONNECT request
        bool is_extended_connect() const { return method == "CONNECT" && !protocol.empty(); }

        // Get pseudo-headers as HeaderList for encoding
        HeaderList get_pseudo_headers() const {
            HeaderList pseudo;
            pseudo.push_back(HeaderField(":method", method));

            if (is_extended_connect()) {
                // Extended CONNECT uses :protocol, :scheme, :path, :authority
                pseudo.push_back(HeaderField(":protocol", protocol));
                pseudo.push_back(HeaderField(":scheme", scheme));
                pseudo.push_back(HeaderField(":authority", authority));
                pseudo.push_back(HeaderField(":path", path));
            } else if (method == "CONNECT") {
                // Regular CONNECT only uses :authority
                pseudo.push_back(HeaderField(":authority", authority));
            } else {
                // Normal request
                pseudo.push_back(HeaderField(":scheme", scheme));
                pseudo.push_back(HeaderField(":authority", authority));
                pseudo.push_back(HeaderField(":path", path));
            }
            return pseudo;
        }

        // Get all headers (pseudo + regular)
        HeaderList get_all_headers() const {
            auto all = get_pseudo_headers();
            all.insert(all.end(), headers.begin(), headers.end());
            return all;
        }

        // Check if request has trailers
        bool has_trailers() const { return !trailers.empty(); }
    };

    // HTTP/3 Response
    struct Response {
        dp::u32 status = 0;
        HeaderList headers;
        dp::Vector<dp::u8> body;
        HeaderList trailers; // Trailer headers sent after body

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

        // Check if response has trailers
        bool has_trailers() const { return !trailers.empty(); }
    };

} // namespace netpipe::http3

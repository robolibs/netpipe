#pragma once

#include <datapod/datapod.hpp>
#include <echo/echo.hpp>
#include <netpipe/protocol/http3/types.hpp>

namespace netpipe::http3 {

    // QPACK Static Table (RFC 9204 Appendix A)
    // Subset of commonly used header fields
    inline const HeaderField QPACK_STATIC_TABLE[] = {
        {":authority", ""},
        {":path", "/"},
        {"age", "0"},
        {"content-disposition", ""},
        {"content-length", "0"},
        {"cookie", ""},
        {"date", ""},
        {"etag", ""},
        {"if-modified-since", ""},
        {"if-none-match", ""},
        {"last-modified", ""},
        {"link", ""},
        {"location", ""},
        {"referer", ""},
        {"set-cookie", ""},
        {":method", "CONNECT"},
        {":method", "DELETE"},
        {":method", "GET"},
        {":method", "HEAD"},
        {":method", "OPTIONS"},
        {":method", "POST"},
        {":method", "PUT"},
        {":scheme", "http"},
        {":scheme", "https"},
        {":status", "103"},
        {":status", "200"},
        {":status", "304"},
        {":status", "404"},
        {":status", "503"},
        {"accept", "*/*"},
        {"accept", "application/dns-message"},
        {"accept-encoding", "gzip, deflate, br"},
        {"accept-ranges", "bytes"},
        {"access-control-allow-headers", "cache-control"},
        {"access-control-allow-headers", "content-type"},
        {"access-control-allow-origin", "*"},
        {"cache-control", "max-age=0"},
        {"cache-control", "max-age=2592000"},
        {"cache-control", "max-age=604800"},
        {"cache-control", "no-cache"},
        {"cache-control", "no-store"},
        {"cache-control", "public, max-age=31536000"},
        {"content-encoding", "br"},
        {"content-encoding", "gzip"},
        {"content-type", "application/dns-message"},
        {"content-type", "application/javascript"},
        {"content-type", "application/json"},
        {"content-type", "application/x-www-form-urlencoded"},
        {"content-type", "image/gif"},
        {"content-type", "image/jpeg"},
        {"content-type", "image/png"},
        {"content-type", "text/css"},
        {"content-type", "text/html; charset=utf-8"},
        {"content-type", "text/plain"},
        {"content-type", "text/plain;charset=utf-8"},
        {"range", "bytes=0-"},
        {"strict-transport-security", "max-age=31536000"},
        {"strict-transport-security", "max-age=31536000; includesubdomains"},
        {"strict-transport-security", "max-age=31536000; includesubdomains; preload"},
        {"vary", "accept-encoding"},
        {"vary", "origin"},
        {"x-content-type-options", "nosniff"},
        {"x-xss-protection", "1; mode=block"},
        {":status", "100"},
        {":status", "204"},
        {":status", "206"},
        {":status", "302"},
        {":status", "400"},
        {":status", "403"},
        {":status", "421"},
        {":status", "425"},
        {":status", "500"},
        {"accept-language", ""},
        {"access-control-allow-credentials", "FALSE"},
        {"access-control-allow-credentials", "TRUE"},
        {"access-control-allow-methods", "get"},
        {"access-control-allow-methods", "get, post, options"},
        {"access-control-allow-methods", "options"},
        {"access-control-expose-headers", "content-length"},
        {"access-control-request-headers", "content-type"},
        {"access-control-request-method", "get"},
        {"access-control-request-method", "post"},
        {"alt-svc", "clear"},
        {"authorization", ""},
        {"content-security-policy", "script-src 'none'; object-src 'none'; base-uri 'none'"},
        {"early-data", "1"},
        {"expect-ct", ""},
        {"forwarded", ""},
        {"if-range", ""},
        {"origin", ""},
        {"purpose", "prefetch"},
        {"server", ""},
        {"timing-allow-origin", "*"},
        {"upgrade-insecure-requests", "1"},
        {"user-agent", ""},
        {"x-forwarded-for", ""},
        {"x-frame-options", "deny"},
        {"x-frame-options", "sameorigin"},
    };

    constexpr dp::usize QPACK_STATIC_TABLE_SIZE = sizeof(QPACK_STATIC_TABLE) / sizeof(QPACK_STATIC_TABLE[0]);

    // QPACK Encoder (simplified - no dynamic table)
    class QpackEncoder {
      public:
        QpackEncoder() = default;

        // Encode header list to field section
        dp::Vector<dp::u8> encode(const HeaderList &headers) {
            dp::Vector<dp::u8> result;

            // Required Insert Count = 0 (no dynamic table)
            result.push_back(0x00);

            // Delta Base = 0
            result.push_back(0x00);

            for (const auto &header : headers) {
                encode_header(result, header);
            }

            return result;
        }

      private:
        void encode_header(dp::Vector<dp::u8> &result, const HeaderField &header) {
            // Try to find in static table
            int name_match = -1;
            int full_match = -1;

            for (dp::usize i = 0; i < QPACK_STATIC_TABLE_SIZE; i++) {
                if (QPACK_STATIC_TABLE[i].name == header.name) {
                    if (name_match < 0) {
                        name_match = static_cast<int>(i);
                    }
                    if (QPACK_STATIC_TABLE[i].value == header.value) {
                        full_match = static_cast<int>(i);
                        break;
                    }
                }
            }

            if (full_match >= 0) {
                // Indexed Header Field (static table)
                // Format: 1 T=1 index (6-bit prefix)
                encode_indexed_static(result, static_cast<dp::usize>(full_match));
            } else if (name_match >= 0) {
                // Literal Header Field With Name Reference (static)
                encode_literal_with_name_ref(result, static_cast<dp::usize>(name_match), header.value);
            } else {
                // Literal Header Field Without Name Reference
                encode_literal(result, header.name, header.value);
            }
        }

        void encode_indexed_static(dp::Vector<dp::u8> &result, dp::usize index) {
            // Format: 11xxxxxx (6-bit index with 0b11 prefix)
            if (index < 64) {
                result.push_back(static_cast<dp::u8>(0xC0 | index));
            } else {
                // Need multi-byte encoding
                result.push_back(0xFF); // 0b11111111
                encode_integer(result, index - 63, 0);
            }
        }

        void encode_literal_with_name_ref(dp::Vector<dp::u8> &result, dp::usize index, const dp::String &value) {
            // Format: 01 N T=1 index (4-bit prefix)
            // N = never indexed (0), T = static table (1)
            if (index < 16) {
                result.push_back(static_cast<dp::u8>(0x50 | index)); // 0b0101xxxx
            } else {
                result.push_back(0x5F); // 0b01011111
                encode_integer(result, index - 15, 0);
            }

            // Encode value
            encode_string(result, value);
        }

        void encode_literal(dp::Vector<dp::u8> &result, const dp::String &name, const dp::String &value) {
            // Format: 001 N (4-bit prefix for name length)
            // N = never indexed (0)
            result.push_back(0x20); // 0b00100000

            // Encode name
            encode_string(result, name);

            // Encode value
            encode_string(result, value);
        }

        void encode_string(dp::Vector<dp::u8> &result, const dp::String &str) {
            // Without Huffman encoding (H=0)
            dp::usize len = str.size();
            if (len < 128) {
                result.push_back(static_cast<dp::u8>(len));
            } else {
                result.push_back(0x7F);
                encode_integer(result, len - 127, 0);
            }
            result.insert(result.end(), str.begin(), str.end());
        }

        void encode_integer(dp::Vector<dp::u8> &result, dp::usize value, dp::u8 prefix_bits) {
            // QPACK integer encoding
            (void)prefix_bits;
            while (value >= 128) {
                result.push_back(static_cast<dp::u8>((value & 0x7F) | 0x80));
                value >>= 7;
            }
            result.push_back(static_cast<dp::u8>(value));
        }
    };

    // QPACK Decoder (simplified - no dynamic table)
    class QpackDecoder {
      public:
        QpackDecoder() = default;

        // Decode field section to header list
        dp::Res<HeaderList> decode(const dp::Vector<dp::u8> &data) {
            if (data.size() < 2) {
                return dp::result::err(dp::Error::invalid_argument("field section too short"));
            }

            dp::usize offset = 0;

            // Required Insert Count
            auto ric_result = decode_integer(data.data() + offset, data.size() - offset, 8);
            if (ric_result.is_err()) {
                return dp::result::err(ric_result.error());
            }
            offset += ric_result.value().second;

            // Delta Base (sign bit + integer)
            if (offset >= data.size()) {
                return dp::result::err(dp::Error::invalid_argument("missing delta base"));
            }
            auto db_result = decode_integer(data.data() + offset, data.size() - offset, 7);
            if (db_result.is_err()) {
                return dp::result::err(db_result.error());
            }
            offset += db_result.value().second;

            HeaderList headers;

            while (offset < data.size()) {
                dp::u8 first = data[offset];

                if ((first & 0x80) != 0) {
                    // Indexed Header Field
                    auto result = decode_indexed(data.data() + offset, data.size() - offset);
                    if (result.is_err()) {
                        return dp::result::err(result.error());
                    }
                    auto [header, consumed] = result.value();
                    headers.push_back(header);
                    offset += consumed;
                } else if ((first & 0x40) != 0) {
                    // Literal Header Field With Name Reference
                    auto result = decode_literal_with_ref(data.data() + offset, data.size() - offset);
                    if (result.is_err()) {
                        return dp::result::err(result.error());
                    }
                    auto [header, consumed] = result.value();
                    headers.push_back(header);
                    offset += consumed;
                } else if ((first & 0x20) != 0) {
                    // Literal Header Field Without Name Reference
                    auto result = decode_literal(data.data() + offset, data.size() - offset);
                    if (result.is_err()) {
                        return dp::result::err(result.error());
                    }
                    auto [header, consumed] = result.value();
                    headers.push_back(header);
                    offset += consumed;
                } else {
                    return dp::result::err(dp::Error::invalid_argument("unknown field line type"));
                }
            }

            return dp::result::ok(std::move(headers));
        }

      private:
        dp::Res<std::pair<dp::usize, dp::usize>> decode_integer(const dp::u8 *data, dp::usize size,
                                                                dp::u8 prefix_bits) {
            if (size < 1) {
                return dp::result::err(dp::Error::invalid_argument("empty integer"));
            }

            dp::u8 mask = (1 << prefix_bits) - 1;
            dp::usize value = data[0] & mask;
            dp::usize offset = 1;

            if (value == mask) {
                dp::usize m = 0;
                while (offset < size) {
                    dp::u8 b = data[offset++];
                    value += static_cast<dp::usize>(b & 0x7F) << m;
                    m += 7;
                    if ((b & 0x80) == 0) {
                        break;
                    }
                }
            }

            return dp::result::ok(std::make_pair(value, offset));
        }

        dp::Res<std::pair<dp::String, dp::usize>> decode_string(const dp::u8 *data, dp::usize size) {
            if (size < 1) {
                return dp::result::err(dp::Error::invalid_argument("empty string"));
            }

            bool huffman = (data[0] & 0x80) != 0;
            (void)huffman; // We don't support Huffman decoding yet

            auto len_result = decode_integer(data, size, 7);
            if (len_result.is_err()) {
                return dp::result::err(len_result.error());
            }
            auto [length, len_bytes] = len_result.value();

            if (len_bytes + length > size) {
                return dp::result::err(dp::Error::invalid_argument("string truncated"));
            }

            dp::String str(reinterpret_cast<const char *>(data + len_bytes), length);
            return dp::result::ok(std::make_pair(std::move(str), len_bytes + length));
        }

        dp::Res<std::pair<HeaderField, dp::usize>> decode_indexed(const dp::u8 *data, dp::usize size) {
            // Format: 1 T index
            bool is_static = (data[0] & 0x40) != 0;
            auto idx_result = decode_integer(data, size, 6);
            if (idx_result.is_err()) {
                return dp::result::err(idx_result.error());
            }
            auto [index, consumed] = idx_result.value();

            if (is_static && index < QPACK_STATIC_TABLE_SIZE) {
                return dp::result::ok(std::make_pair(QPACK_STATIC_TABLE[index], consumed));
            }

            return dp::result::err(dp::Error::invalid_argument("invalid indexed reference"));
        }

        dp::Res<std::pair<HeaderField, dp::usize>> decode_literal_with_ref(const dp::u8 *data, dp::usize size) {
            // Format: 01 N T index
            bool is_static = (data[0] & 0x10) != 0;
            auto idx_result = decode_integer(data, size, 4);
            if (idx_result.is_err()) {
                return dp::result::err(idx_result.error());
            }
            auto [index, idx_consumed] = idx_result.value();

            dp::String name;
            if (is_static && index < QPACK_STATIC_TABLE_SIZE) {
                name = QPACK_STATIC_TABLE[index].name;
            } else {
                return dp::result::err(dp::Error::invalid_argument("invalid name reference"));
            }

            auto val_result = decode_string(data + idx_consumed, size - idx_consumed);
            if (val_result.is_err()) {
                return dp::result::err(val_result.error());
            }
            auto [value, val_consumed] = val_result.value();

            return dp::result::ok(std::make_pair(HeaderField{name, value}, idx_consumed + val_consumed));
        }

        dp::Res<std::pair<HeaderField, dp::usize>> decode_literal(const dp::u8 *data, dp::usize size) {
            // Format: 001 N
            dp::usize offset = 0;

            // Skip first byte (we know it's 0x2X)
            offset++;

            // Decode name
            auto name_result = decode_string(data + offset, size - offset);
            if (name_result.is_err()) {
                return dp::result::err(name_result.error());
            }
            auto [name, name_consumed] = name_result.value();
            offset += name_consumed;

            // Decode value
            auto val_result = decode_string(data + offset, size - offset);
            if (val_result.is_err()) {
                return dp::result::err(val_result.error());
            }
            auto [value, val_consumed] = val_result.value();
            offset += val_consumed;

            return dp::result::ok(std::make_pair(HeaderField{name, value}, offset));
        }
    };

} // namespace netpipe::http3

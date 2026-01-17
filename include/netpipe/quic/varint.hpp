#pragma once

#include <datapod/datapod.hpp>

namespace netpipe::quic {

    // QUIC Variable-Length Integer Encoding (RFC 9000 Section 16)
    //
    // The QUIC variable-length integer encoding uses the two most significant
    // bits of the first byte to encode the length of the integer:
    //
    // 2MSB | Length | Usable Bits | Range
    // -----|--------|-------------|-------------------------
    //  00  | 1      | 6           | 0-63
    //  01  | 2      | 14          | 0-16383
    //  10  | 4      | 30          | 0-1073741823
    //  11  | 8      | 62          | 0-4611686018427387903

    // Maximum value encodable in a QUIC varint (2^62 - 1)
    constexpr dp::u64 VARINT_MAX = (1ULL << 62) - 1;

    // Length prefixes
    constexpr dp::u8 VARINT_1BYTE_PREFIX = 0x00;
    constexpr dp::u8 VARINT_2BYTE_PREFIX = 0x40;
    constexpr dp::u8 VARINT_4BYTE_PREFIX = 0x80;
    constexpr dp::u8 VARINT_8BYTE_PREFIX = 0xC0;

    // Mask for the prefix bits
    constexpr dp::u8 VARINT_PREFIX_MASK = 0xC0;

    // Maximum values for each encoding length
    constexpr dp::u64 VARINT_1BYTE_MAX = 63;
    constexpr dp::u64 VARINT_2BYTE_MAX = 16383;
    constexpr dp::u64 VARINT_4BYTE_MAX = 1073741823;
    constexpr dp::u64 VARINT_8BYTE_MAX = VARINT_MAX;

    // Get the number of bytes needed to encode a value
    inline dp::usize varint_length(dp::u64 value) {
        if (value <= VARINT_1BYTE_MAX) {
            return 1;
        } else if (value <= VARINT_2BYTE_MAX) {
            return 2;
        } else if (value <= VARINT_4BYTE_MAX) {
            return 4;
        } else {
            return 8;
        }
    }

    // Get the length of a varint from its first byte
    inline dp::usize varint_length_from_first_byte(dp::u8 first_byte) {
        switch (first_byte & VARINT_PREFIX_MASK) {
        case VARINT_1BYTE_PREFIX:
            return 1;
        case VARINT_2BYTE_PREFIX:
            return 2;
        case VARINT_4BYTE_PREFIX:
            return 4;
        case VARINT_8BYTE_PREFIX:
            return 8;
        default:
            return 0; // Should never happen
        }
    }

    // Encode a value as a QUIC variable-length integer
    // Returns the encoded bytes, or empty vector if value is too large
    inline dp::Vector<dp::u8> varint_encode(dp::u64 value) {
        dp::Vector<dp::u8> result;

        if (value <= VARINT_1BYTE_MAX) {
            // 1 byte: prefix 00
            result.push_back(static_cast<dp::u8>(value));
        } else if (value <= VARINT_2BYTE_MAX) {
            // 2 bytes: prefix 01
            result.push_back(static_cast<dp::u8>(VARINT_2BYTE_PREFIX | ((value >> 8) & 0x3F)));
            result.push_back(static_cast<dp::u8>(value & 0xFF));
        } else if (value <= VARINT_4BYTE_MAX) {
            // 4 bytes: prefix 10
            result.push_back(static_cast<dp::u8>(VARINT_4BYTE_PREFIX | ((value >> 24) & 0x3F)));
            result.push_back(static_cast<dp::u8>((value >> 16) & 0xFF));
            result.push_back(static_cast<dp::u8>((value >> 8) & 0xFF));
            result.push_back(static_cast<dp::u8>(value & 0xFF));
        } else if (value <= VARINT_MAX) {
            // 8 bytes: prefix 11
            result.push_back(static_cast<dp::u8>(VARINT_8BYTE_PREFIX | ((value >> 56) & 0x3F)));
            result.push_back(static_cast<dp::u8>((value >> 48) & 0xFF));
            result.push_back(static_cast<dp::u8>((value >> 40) & 0xFF));
            result.push_back(static_cast<dp::u8>((value >> 32) & 0xFF));
            result.push_back(static_cast<dp::u8>((value >> 24) & 0xFF));
            result.push_back(static_cast<dp::u8>((value >> 16) & 0xFF));
            result.push_back(static_cast<dp::u8>((value >> 8) & 0xFF));
            result.push_back(static_cast<dp::u8>(value & 0xFF));
        }
        // If value > VARINT_MAX, return empty vector (caller should check)

        return result;
    }

    // Decode a QUIC variable-length integer from raw bytes
    // Returns (decoded value, bytes consumed), or error
    inline dp::Res<std::pair<dp::u64, dp::usize>> varint_decode(const dp::u8 *data, dp::usize size) {
        if (size == 0) {
            return dp::result::err(dp::Error::invalid_argument("varint: no data"));
        }

        dp::usize length = varint_length_from_first_byte(data[0]);

        if (size < length) {
            return dp::result::err(dp::Error::invalid_argument("varint: insufficient data"));
        }

        dp::u64 value = 0;

        switch (length) {
        case 1:
            value = data[0] & 0x3F;
            break;
        case 2:
            value = (static_cast<dp::u64>(data[0] & 0x3F) << 8) | static_cast<dp::u64>(data[1]);
            break;
        case 4:
            value = (static_cast<dp::u64>(data[0] & 0x3F) << 24) | (static_cast<dp::u64>(data[1]) << 16) |
                    (static_cast<dp::u64>(data[2]) << 8) | static_cast<dp::u64>(data[3]);
            break;
        case 8:
            value = (static_cast<dp::u64>(data[0] & 0x3F) << 56) | (static_cast<dp::u64>(data[1]) << 48) |
                    (static_cast<dp::u64>(data[2]) << 40) | (static_cast<dp::u64>(data[3]) << 32) |
                    (static_cast<dp::u64>(data[4]) << 24) | (static_cast<dp::u64>(data[5]) << 16) |
                    (static_cast<dp::u64>(data[6]) << 8) | static_cast<dp::u64>(data[7]);
            break;
        }

        return dp::result::ok(std::make_pair(value, length));
    }

    // Decode a varint from a vector
    inline dp::Res<std::pair<dp::u64, dp::usize>> varint_decode(const dp::Vector<dp::u8> &data, dp::usize offset = 0) {
        if (offset >= data.size()) {
            return dp::result::err(dp::Error::invalid_argument("varint: offset out of bounds"));
        }
        return varint_decode(data.data() + offset, data.size() - offset);
    }

    // VarInt class for convenient encoding/decoding
    class VarInt {
      public:
        VarInt() : value_(0) {}
        explicit VarInt(dp::u64 value) : value_(value) {}

        dp::u64 value() const { return value_; }
        void set_value(dp::u64 value) { value_ = value; }

        // Get encoded length
        dp::usize encoded_length() const { return varint_length(value_); }

        // Encode to bytes
        dp::Vector<dp::u8> encode() const { return varint_encode(value_); }

        // Decode from bytes
        static dp::Res<std::pair<VarInt, dp::usize>> decode(const dp::u8 *data, dp::usize size) {
            auto result = varint_decode(data, size);
            if (result.is_err()) {
                return dp::result::err(result.error());
            }
            auto [value, consumed] = result.value();
            return dp::result::ok(std::make_pair(VarInt(value), consumed));
        }

        // Decode from vector
        static dp::Res<std::pair<VarInt, dp::usize>> decode(const dp::Vector<dp::u8> &data, dp::usize offset = 0) {
            if (offset >= data.size()) {
                return dp::result::err(dp::Error::invalid_argument("varint: offset out of bounds"));
            }
            return decode(data.data() + offset, data.size() - offset);
        }

        // Comparison operators
        bool operator==(const VarInt &other) const { return value_ == other.value_; }
        bool operator!=(const VarInt &other) const { return value_ != other.value_; }
        bool operator<(const VarInt &other) const { return value_ < other.value_; }
        bool operator<=(const VarInt &other) const { return value_ <= other.value_; }
        bool operator>(const VarInt &other) const { return value_ > other.value_; }
        bool operator>=(const VarInt &other) const { return value_ >= other.value_; }

        // Implicit conversion to dp::u64
        operator dp::u64() const { return value_; }

      private:
        dp::u64 value_;
    };

} // namespace netpipe::quic

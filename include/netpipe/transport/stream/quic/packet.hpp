#pragma once

#include <datapod/datapod.hpp>
#include <netpipe/transport/stream/quic/types.hpp>
#include <netpipe/transport/stream/quic/varint.hpp>

namespace netpipe::quic {

    // QUIC Packet Header Constants
    constexpr dp::u8 HEADER_FORM_BIT = 0x80; // 1 = Long, 0 = Short
    constexpr dp::u8 FIXED_BIT = 0x40;       // Must be 1 (for demuxing with other protocols)
    constexpr dp::u8 LONG_PACKET_TYPE_MASK = 0x30;
    constexpr dp::u8 LONG_PACKET_TYPE_SHIFT = 4;
    constexpr dp::u8 RESERVED_BITS_MASK = 0x0C; // Reserved bits (protected)
    constexpr dp::u8 PACKET_NUMBER_LENGTH_MASK = 0x03;

    // Short header specific
    constexpr dp::u8 SHORT_SPIN_BIT = 0x20;
    constexpr dp::u8 SHORT_KEY_PHASE_BIT = 0x04;

    // Long Header Packet (RFC 9000 Section 17.2)
    //
    // Long Header Packet {
    //   Header Form (1) = 1,
    //   Fixed Bit (1) = 1,
    //   Long Packet Type (2),
    //   Type-Specific Bits (4),
    //   Version (32),
    //   Destination Connection ID Length (8),
    //   Destination Connection ID (0..160),
    //   Source Connection ID Length (8),
    //   Source Connection ID (0..160),
    //   Type-Specific Payload (..),
    // }
    struct LongHeader {
        LongPacketType packet_type;
        dp::u32 version;
        ConnectionId dest_cid;
        ConnectionId src_cid;
        dp::u8 reserved_bits = 0; // Protected by header protection
        dp::u8 pn_length = 0;     // 0-3 meaning 1-4 bytes

        // Serialize the header (without packet number - that's protected separately)
        dp::Vector<dp::u8> serialize() const {
            dp::Vector<dp::u8> result;

            // First byte: form(1) + fixed(1) + type(2) + reserved(2) + pn_length(2)
            dp::u8 first_byte = HEADER_FORM_BIT | FIXED_BIT |
                                (static_cast<dp::u8>(packet_type) << LONG_PACKET_TYPE_SHIFT) |
                                (reserved_bits & RESERVED_BITS_MASK) | (pn_length & PACKET_NUMBER_LENGTH_MASK);
            result.push_back(first_byte);

            // Version (4 bytes, big-endian)
            result.push_back(static_cast<dp::u8>((version >> 24) & 0xFF));
            result.push_back(static_cast<dp::u8>((version >> 16) & 0xFF));
            result.push_back(static_cast<dp::u8>((version >> 8) & 0xFF));
            result.push_back(static_cast<dp::u8>(version & 0xFF));

            // Destination Connection ID Length + Data
            result.push_back(static_cast<dp::u8>(dest_cid.size()));
            result.insert(result.end(), dest_cid.data.begin(), dest_cid.data.end());

            // Source Connection ID Length + Data
            result.push_back(static_cast<dp::u8>(src_cid.size()));
            result.insert(result.end(), src_cid.data.begin(), src_cid.data.end());

            return result;
        }

        // Parse a long header from bytes (returns header and bytes consumed)
        static dp::Res<std::pair<LongHeader, dp::usize>> parse(const dp::u8 *data, dp::usize size) {
            if (size < 7) { // Minimum: first_byte + version + dcid_len + scid_len
                return dp::result::err(dp::Error::invalid_argument("long header too short"));
            }

            if ((data[0] & HEADER_FORM_BIT) == 0) {
                return dp::result::err(dp::Error::invalid_argument("not a long header"));
            }

            LongHeader header;
            dp::usize offset = 0;

            // First byte
            header.packet_type =
                static_cast<LongPacketType>((data[0] & LONG_PACKET_TYPE_MASK) >> LONG_PACKET_TYPE_SHIFT);
            header.reserved_bits = data[0] & RESERVED_BITS_MASK;
            header.pn_length = data[0] & PACKET_NUMBER_LENGTH_MASK;
            offset++;

            // Version
            header.version = (static_cast<dp::u32>(data[offset]) << 24) |
                             (static_cast<dp::u32>(data[offset + 1]) << 16) |
                             (static_cast<dp::u32>(data[offset + 2]) << 8) | static_cast<dp::u32>(data[offset + 3]);
            offset += 4;

            // Destination Connection ID
            if (offset >= size) {
                return dp::result::err(dp::Error::invalid_argument("long header truncated at dcid length"));
            }
            dp::usize dcid_len = data[offset++];
            if (dcid_len > MAX_CID_LENGTH || offset + dcid_len > size) {
                return dp::result::err(dp::Error::invalid_argument("invalid dcid length"));
            }
            header.dest_cid = ConnectionId::from_bytes(data + offset, dcid_len);
            offset += dcid_len;

            // Source Connection ID
            if (offset >= size) {
                return dp::result::err(dp::Error::invalid_argument("long header truncated at scid length"));
            }
            dp::usize scid_len = data[offset++];
            if (scid_len > MAX_CID_LENGTH || offset + scid_len > size) {
                return dp::result::err(dp::Error::invalid_argument("invalid scid length"));
            }
            header.src_cid = ConnectionId::from_bytes(data + offset, scid_len);
            offset += scid_len;

            return dp::result::ok(std::make_pair(std::move(header), offset));
        }
    };

    // Initial Packet specific fields
    struct InitialPacket {
        LongHeader header;
        dp::Vector<dp::u8> token; // Token for retry validation
        dp::u64 packet_number;
        dp::Vector<dp::u8> payload; // Encrypted CRYPTO frames

        // Serialize (excluding encryption - that's done by crypto layer)
        dp::Vector<dp::u8> serialize_header_and_token() const {
            auto result = header.serialize();

            // Token length (varint) + token
            auto token_len_bytes = varint_encode(token.size());
            result.insert(result.end(), token_len_bytes.begin(), token_len_bytes.end());
            result.insert(result.end(), token.begin(), token.end());

            return result;
        }

        // Parse Initial packet header (after long header)
        static dp::Res<std::pair<InitialPacket, dp::usize>> parse(const dp::u8 *data, dp::usize size) {
            auto header_result = LongHeader::parse(data, size);
            if (header_result.is_err()) {
                return dp::result::err(header_result.error());
            }

            auto [header, offset] = header_result.value();
            if (header.packet_type != LongPacketType::Initial) {
                return dp::result::err(dp::Error::invalid_argument("not an Initial packet"));
            }

            InitialPacket packet;
            packet.header = std::move(header);

            // Token length (varint)
            auto token_len_result = varint_decode(data + offset, size - offset);
            if (token_len_result.is_err()) {
                return dp::result::err(token_len_result.error());
            }
            auto [token_len, token_len_bytes] = token_len_result.value();
            offset += token_len_bytes;

            // Token
            if (offset + token_len > size) {
                return dp::result::err(dp::Error::invalid_argument("token truncated"));
            }
            packet.token = dp::Vector<dp::u8>(data + offset, data + offset + token_len);
            offset += token_len;

            // Length (varint) - length of packet number + payload
            auto length_result = varint_decode(data + offset, size - offset);
            if (length_result.is_err()) {
                return dp::result::err(length_result.error());
            }
            auto [length, length_bytes] = length_result.value();
            offset += length_bytes;

            // The remaining data is packet_number (protected) + encrypted payload
            // We return offset here; caller handles decryption
            return dp::result::ok(std::make_pair(std::move(packet), offset));
        }
    };

    // Handshake Packet specific fields
    struct HandshakePacket {
        LongHeader header;
        dp::u64 packet_number;
        dp::Vector<dp::u8> payload;

        dp::Vector<dp::u8> serialize_header() const { return header.serialize(); }

        static dp::Res<std::pair<HandshakePacket, dp::usize>> parse(const dp::u8 *data, dp::usize size) {
            auto header_result = LongHeader::parse(data, size);
            if (header_result.is_err()) {
                return dp::result::err(header_result.error());
            }

            auto [header, offset] = header_result.value();
            if (header.packet_type != LongPacketType::Handshake) {
                return dp::result::err(dp::Error::invalid_argument("not a Handshake packet"));
            }

            HandshakePacket packet;
            packet.header = std::move(header);

            // Length (varint)
            auto length_result = varint_decode(data + offset, size - offset);
            if (length_result.is_err()) {
                return dp::result::err(length_result.error());
            }
            auto [length, length_bytes] = length_result.value();
            offset += length_bytes;

            return dp::result::ok(std::make_pair(std::move(packet), offset));
        }
    };

    // 0-RTT Packet
    struct ZeroRTTPacket {
        LongHeader header;
        dp::u64 packet_number;
        dp::Vector<dp::u8> payload;

        dp::Vector<dp::u8> serialize_header() const { return header.serialize(); }

        static dp::Res<std::pair<ZeroRTTPacket, dp::usize>> parse(const dp::u8 *data, dp::usize size) {
            auto header_result = LongHeader::parse(data, size);
            if (header_result.is_err()) {
                return dp::result::err(header_result.error());
            }

            auto [header, offset] = header_result.value();
            if (header.packet_type != LongPacketType::ZeroRTT) {
                return dp::result::err(dp::Error::invalid_argument("not a 0-RTT packet"));
            }

            ZeroRTTPacket packet;
            packet.header = std::move(header);

            // Length (varint)
            auto length_result = varint_decode(data + offset, size - offset);
            if (length_result.is_err()) {
                return dp::result::err(length_result.error());
            }
            offset += length_result.value().second;

            return dp::result::ok(std::make_pair(std::move(packet), offset));
        }
    };

    // Retry Packet (no encryption, no packet number)
    struct RetryPacket {
        LongHeader header;
        dp::Vector<dp::u8> retry_token;
        dp::Vector<dp::u8> retry_integrity_tag; // 128-bit tag

        static constexpr dp::usize RETRY_INTEGRITY_TAG_LENGTH = 16;

        dp::Vector<dp::u8> serialize() const {
            auto result = header.serialize();
            result.insert(result.end(), retry_token.begin(), retry_token.end());
            result.insert(result.end(), retry_integrity_tag.begin(), retry_integrity_tag.end());
            return result;
        }

        static dp::Res<RetryPacket> parse(const dp::u8 *data, dp::usize size) {
            auto header_result = LongHeader::parse(data, size);
            if (header_result.is_err()) {
                return dp::result::err(header_result.error());
            }

            auto [header, offset] = header_result.value();
            if (header.packet_type != LongPacketType::Retry) {
                return dp::result::err(dp::Error::invalid_argument("not a Retry packet"));
            }

            if (size < offset + RETRY_INTEGRITY_TAG_LENGTH) {
                return dp::result::err(dp::Error::invalid_argument("Retry packet too short"));
            }

            RetryPacket packet;
            packet.header = std::move(header);

            // Everything except the last 16 bytes is the retry token
            dp::usize token_len = size - offset - RETRY_INTEGRITY_TAG_LENGTH;
            packet.retry_token = dp::Vector<dp::u8>(data + offset, data + offset + token_len);
            offset += token_len;

            // Last 16 bytes are the integrity tag
            packet.retry_integrity_tag = dp::Vector<dp::u8>(data + offset, data + offset + RETRY_INTEGRITY_TAG_LENGTH);

            return dp::result::ok(std::move(packet));
        }
    };

    // Short Header Packet (1-RTT) (RFC 9000 Section 17.3)
    //
    // 1-RTT Packet {
    //   Header Form (1) = 0,
    //   Fixed Bit (1) = 1,
    //   Spin Bit (1),
    //   Reserved Bits (2),
    //   Key Phase (1),
    //   Packet Number Length (2),
    //   Destination Connection ID (0..160),
    //   Packet Number (8..32),
    //   Packet Payload (..),
    // }
    struct ShortHeader {
        bool spin_bit = false;
        dp::u8 reserved_bits = 0;
        bool key_phase = false;
        dp::u8 pn_length = 0; // 0-3 meaning 1-4 bytes
        ConnectionId dest_cid;

        dp::Vector<dp::u8> serialize() const {
            dp::Vector<dp::u8> result;

            // First byte
            dp::u8 first_byte = FIXED_BIT | (spin_bit ? SHORT_SPIN_BIT : 0) | (reserved_bits & RESERVED_BITS_MASK) |
                                (key_phase ? SHORT_KEY_PHASE_BIT : 0) | (pn_length & PACKET_NUMBER_LENGTH_MASK);
            result.push_back(first_byte);

            // Destination Connection ID (length is known from context)
            result.insert(result.end(), dest_cid.data.begin(), dest_cid.data.end());

            return result;
        }

        // Parse short header (dcid_length must be known from connection context)
        static dp::Res<std::pair<ShortHeader, dp::usize>> parse(const dp::u8 *data, dp::usize size,
                                                                dp::usize dcid_length) {
            if (size < 1 + dcid_length) {
                return dp::result::err(dp::Error::invalid_argument("short header too short"));
            }

            if ((data[0] & HEADER_FORM_BIT) != 0) {
                return dp::result::err(dp::Error::invalid_argument("not a short header"));
            }

            ShortHeader header;
            header.spin_bit = (data[0] & SHORT_SPIN_BIT) != 0;
            header.reserved_bits = data[0] & RESERVED_BITS_MASK;
            header.key_phase = (data[0] & SHORT_KEY_PHASE_BIT) != 0;
            header.pn_length = data[0] & PACKET_NUMBER_LENGTH_MASK;
            header.dest_cid = ConnectionId::from_bytes(data + 1, dcid_length);

            return dp::result::ok(std::make_pair(std::move(header), 1 + dcid_length));
        }
    };

    // One-RTT Packet (uses short header)
    struct OneRTTPacket {
        ShortHeader header;
        dp::u64 packet_number;
        dp::Vector<dp::u8> payload;

        dp::Vector<dp::u8> serialize_header() const { return header.serialize(); }
    };

    // Version Negotiation Packet (RFC 9000 Section 17.2.1)
    // Sent by server when client's version is not supported
    struct VersionNegotiationPacket {
        ConnectionId dest_cid;
        ConnectionId src_cid;
        dp::Vector<dp::u32> supported_versions;

        dp::Vector<dp::u8> serialize() const {
            dp::Vector<dp::u8> result;

            // First byte: random bits with form=1
            result.push_back(HEADER_FORM_BIT | 0x00); // Version negotiation uses random low bits

            // Version = 0 (indicates version negotiation)
            result.push_back(0);
            result.push_back(0);
            result.push_back(0);
            result.push_back(0);

            // Destination Connection ID
            result.push_back(static_cast<dp::u8>(dest_cid.size()));
            result.insert(result.end(), dest_cid.data.begin(), dest_cid.data.end());

            // Source Connection ID
            result.push_back(static_cast<dp::u8>(src_cid.size()));
            result.insert(result.end(), src_cid.data.begin(), src_cid.data.end());

            // Supported versions
            for (auto v : supported_versions) {
                result.push_back(static_cast<dp::u8>((v >> 24) & 0xFF));
                result.push_back(static_cast<dp::u8>((v >> 16) & 0xFF));
                result.push_back(static_cast<dp::u8>((v >> 8) & 0xFF));
                result.push_back(static_cast<dp::u8>(v & 0xFF));
            }

            return result;
        }

        static dp::Res<VersionNegotiationPacket> parse(const dp::u8 *data, dp::usize size) {
            if (size < 7) {
                return dp::result::err(dp::Error::invalid_argument("version negotiation packet too short"));
            }

            // Check version is 0
            dp::u32 version = (static_cast<dp::u32>(data[1]) << 24) | (static_cast<dp::u32>(data[2]) << 16) |
                              (static_cast<dp::u32>(data[3]) << 8) | static_cast<dp::u32>(data[4]);
            if (version != 0) {
                return dp::result::err(dp::Error::invalid_argument("not a version negotiation packet"));
            }

            dp::usize offset = 5;

            VersionNegotiationPacket packet;

            // Destination Connection ID
            if (offset >= size) {
                return dp::result::err(dp::Error::invalid_argument("version negotiation truncated"));
            }
            dp::usize dcid_len = data[offset++];
            if (offset + dcid_len > size) {
                return dp::result::err(dp::Error::invalid_argument("dcid truncated"));
            }
            packet.dest_cid = ConnectionId::from_bytes(data + offset, dcid_len);
            offset += dcid_len;

            // Source Connection ID
            if (offset >= size) {
                return dp::result::err(dp::Error::invalid_argument("version negotiation truncated"));
            }
            dp::usize scid_len = data[offset++];
            if (offset + scid_len > size) {
                return dp::result::err(dp::Error::invalid_argument("scid truncated"));
            }
            packet.src_cid = ConnectionId::from_bytes(data + offset, scid_len);
            offset += scid_len;

            // Supported versions
            while (offset + 4 <= size) {
                dp::u32 v = (static_cast<dp::u32>(data[offset]) << 24) |
                            (static_cast<dp::u32>(data[offset + 1]) << 16) |
                            (static_cast<dp::u32>(data[offset + 2]) << 8) | static_cast<dp::u32>(data[offset + 3]);
                packet.supported_versions.push_back(v);
                offset += 4;
            }

            return dp::result::ok(std::move(packet));
        }
    };

    // Encode packet number (1-4 bytes based on pn_length field)
    inline dp::Vector<dp::u8> encode_packet_number(dp::u64 pn, dp::u8 pn_length) {
        dp::Vector<dp::u8> result;
        dp::usize num_bytes = pn_length + 1; // pn_length is 0-3 meaning 1-4 bytes

        for (dp::usize i = num_bytes; i > 0; i--) {
            result.push_back(static_cast<dp::u8>((pn >> ((i - 1) * 8)) & 0xFF));
        }

        return result;
    }

    // Decode packet number (after header protection is removed)
    inline dp::u64 decode_packet_number(const dp::u8 *data, dp::u8 pn_length) {
        dp::usize num_bytes = pn_length + 1;
        dp::u64 pn = 0;

        for (dp::usize i = 0; i < num_bytes; i++) {
            pn = (pn << 8) | data[i];
        }

        return pn;
    }

    // Expand a truncated packet number based on the largest acknowledged PN
    // RFC 9000 Appendix A
    inline dp::u64 expand_packet_number(dp::u64 largest_pn, dp::u64 truncated_pn, dp::u8 pn_length) {
        dp::usize pn_nbits = (pn_length + 1) * 8;
        dp::u64 expected_pn = largest_pn + 1;
        dp::u64 pn_win = 1ULL << pn_nbits;
        dp::u64 pn_hwin = pn_win / 2;
        dp::u64 pn_mask = pn_win - 1;

        // The upper bits of the packet number
        dp::u64 candidate_pn = (expected_pn & ~pn_mask) | truncated_pn;

        if (candidate_pn <= expected_pn - pn_hwin && candidate_pn < (1ULL << 62) - pn_win) {
            return candidate_pn + pn_win;
        }
        if (candidate_pn > expected_pn + pn_hwin && candidate_pn >= pn_win) {
            return candidate_pn - pn_win;
        }
        return candidate_pn;
    }

    // Determine the minimum packet number length needed
    inline dp::u8 packet_number_length(dp::u64 pn, dp::u64 largest_acked) {
        dp::u64 range = pn - largest_acked;

        if (range < (1 << 7)) {
            return 0; // 1 byte
        } else if (range < (1 << 15)) {
            return 1; // 2 bytes
        } else if (range < (1 << 23)) {
            return 2; // 3 bytes
        } else {
            return 3; // 4 bytes
        }
    }

    // Check if packet is a long header packet
    inline bool is_long_header(dp::u8 first_byte) { return (first_byte & HEADER_FORM_BIT) != 0; }

    // Check if packet is a short header packet
    inline bool is_short_header(dp::u8 first_byte) { return (first_byte & HEADER_FORM_BIT) == 0; }

    // Get packet type from first byte (only valid for long headers)
    inline LongPacketType get_long_packet_type(dp::u8 first_byte) {
        return static_cast<LongPacketType>((first_byte & LONG_PACKET_TYPE_MASK) >> LONG_PACKET_TYPE_SHIFT);
    }

} // namespace netpipe::quic

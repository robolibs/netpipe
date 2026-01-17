#pragma once

#include <datapod/datapod.hpp>
#include <keylock/utils/common.hpp>

namespace netpipe::quic {

    // QUIC versions (RFC 9000)
    constexpr dp::u32 QUIC_VERSION_1 = 0x00000001;
    constexpr dp::u32 QUIC_VERSION_2 = 0x6b3343cf;

    // Packet number spaces
    enum class PacketNumberSpace : dp::u8 { Initial = 0, Handshake = 1, ApplicationData = 2 };

    // Long header packet types
    enum class LongPacketType : dp::u8 { Initial = 0, ZeroRTT = 1, Handshake = 2, Retry = 3 };

    // Maximum connection ID length
    constexpr dp::usize MAX_CID_LENGTH = 20;

    // Transport error codes (RFC 9000 Section 20)
    enum class TransportError : dp::u64 {
        NoError = 0x00,
        InternalError = 0x01,
        ConnectionRefused = 0x02,
        FlowControlError = 0x03,
        StreamLimitError = 0x04,
        StreamStateError = 0x05,
        FinalSizeError = 0x06,
        FrameEncodingError = 0x07,
        TransportParameterError = 0x08,
        ConnectionIdLimitError = 0x09,
        ProtocolViolation = 0x0a,
        InvalidToken = 0x0b,
        ApplicationError = 0x0c,
        CryptoBufferExceeded = 0x0d,
        KeyUpdateError = 0x0e,
        AeadLimitReached = 0x0f,
        NoViablePath = 0x10,
        CryptoError = 0x100 // 0x100-0x1ff reserved for TLS alerts
    };

    // Connection ID (0-20 bytes)
    struct ConnectionId {
        dp::Vector<dp::u8> data;

        ConnectionId() = default;

        explicit ConnectionId(dp::Vector<dp::u8> d) : data(std::move(d)) {}

        bool empty() const { return data.empty(); }
        dp::usize size() const { return data.size(); }

        const dp::u8 *bytes() const { return data.data(); }

        bool operator==(const ConnectionId &other) const { return data == other.data; }
        bool operator!=(const ConnectionId &other) const { return data != other.data; }
        bool operator<(const ConnectionId &other) const { return data < other.data; }

        // Generate a random connection ID
        static ConnectionId generate(dp::usize length = 8) {
            if (length > MAX_CID_LENGTH) {
                length = MAX_CID_LENGTH;
            }
            auto random = keylock::utils::Common::generate_random_bytes(length);
            return ConnectionId(dp::Vector<dp::u8>(random.begin(), random.end()));
        }

        // Create from raw bytes
        static ConnectionId from_bytes(const dp::u8 *data, dp::usize len) {
            return ConnectionId(dp::Vector<dp::u8>(data, data + len));
        }

        // Serialize (length prefix + data)
        dp::Vector<dp::u8> serialize() const {
            dp::Vector<dp::u8> result;
            result.push_back(static_cast<dp::u8>(data.size()));
            result.insert(result.end(), data.begin(), data.end());
            return result;
        }
    };

    // Minimum packet sizes
    constexpr dp::usize MIN_INITIAL_PACKET_SIZE = 1200; // MUST pad Initial packets to >= 1200 bytes
    constexpr dp::usize MAX_UDP_PAYLOAD_SIZE = 65527;

    // Packet number limits
    constexpr dp::u64 MAX_PACKET_NUMBER = (1ULL << 62) - 1;

    // Stream ID helpers
    // Client-initiated bidirectional:  0, 4, 8, ...   (ID & 0x3 == 0)
    // Server-initiated bidirectional:  1, 5, 9, ...   (ID & 0x3 == 1)
    // Client-initiated unidirectional: 2, 6, 10, ...  (ID & 0x3 == 2)
    // Server-initiated unidirectional: 3, 7, 11, ...  (ID & 0x3 == 3)

    enum class StreamType : dp::u8 {
        ClientBidirectional = 0,
        ServerBidirectional = 1,
        ClientUnidirectional = 2,
        ServerUnidirectional = 3
    };

    inline StreamType stream_type(dp::u64 stream_id) { return static_cast<StreamType>(stream_id & 0x3); }

    inline bool is_client_initiated(dp::u64 stream_id) { return (stream_id & 0x1) == 0; }

    inline bool is_bidirectional(dp::u64 stream_id) { return (stream_id & 0x2) == 0; }

    inline bool is_unidirectional(dp::u64 stream_id) { return (stream_id & 0x2) != 0; }

    // Get next stream ID for a given type
    inline dp::u64 next_stream_id(dp::u64 current, StreamType type) {
        if (current == 0) {
            return static_cast<dp::u64>(type);
        }
        return current + 4; // Stream IDs increment by 4
    }

    // Encryption levels
    enum class EncryptionLevel : dp::u8 { Initial = 0, ZeroRTT = 1, Handshake = 2, OneRTT = 3 };

    // Map packet type to encryption level
    inline EncryptionLevel packet_type_to_level(LongPacketType type) {
        switch (type) {
        case LongPacketType::Initial:
            return EncryptionLevel::Initial;
        case LongPacketType::ZeroRTT:
            return EncryptionLevel::ZeroRTT;
        case LongPacketType::Handshake:
            return EncryptionLevel::Handshake;
        default:
            return EncryptionLevel::Initial;
        }
    }

    // Map encryption level to packet number space
    inline PacketNumberSpace level_to_space(EncryptionLevel level) {
        switch (level) {
        case EncryptionLevel::Initial:
            return PacketNumberSpace::Initial;
        case EncryptionLevel::Handshake:
            return PacketNumberSpace::Handshake;
        case EncryptionLevel::ZeroRTT:
        case EncryptionLevel::OneRTT:
            return PacketNumberSpace::ApplicationData;
        }
        return PacketNumberSpace::Initial;
    }

    // Convert transport error to string
    inline const char *transport_error_to_string(TransportError err) {
        switch (err) {
        case TransportError::NoError:
            return "NO_ERROR";
        case TransportError::InternalError:
            return "INTERNAL_ERROR";
        case TransportError::ConnectionRefused:
            return "CONNECTION_REFUSED";
        case TransportError::FlowControlError:
            return "FLOW_CONTROL_ERROR";
        case TransportError::StreamLimitError:
            return "STREAM_LIMIT_ERROR";
        case TransportError::StreamStateError:
            return "STREAM_STATE_ERROR";
        case TransportError::FinalSizeError:
            return "FINAL_SIZE_ERROR";
        case TransportError::FrameEncodingError:
            return "FRAME_ENCODING_ERROR";
        case TransportError::TransportParameterError:
            return "TRANSPORT_PARAMETER_ERROR";
        case TransportError::ConnectionIdLimitError:
            return "CONNECTION_ID_LIMIT_ERROR";
        case TransportError::ProtocolViolation:
            return "PROTOCOL_VIOLATION";
        case TransportError::InvalidToken:
            return "INVALID_TOKEN";
        case TransportError::ApplicationError:
            return "APPLICATION_ERROR";
        case TransportError::CryptoBufferExceeded:
            return "CRYPTO_BUFFER_EXCEEDED";
        case TransportError::KeyUpdateError:
            return "KEY_UPDATE_ERROR";
        case TransportError::AeadLimitReached:
            return "AEAD_LIMIT_REACHED";
        case TransportError::NoViablePath:
            return "NO_VIABLE_PATH";
        default:
            if (static_cast<dp::u64>(err) >= 0x100 && static_cast<dp::u64>(err) <= 0x1ff) {
                return "CRYPTO_ERROR";
            }
            return "UNKNOWN_ERROR";
        }
    }

} // namespace netpipe::quic

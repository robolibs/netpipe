#pragma once

#include <netpipe/common.hpp>

namespace netpipe {
    namespace remote {

        /// Protocol version
        constexpr dp::u8 PROTOCOL_VERSION_1 = 1; // Original: [request_id:4][is_error:1][length:4][payload:N]
        constexpr dp::u8 PROTOCOL_VERSION_2 =
            2; // Enhanced: [version:1][type:1][flags:2][request_id:4][method_id:4][length:4][payload:N]
        constexpr dp::u8 PROTOCOL_VERSION_CURRENT = PROTOCOL_VERSION_2;

        /// Message types
        enum class MessageType : dp::u8 {
            Request = 0,      // Client request
            Response = 1,     // Server success response
            Error = 2,        // Server error response
            Notification = 3, // One-way message (no response expected)
            StreamData = 4,   // Streaming data chunk
            StreamEnd = 5,    // End of stream
            StreamError = 6   // Stream error
        };

        /// Message flags (bitfield)
        namespace MessageFlags {
            constexpr dp::u16 None = 0x0000;
            constexpr dp::u16 Compressed = 0x0001;  // Payload is compressed
            constexpr dp::u16 Streaming = 0x0002;   // Part of a stream
            constexpr dp::u16 RequiresAck = 0x0004; // Requires acknowledgment
            constexpr dp::u16 Final = 0x0008;       // Final message in sequence
        } // namespace MessageFlags

        /// Wire protocol definitions for Remote RPC
        /// V1 Format: [request_id:4][is_error:1][length:4][payload:N]
        /// V2 Format: [version:1][type:1][flags:2][request_id:4][method_id:4][length:4][payload:N]
        /// All multi-byte integers are big-endian

        /// Encode Remote message: [request_id:4][is_error:1][length:4][payload:N]
        inline Message encode_remote_message(dp::u32 request_id, const Message &payload, bool is_error = false) {
            Message msg;

            // Encode request_id (4 bytes big-endian)
            auto id_bytes = encode_u32_be(request_id);
            msg.insert(msg.end(), id_bytes.begin(), id_bytes.end());

            // Encode is_error flag (1 byte)
            msg.push_back(is_error ? 1 : 0);

            // Encode payload length (4 bytes big-endian)
            auto len_bytes = encode_u32_be(static_cast<dp::u32>(payload.size()));
            msg.insert(msg.end(), len_bytes.begin(), len_bytes.end());

            // Append payload
            msg.insert(msg.end(), payload.begin(), payload.end());

            return msg;
        }

        /// Result of decoding a Remote message
        struct DecodedMessage {
            dp::u32 request_id;
            bool is_error;
            Message payload;
        };

        /// Decode Remote message: extract request_id, is_error flag, and payload
        inline dp::Res<DecodedMessage> decode_remote_message(const Message &msg) {
            if (msg.size() < 9) {
                echo::error("remote message too short: ", msg.size());
                return dp::result::err(dp::Error::invalid_argument("message too short"));
            }

            // Decode request_id
            dp::u32 request_id = decode_u32_be(msg.data());

            // Decode is_error flag
            bool is_error = (msg[4] != 0);

            // Decode payload length
            dp::u32 payload_length = decode_u32_be(msg.data() + 5);

            // Verify message size
            if (msg.size() != 9 + payload_length) {
                echo::error("remote message size mismatch: expected ", 9 + payload_length, " got ", msg.size());
                return dp::result::err(dp::Error::invalid_argument("message size mismatch"));
            }

            // Extract payload
            Message payload(msg.begin() + 9, msg.end());

            DecodedMessage decoded{request_id, is_error, std::move(payload)};
            return dp::result::ok(std::move(decoded));
        }

        /// Legacy decode function for backward compatibility
        /// Returns (request_id, payload) pair
        inline dp::Res<dp::Pair<dp::u32, Message>> decode_remote_message_legacy(const Message &msg) {
            auto res = decode_remote_message(msg);
            if (res.is_err()) {
                return dp::result::err(res.error());
            }
            auto decoded = res.value();
            return dp::result::ok(dp::Pair<dp::u32, Message>(decoded.request_id, std::move(decoded.payload)));
        }

        // ============================================================================
        // Protocol V2 - Enhanced with versioning, types, flags, and method routing
        // ============================================================================

        /// Result of decoding a V2 Remote message
        struct DecodedMessageV2 {
            dp::u8 version;
            MessageType type;
            dp::u16 flags;
            dp::u32 request_id;
            dp::u32 method_id;
            Message payload;
        };

        /// Encode V2 Remote message: [version:1][type:1][flags:2][request_id:4][method_id:4][length:4][payload:N]
        inline Message encode_remote_message_v2(dp::u32 request_id, dp::u32 method_id, const Message &payload,
                                                MessageType type = MessageType::Request,
                                                dp::u16 flags = MessageFlags::None) {
            Message msg;

            // Encode version (1 byte)
            msg.push_back(PROTOCOL_VERSION_2);

            // Encode type (1 byte)
            msg.push_back(static_cast<dp::u8>(type));

            // Encode flags (2 bytes big-endian)
            msg.push_back(static_cast<dp::u8>((flags >> 8) & 0xFF));
            msg.push_back(static_cast<dp::u8>(flags & 0xFF));

            // Encode request_id (4 bytes big-endian)
            auto id_bytes = encode_u32_be(request_id);
            msg.insert(msg.end(), id_bytes.begin(), id_bytes.end());

            // Encode method_id (4 bytes big-endian)
            auto method_bytes = encode_u32_be(method_id);
            msg.insert(msg.end(), method_bytes.begin(), method_bytes.end());

            // Encode payload length (4 bytes big-endian)
            auto len_bytes = encode_u32_be(static_cast<dp::u32>(payload.size()));
            msg.insert(msg.end(), len_bytes.begin(), len_bytes.end());

            // Append payload
            msg.insert(msg.end(), payload.begin(), payload.end());

            return msg;
        }

        /// Decode V2 Remote message: extract all fields
        inline dp::Res<DecodedMessageV2> decode_remote_message_v2(const Message &msg) {
            if (msg.size() < 16) {
                echo::error("remote v2 message too short: ", msg.size());
                return dp::result::err(dp::Error::invalid_argument("message too short"));
            }

            // Decode version
            dp::u8 version = msg[0];
            if (version != PROTOCOL_VERSION_2) {
                echo::error("unsupported protocol version: ", static_cast<int>(version));
                return dp::result::err(dp::Error::invalid_argument("unsupported protocol version"));
            }

            // Decode type
            MessageType type = static_cast<MessageType>(msg[1]);

            // Decode flags (2 bytes big-endian)
            dp::u16 flags = (static_cast<dp::u16>(msg[2]) << 8) | static_cast<dp::u16>(msg[3]);

            // Decode request_id
            dp::u32 request_id = decode_u32_be(msg.data() + 4);

            // Decode method_id
            dp::u32 method_id = decode_u32_be(msg.data() + 8);

            // Decode payload length
            dp::u32 payload_length = decode_u32_be(msg.data() + 12);

            // Verify message size
            if (msg.size() != 16 + payload_length) {
                echo::error("remote v2 message size mismatch: expected ", 16 + payload_length, " got ", msg.size());
                return dp::result::err(dp::Error::invalid_argument("message size mismatch"));
            }

            // Extract payload
            Message payload(msg.begin() + 16, msg.end());

            DecodedMessageV2 decoded{version, type, flags, request_id, method_id, std::move(payload)};
            return dp::result::ok(std::move(decoded));
        }

        /// Auto-detect protocol version and decode accordingly
        inline dp::Res<DecodedMessageV2> decode_remote_message_auto(const Message &msg) {
            if (msg.size() < 1) {
                echo::error("message too short to detect version");
                return dp::result::err(dp::Error::invalid_argument("message too short"));
            }

            // Check if first byte looks like a version number
            dp::u8 first_byte = msg[0];

            if (first_byte == PROTOCOL_VERSION_2 && msg.size() >= 16) {
                // V2 protocol
                return decode_remote_message_v2(msg);
            } else if (msg.size() >= 9) {
                // V1 protocol - convert to V2 format
                auto v1_res = decode_remote_message(msg);
                if (v1_res.is_err()) {
                    return dp::result::err(v1_res.error());
                }
                auto v1_decoded = v1_res.value();

                // Convert V1 to V2 format
                MessageType type = v1_decoded.is_error ? MessageType::Error : MessageType::Response;
                DecodedMessageV2 v2_decoded{PROTOCOL_VERSION_1,           type, MessageFlags::None,
                                            v1_decoded.request_id,        0, // method_id = 0 for V1
                                            std::move(v1_decoded.payload)};
                return dp::result::ok(std::move(v2_decoded));
            } else {
                echo::error("cannot detect protocol version");
                return dp::result::err(dp::Error::invalid_argument("cannot detect protocol version"));
            }
        }

    } // namespace remote
} // namespace netpipe

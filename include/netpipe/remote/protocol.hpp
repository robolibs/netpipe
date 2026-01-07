#pragma once

#include <netpipe/common.hpp>

namespace netpipe {
    namespace remote {

        /// Wire protocol definitions for Remote RPC
        /// Format: [request_id:4][is_error:1][length:4][payload:N]
        /// All multi-byte integers are big-endian
        /// is_error: 0 = success response, 1 = error response

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

    } // namespace remote
} // namespace netpipe

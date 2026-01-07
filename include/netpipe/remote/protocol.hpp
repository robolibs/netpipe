#pragma once

#include <netpipe/common.hpp>

namespace netpipe {
    namespace remote {

        /// Wire protocol definitions for Remote RPC
        /// Current format: [request_id:4][length:4][payload:N]
        /// All multi-byte integers are big-endian

        /// Encode Remote message: [request_id:4][length:4][payload:N]
        inline Message encode_remote_message(dp::u32 request_id, const Message &payload) {
            Message msg;

            // Encode request_id (4 bytes big-endian)
            auto id_bytes = encode_u32_be(request_id);
            msg.insert(msg.end(), id_bytes.begin(), id_bytes.end());

            // Encode payload length (4 bytes big-endian)
            auto len_bytes = encode_u32_be(static_cast<dp::u32>(payload.size()));
            msg.insert(msg.end(), len_bytes.begin(), len_bytes.end());

            // Append payload
            msg.insert(msg.end(), payload.begin(), payload.end());

            return msg;
        }

        /// Decode Remote message: extract request_id and payload
        inline dp::Res<dp::Pair<dp::u32, Message>> decode_remote_message(const Message &msg) {
            if (msg.size() < 8) {
                echo::error("remote message too short: ", msg.size());
                return dp::result::err(dp::Error::invalid_argument("message too short"));
            }

            // Decode request_id
            dp::u32 request_id = decode_u32_be(msg.data());

            // Decode payload length
            dp::u32 payload_length = decode_u32_be(msg.data() + 4);

            // Verify message size
            if (msg.size() != 8 + payload_length) {
                echo::error("remote message size mismatch: expected ", 8 + payload_length, " got ", msg.size());
                return dp::result::err(dp::Error::invalid_argument("message size mismatch"));
            }

            // Extract payload
            Message payload(msg.begin() + 8, msg.end());

            return dp::result::ok(dp::Pair<dp::u32, Message>(request_id, std::move(payload)));
        }

    } // namespace remote
} // namespace netpipe

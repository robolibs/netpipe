#pragma once

#include <functional>
#include <netpipe/stream.hpp>

namespace netpipe {

    // RPC layer on top of Stream
    // Provides request-response semantics with request ID matching
    // Single-threaded, synchronous, one outstanding call at a time
    class Rpc {
      public:
        using Handler = std::function<Message(const Message &)>;

      private:
        Stream &stream_;
        dp::u32 next_request_id_;

        // Encode RPC message: [request_id:4][length:4][payload:N]
        Message encode_rpc_message(dp::u32 request_id, const Message &payload) {
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

        // Decode RPC message: extract request_id and payload
        dp::Res<dp::Pair<dp::u32, Message>> decode_rpc_message(const Message &msg) {
            if (msg.size() < 8) {
                echo::error("rpc message too short: ", msg.size());
                return dp::result::err(dp::Error::invalid_argument("message too short"));
            }

            // Decode request_id
            dp::u32 request_id = decode_u32_be(msg.data());

            // Decode payload length
            dp::u32 payload_length = decode_u32_be(msg.data() + 4);

            // Verify message size
            if (msg.size() != 8 + payload_length) {
                echo::error("rpc message size mismatch: expected ", 8 + payload_length, " got ", msg.size());
                return dp::result::err(dp::Error::invalid_argument("message size mismatch"));
            }

            // Extract payload
            Message payload(msg.begin() + 8, msg.end());

            return dp::result::ok(dp::Pair<dp::u32, Message>(request_id, std::move(payload)));
        }

      public:
        explicit Rpc(Stream &stream) : stream_(stream), next_request_id_(0) { echo::trace("Rpc constructed"); }

        // Client side: send request and wait for response
        // Blocks until response arrives or timeout
        dp::Res<Message> call(const Message &request, dp::u32 timeout_ms = 5000) {
            dp::u32 request_id = next_request_id_++;
            echo::trace("rpc call id=", request_id, " len=", request.size());
            echo::debug("rpc call timeout_ms=", timeout_ms);

            // Encode request
            Message rpc_request = encode_rpc_message(request_id, request);

            // Send request
            auto send_res = stream_.send(rpc_request);
            if (send_res.is_err()) {
                echo::error("rpc send failed");
                return dp::result::err(send_res.error());
            }

            // Receive response
            // TODO: Implement timeout using select/poll or SO_RCVTIMEO
            // For now, just blocking recv
            auto recv_res = stream_.recv();
            if (recv_res.is_err()) {
                echo::error("rpc recv failed");
                return dp::result::err(recv_res.error());
            }

            // Decode response
            auto decode_res = decode_rpc_message(recv_res.value());
            if (decode_res.is_err()) {
                echo::error("rpc decode failed");
                return dp::result::err(decode_res.error());
            }

            auto [response_id, response_payload] = decode_res.value();

            // Verify request_id matches
            if (response_id != request_id) {
                echo::error("rpc request_id mismatch: expected ", request_id, " got ", response_id);
                return dp::result::err(dp::Error::invalid_argument("request_id mismatch"));
            }

            echo::trace("rpc response id=", response_id, " len=", response_payload.size());
            echo::debug("rpc call completed");

            return dp::result::ok(std::move(response_payload));
        }

        // Server side: loop handling requests
        // Calls handler for each request and sends response
        // Runs until stream disconnects or error
        dp::Res<void> serve(Handler handler) {
            echo::info("rpc serve started");

            while (true) {
                // Receive request
                auto recv_res = stream_.recv();
                if (recv_res.is_err()) {
                    echo::error("rpc serve recv failed");
                    return dp::result::err(recv_res.error());
                }

                // Decode request
                auto decode_res = decode_rpc_message(recv_res.value());
                if (decode_res.is_err()) {
                    echo::error("rpc serve decode failed");
                    return dp::result::err(decode_res.error());
                }

                auto [request_id, request_payload] = decode_res.value();
                echo::trace("rpc serve handling request id=", request_id, " len=", request_payload.size());
                echo::debug("rpc serve handling request id=", request_id);

                // Call handler
                Message response_payload = handler(request_payload);

                // Encode response
                Message rpc_response = encode_rpc_message(request_id, response_payload);

                // Send response
                auto send_res = stream_.send(rpc_response);
                if (send_res.is_err()) {
                    echo::error("rpc serve send failed");
                    return dp::result::err(send_res.error());
                }

                echo::trace("rpc serve sent response id=", request_id, " len=", response_payload.size());
                echo::debug("rpc serve completed request id=", request_id);
            }

            return dp::result::ok();
        }
    };

} // namespace netpipe

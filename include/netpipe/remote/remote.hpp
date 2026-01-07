#pragma once

#include <netpipe/remote/common.hpp>
#include <netpipe/remote/protocol.hpp>
#include <netpipe/stream.hpp>

namespace netpipe {
    namespace remote {

        /// Remote RPC layer on top of Stream
        /// Provides request-response semantics with request ID matching
        /// Single-threaded, synchronous, one outstanding call at a time
        class Remote {
          private:
            Stream &stream_;
            dp::u32 next_request_id_;

          public:
            explicit Remote(Stream &stream) : stream_(stream), next_request_id_(0) {
                echo::trace("Remote constructed");
            }

            /// Client side: send request and wait for response
            /// Blocks until response arrives or timeout
            /// @param request The request message to send
            /// @param timeout_ms Timeout in milliseconds (default 5000ms)
            /// @return Response message or error
            dp::Res<Message> call(const Message &request, dp::u32 timeout_ms = 5000) {
                dp::u32 request_id = next_request_id_++;
                echo::trace("remote call id=", request_id, " len=", request.size());
                echo::debug("remote call timeout_ms=", timeout_ms);

                // Set receive timeout
                auto timeout_res = stream_.set_recv_timeout(timeout_ms);
                if (timeout_res.is_err()) {
                    echo::warn("failed to set recv timeout: ", timeout_res.error().message.c_str());
                    // Continue anyway - some streams may not support timeout
                }

                // Encode request
                Message remote_request = encode_remote_message(request_id, request);

                // Send request
                auto send_res = stream_.send(remote_request);
                if (send_res.is_err()) {
                    echo::error("remote send failed");
                    return dp::result::err(send_res.error());
                }

                // Receive response (with timeout set above)
                auto recv_res = stream_.recv();
                if (recv_res.is_err()) {
                    echo::error("remote recv failed");
                    return dp::result::err(recv_res.error());
                }

                // Decode response
                auto decode_res = decode_remote_message(recv_res.value());
                if (decode_res.is_err()) {
                    echo::error("remote decode failed");
                    return dp::result::err(decode_res.error());
                }

                auto decoded = decode_res.value();

                // Verify request_id matches
                if (decoded.request_id != request_id) {
                    echo::error("remote request_id mismatch: expected ", request_id, " got ", decoded.request_id);
                    return dp::result::err(dp::Error::invalid_argument("request_id mismatch"));
                }

                // Check if response is an error
                if (decoded.is_error) {
                    echo::error("remote call returned error");
                    // Payload contains error message
                    dp::String error_msg(reinterpret_cast<const char *>(decoded.payload.data()),
                                         decoded.payload.size());
                    return dp::result::err(dp::Error::io_error(error_msg.c_str()));
                }

                echo::trace("remote response id=", decoded.request_id, " len=", decoded.payload.size());
                echo::debug("remote call completed");

                return dp::result::ok(std::move(decoded.payload));
            }

            /// Server side: loop handling requests
            /// Calls handler for each request and sends response
            /// Runs until stream disconnects or error
            /// @param handler Function to handle requests
            /// @return Error if stream fails, ok otherwise
            dp::Res<void> serve(Handler handler) {
                echo::info("remote serve started");

                while (true) {
                    // Receive request
                    auto recv_res = stream_.recv();
                    if (recv_res.is_err()) {
                        echo::error("remote serve recv failed");
                        return dp::result::err(recv_res.error());
                    }

                    // Decode request
                    auto decode_res = decode_remote_message(recv_res.value());
                    if (decode_res.is_err()) {
                        echo::error("remote serve decode failed");
                        return dp::result::err(decode_res.error());
                    }

                    auto decoded = decode_res.value();
                    echo::trace("remote serve handling request id=", decoded.request_id,
                                " len=", decoded.payload.size());
                    echo::debug("remote serve handling request id=", decoded.request_id);

                    // Call handler
                    auto handler_res = handler(decoded.payload);

                    Message remote_response;
                    if (handler_res.is_err()) {
                        // Handler returned error - send error response
                        echo::warn("handler returned error: ", handler_res.error().message.c_str());
                        Message error_payload(handler_res.error().message.begin(), handler_res.error().message.end());
                        remote_response = encode_remote_message(decoded.request_id, error_payload, true);
                    } else {
                        // Handler succeeded - send success response
                        remote_response = encode_remote_message(decoded.request_id, handler_res.value(), false);
                    }

                    // Send response
                    auto send_res = stream_.send(remote_response);
                    if (send_res.is_err()) {
                        echo::error("remote serve send failed");
                        return dp::result::err(send_res.error());
                    }

                    echo::trace("remote serve sent response id=", decoded.request_id);
                    echo::debug("remote serve completed request id=", decoded.request_id);
                }

                return dp::result::ok();
            }
        };

    } // namespace remote

    // Backward compatibility: expose Remote in netpipe namespace as well
    using Remote = remote::Remote;

} // namespace netpipe

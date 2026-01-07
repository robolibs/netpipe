#pragma once

#include <netpipe/remote/common.hpp>
#include <netpipe/remote/protocol.hpp>
#include <netpipe/remote/registry.hpp>
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

        /// Remote with method routing support using Protocol V2
        /// Allows registering multiple handlers for different method_ids
        class RemoteRouter {
          private:
            Stream &stream_;
            dp::u32 next_request_id_;
            MethodRegistry registry_;

          public:
            explicit RemoteRouter(Stream &stream) : stream_(stream), next_request_id_(0) {
                echo::trace("RemoteRouter constructed");
            }

            /// Register a handler for a specific method_id
            dp::Res<void> register_method(dp::u32 method_id, Handler handler) {
                return registry_.register_method(method_id, handler);
            }

            /// Unregister a handler
            dp::Res<void> unregister_method(dp::u32 method_id) { return registry_.unregister_method(method_id); }

            /// Set default handler for unknown methods
            void set_default_handler(Handler handler) { registry_.set_default_handler(handler); }

            /// Clear default handler
            void clear_default_handler() { registry_.clear_default_handler(); }

            /// Client side: call a specific method using Protocol V2
            dp::Res<Message> call(dp::u32 method_id, const Message &request, dp::u32 timeout_ms = 5000) {
                dp::u32 request_id = next_request_id_++;
                echo::trace("remote router call id=", request_id, " method=", method_id, " len=", request.size());

                // Set receive timeout
                auto timeout_res = stream_.set_recv_timeout(timeout_ms);
                if (timeout_res.is_err()) {
                    echo::warn("failed to set recv timeout: ", timeout_res.error().message.c_str());
                }

                // Encode request using V2 protocol
                Message remote_request = encode_remote_message_v2(request_id, method_id, request, MessageType::Request);

                // Send request
                auto send_res = stream_.send(remote_request);
                if (send_res.is_err()) {
                    echo::error("remote router send failed");
                    return dp::result::err(send_res.error());
                }

                // Receive response
                auto recv_res = stream_.recv();
                if (recv_res.is_err()) {
                    echo::error("remote router recv failed");
                    return dp::result::err(recv_res.error());
                }

                // Decode response using V2 protocol
                auto decode_res = decode_remote_message_v2(recv_res.value());
                if (decode_res.is_err()) {
                    echo::error("remote router decode failed");
                    return dp::result::err(decode_res.error());
                }

                auto decoded = decode_res.value();

                // Verify request_id matches
                if (decoded.request_id != request_id) {
                    echo::error("remote router request_id mismatch: expected ", request_id, " got ",
                                decoded.request_id);
                    return dp::result::err(dp::Error::invalid_argument("request_id mismatch"));
                }

                // Check if response is an error
                if (decoded.type == MessageType::Error) {
                    echo::error("remote router call returned error");
                    dp::String error_msg(reinterpret_cast<const char *>(decoded.payload.data()),
                                         decoded.payload.size());
                    return dp::result::err(dp::Error::io_error(error_msg.c_str()));
                }

                echo::trace("remote router response id=", decoded.request_id, " len=", decoded.payload.size());

                return dp::result::ok(std::move(decoded.payload));
            }

            /// Server side: serve requests using registered handlers
            dp::Res<void> serve() {
                echo::info("remote router serve started");

                while (true) {
                    // Receive request
                    auto recv_res = stream_.recv();
                    if (recv_res.is_err()) {
                        echo::error("remote router serve recv failed");
                        return dp::result::err(recv_res.error());
                    }

                    // Decode request using V2 protocol
                    auto decode_res = decode_remote_message_v2(recv_res.value());
                    if (decode_res.is_err()) {
                        echo::error("remote router serve decode failed");
                        return dp::result::err(decode_res.error());
                    }

                    auto decoded = decode_res.value();
                    echo::trace("remote router serve handling request id=", decoded.request_id,
                                " method=", decoded.method_id);

                    // Get handler for method_id
                    auto handler_res = registry_.get_handler(decoded.method_id);
                    Message remote_response;

                    if (handler_res.is_err()) {
                        // No handler found - send error response
                        echo::warn("no handler for method_id: ", decoded.method_id);
                        dp::String error_msg = dp::String("No handler for method_id: ") +
                                               dp::String(std::to_string(decoded.method_id).c_str());
                        Message error_payload(error_msg.begin(), error_msg.end());
                        remote_response = encode_remote_message_v2(decoded.request_id, decoded.method_id, error_payload,
                                                                   MessageType::Error);
                    } else {
                        // Call handler
                        auto handler = handler_res.value();
                        auto result = handler(decoded.payload);

                        if (result.is_err()) {
                            // Handler returned error
                            echo::warn("handler returned error: ", result.error().message.c_str());
                            Message error_payload(result.error().message.begin(), result.error().message.end());
                            remote_response = encode_remote_message_v2(decoded.request_id, decoded.method_id,
                                                                       error_payload, MessageType::Error);
                        } else {
                            // Handler succeeded
                            remote_response = encode_remote_message_v2(decoded.request_id, decoded.method_id,
                                                                       result.value(), MessageType::Response);
                        }
                    }

                    // Send response
                    auto send_res = stream_.send(remote_response);
                    if (send_res.is_err()) {
                        echo::error("remote router serve send failed");
                        return dp::result::err(send_res.error());
                    }

                    echo::trace("remote router serve sent response id=", decoded.request_id);
                }

                return dp::result::ok();
            }
        };

    } // namespace remote

    // Backward compatibility: expose Remote in netpipe namespace as well
    using Remote = remote::Remote;
    using RemoteRouter = remote::RemoteRouter;

} // namespace netpipe

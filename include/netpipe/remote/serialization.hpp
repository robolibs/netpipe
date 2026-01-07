#pragma once

#include <netpipe/remote/common.hpp>
#include <type_traits>

namespace netpipe {
    namespace remote {

        /// Serializer interface - convert type T to/from Message
        template <typename T> struct Serializer {
            /// Serialize value to Message
            static Message serialize(const T &value);

            /// Deserialize Message to value
            static dp::Res<T> deserialize(const Message &msg);
        };

        /// Default serializer for trivially copyable types (POD types)
        template <typename T> struct TrivialSerializer {
            static_assert(std::is_trivially_copyable<T>::value, "T must be trivially copyable");

            static Message serialize(const T &value) {
                const dp::u8 *bytes = reinterpret_cast<const dp::u8 *>(&value);
                return Message(bytes, bytes + sizeof(T));
            }

            static dp::Res<T> deserialize(const Message &msg) {
                if (msg.size() != sizeof(T)) {
                    echo::error("deserialize size mismatch: expected ", sizeof(T), " got ", msg.size());
                    return dp::result::err(dp::Error::invalid_argument("size mismatch"));
                }
                T value;
                std::memcpy(&value, msg.data(), sizeof(T));
                return dp::result::ok(value);
            }
        };

        /// Serializer for dp::String
        template <> struct Serializer<dp::String> {
            static Message serialize(const dp::String &value) { return Message(value.begin(), value.end()); }

            static dp::Res<dp::String> deserialize(const Message &msg) {
                return dp::result::ok(dp::String(reinterpret_cast<const char *>(msg.data()), msg.size()));
            }
        };

        /// Serializer for std::string
        template <> struct Serializer<std::string> {
            static Message serialize(const std::string &value) { return Message(value.begin(), value.end()); }

            static dp::Res<std::string> deserialize(const Message &msg) {
                return dp::result::ok(std::string(reinterpret_cast<const char *>(msg.data()), msg.size()));
            }
        };

        /// Serializer for integral types (u8, u16, u32, u64, i8, i16, i32, i64)
        template <> struct Serializer<dp::u8> : TrivialSerializer<dp::u8> {};
        template <> struct Serializer<dp::u16> : TrivialSerializer<dp::u16> {};
        template <> struct Serializer<dp::u32> : TrivialSerializer<dp::u32> {};
        template <> struct Serializer<dp::u64> : TrivialSerializer<dp::u64> {};
        template <> struct Serializer<dp::i8> : TrivialSerializer<dp::i8> {};
        template <> struct Serializer<dp::i16> : TrivialSerializer<dp::i16> {};
        template <> struct Serializer<dp::i32> : TrivialSerializer<dp::i32> {};
        template <> struct Serializer<dp::i64> : TrivialSerializer<dp::i64> {};

        /// Serializer for float and double
        template <> struct Serializer<float> : TrivialSerializer<float> {};
        template <> struct Serializer<double> : TrivialSerializer<double> {};

        /// Serializer for bool
        template <> struct Serializer<bool> {
            static Message serialize(const bool &value) { return Message{value ? dp::u8(1) : dp::u8(0)}; }

            static dp::Res<bool> deserialize(const Message &msg) {
                if (msg.size() != 1) {
                    return dp::result::err(dp::Error::invalid_argument("bool size must be 1"));
                }
                return dp::result::ok(msg[0] != 0);
            }
        };

        /// Type-safe Remote wrapper with serialization
        template <typename RemoteType> class TypedRemote {
          private:
            RemoteType &remote_;

          public:
            explicit TypedRemote(RemoteType &remote) : remote_(remote) {}

            /// Type-safe call with automatic serialization/deserialization
            template <typename Resp, typename Req>
            dp::Res<Resp> call(dp::u32 method_id, const Req &request, dp::u32 timeout_ms = 5000) {
                // Serialize request
                Message req_msg = Serializer<Req>::serialize(request);

                // Call remote
                auto resp_msg_res = remote_.call(method_id, req_msg, timeout_ms);
                if (resp_msg_res.is_err()) {
                    return dp::result::err(resp_msg_res.error());
                }

                // Deserialize response
                return Serializer<Resp>::deserialize(resp_msg_res.value());
            }

            /// Register type-safe handler
            template <typename Req, typename Resp>
            dp::Res<void> register_method(dp::u32 method_id, std::function<dp::Res<Resp>(const Req &)> handler) {
                // Wrap handler with serialization
                auto wrapped_handler = [handler](const Message &msg) -> dp::Res<Message> {
                    // Deserialize request
                    auto req_res = Serializer<Req>::deserialize(msg);
                    if (req_res.is_err()) {
                        return dp::result::err(req_res.error());
                    }

                    // Call handler
                    auto resp_res = handler(req_res.value());
                    if (resp_res.is_err()) {
                        return dp::result::err(resp_res.error());
                    }

                    // Serialize response
                    Message resp_msg = Serializer<Resp>::serialize(resp_res.value());
                    return dp::result::ok(resp_msg);
                };

                return remote_.register_method(method_id, wrapped_handler);
            }

            /// Get underlying remote
            RemoteType &remote() { return remote_; }
        };

        /// Helper to create TypedRemote
        template <typename RemoteType> TypedRemote<RemoteType> make_typed(RemoteType &remote) {
            return TypedRemote<RemoteType>(remote);
        }

    } // namespace remote

    // Expose in netpipe namespace
    template <typename T> using Serializer = remote::Serializer<T>;

    template <typename RemoteType> using TypedRemote = remote::TypedRemote<RemoteType>;

    template <typename RemoteType> TypedRemote<RemoteType> make_typed(RemoteType &remote) {
        return remote::make_typed(remote);
    }

} // namespace netpipe

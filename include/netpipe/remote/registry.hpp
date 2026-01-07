#pragma once

#include <map>
#include <netpipe/remote/common.hpp>

namespace netpipe {
    namespace remote {

        /// Method registry for routing RPC calls to different handlers
        /// Maps method_id to handler functions
        class MethodRegistry {
          private:
            std::map<dp::u32, Handler> handlers_;
            Handler default_handler_;
            bool has_default_;

          public:
            MethodRegistry() : has_default_(false) { echo::trace("MethodRegistry constructed"); }

            /// Register a handler for a specific method_id
            /// @param method_id The method identifier
            /// @param handler The handler function
            /// @return Error if method_id already registered
            dp::Res<void> register_method(dp::u32 method_id, Handler handler) {
                if (handlers_.find(method_id) != handlers_.end()) {
                    echo::error("method_id already registered: ", method_id);
                    return dp::result::err(dp::Error::invalid_argument("method_id already registered"));
                }

                handlers_[method_id] = handler;
                echo::debug("registered method_id: ", method_id);
                return dp::result::ok();
            }

            /// Unregister a handler for a specific method_id
            /// @param method_id The method identifier
            /// @return Error if method_id not found
            dp::Res<void> unregister_method(dp::u32 method_id) {
                auto it = handlers_.find(method_id);
                if (it == handlers_.end()) {
                    echo::error("method_id not found: ", method_id);
                    return dp::result::err(dp::Error::not_found("method_id not found"));
                }

                handlers_.erase(it);
                echo::debug("unregistered method_id: ", method_id);
                return dp::result::ok();
            }

            /// Set a default handler for unknown method_ids
            /// @param handler The default handler function
            void set_default_handler(Handler handler) {
                default_handler_ = handler;
                has_default_ = true;
                echo::debug("set default handler");
            }

            /// Clear the default handler
            void clear_default_handler() {
                has_default_ = false;
                echo::debug("cleared default handler");
            }

            /// Get handler for a method_id
            /// @param method_id The method identifier
            /// @return Handler if found, error otherwise
            dp::Res<Handler> get_handler(dp::u32 method_id) const {
                auto it = handlers_.find(method_id);
                if (it != handlers_.end()) {
                    return dp::result::ok(it->second);
                }

                if (has_default_) {
                    echo::trace("using default handler for method_id: ", method_id);
                    return dp::result::ok(default_handler_);
                }

                echo::error("no handler for method_id: ", method_id);
                return dp::result::err(dp::Error::not_found("no handler for method_id"));
            }

            /// Check if a method_id is registered
            /// @param method_id The method identifier
            /// @return true if registered, false otherwise
            bool has_method(dp::u32 method_id) const { return handlers_.find(method_id) != handlers_.end(); }

            /// Get number of registered methods
            /// @return Number of registered methods (excluding default)
            dp::usize method_count() const { return handlers_.size(); }

            /// Clear all registered methods
            void clear() {
                handlers_.clear();
                echo::debug("cleared all methods");
            }
        };

    } // namespace remote
} // namespace netpipe

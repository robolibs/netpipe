#pragma once

#include <datapod/datapod.hpp>

namespace netpipe {
    namespace remote {

        /// Protocol version constants
        /// These define the wire format versions for Remote RPC communication

        /// Version 1: Original protocol
        /// Format: [request_id:4][is_error:1][length:4][payload:N]
        /// Total header: 9 bytes
        /// Features: Basic request/response, error flag
        constexpr dp::u8 PROTOCOL_VERSION_1 = 1;

        /// Version 2: Enhanced protocol with streaming support
        /// Format: [version:1][type:1][flags:2][request_id:4][method_id:4][length:4][payload:N]
        /// Total header: 16 bytes
        /// Features: Version field, message types, flags, method routing, streaming
        constexpr dp::u8 PROTOCOL_VERSION_2 = 2;

        /// Current protocol version (what we use by default)
        constexpr dp::u8 PROTOCOL_VERSION_CURRENT = PROTOCOL_VERSION_2;

        /// Library version information
        struct Version {
            dp::u8 major;
            dp::u8 minor;
            dp::u8 patch;

            /// Get version as string "major.minor.patch"
            inline dp::String to_string() const {
                return dp::String(std::to_string(major).c_str()) + "." + dp::String(std::to_string(minor).c_str()) +
                       "." + dp::String(std::to_string(patch).c_str());
            }
        };

        /// Current library version
        constexpr Version LIBRARY_VERSION = {0, 1, 0}; // 0.1.0

        /// Get library version as string
        inline dp::String get_version_string() { return LIBRARY_VERSION.to_string(); }

        /// Check if a protocol version is supported
        inline bool is_protocol_supported(dp::u8 version) {
            return version == PROTOCOL_VERSION_1 || version == PROTOCOL_VERSION_2;
        }

        /// Get protocol version name
        inline const char *get_protocol_name(dp::u8 version) {
            switch (version) {
            case PROTOCOL_VERSION_1:
                return "V1 (Basic)";
            case PROTOCOL_VERSION_2:
                return "V2 (Streaming)";
            default:
                return "Unknown";
            }
        }

    } // namespace remote
} // namespace netpipe

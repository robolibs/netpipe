#pragma once

#include <netpipe/common.hpp>

namespace netpipe {

    // TCP endpoint - host and port
    struct TcpEndpoint {
        dp::String host; // IP address or hostname
        dp::u16 port;

        inline dp::String to_string() const { return host + ":" + dp::String(std::to_string(port).c_str()); }
    };

    // UDP endpoint - host and port
    struct UdpEndpoint {
        dp::String host; // IP address or hostname
        dp::u16 port;

        inline dp::String to_string() const { return host + ":" + dp::String(std::to_string(port).c_str()); }
    };

    // IPC endpoint - Unix domain socket path
    struct IpcEndpoint {
        dp::String path; // Filesystem path like /tmp/myapp.sock

        inline dp::String to_string() const { return path; }
    };

    // Shared memory endpoint - name and size
    struct ShmEndpoint {
        dp::String name; // Shared memory region name
        dp::usize size;  // Ring buffer size in bytes

        inline dp::String to_string() const {
            return name + " (size=" + dp::String(std::to_string(size).c_str()) + ")";
        }
    };

    // LoRa endpoint - IPv6 address (melodi uses IPv6 mesh)
    struct LoraEndpoint {
        dp::String ipv6; // IPv6 address like 2001:db8::42
        // No port - LoRa doesn't use ports

        inline dp::String to_string() const { return ipv6; }
    };

} // namespace netpipe

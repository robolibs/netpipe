#pragma once

#include <datapod/datapod.hpp>
#include <echo/echo.hpp>

#include <cerrno>
#include <cstring>
#include <unistd.h>

namespace netpipe {

    // Message type - just a vector of bytes
    using Message = dp::Vector<dp::u8>;

    // Big-endian encoding for length-prefix framing
    inline dp::Array<dp::u8, 4> encode_u32_be(dp::u32 value) {
        echo::trace("encode_u32_be: value=", value);
        dp::Array<dp::u8, 4> bytes;
        bytes[0] = static_cast<dp::u8>((value >> 24) & 0xFF);
        bytes[1] = static_cast<dp::u8>((value >> 16) & 0xFF);
        bytes[2] = static_cast<dp::u8>((value >> 8) & 0xFF);
        bytes[3] = static_cast<dp::u8>(value & 0xFF);
        return bytes;
    }

    // Big-endian decoding for length-prefix framing
    inline dp::u32 decode_u32_be(const dp::u8 *bytes) {
        dp::u32 value = (static_cast<dp::u32>(bytes[0]) << 24) | (static_cast<dp::u32>(bytes[1]) << 16) |
                        (static_cast<dp::u32>(bytes[2]) << 8) | static_cast<dp::u32>(bytes[3]);
        echo::trace("decode_u32_be: value=", value);
        return value;
    }

    // Helper to encode u32 directly into a vector
    inline void append_u32_be(dp::Vector<dp::u8> &buffer, dp::u32 value) {
        auto bytes = encode_u32_be(value);
        buffer.insert(buffer.end(), bytes.begin(), bytes.end());
    }

    // Helper to read exactly n bytes from a file descriptor
    // Returns dp::Res<void> - ok if all bytes read, error otherwise
    inline dp::Res<void> read_exact(dp::i32 fd, dp::u8 *buffer, dp::usize count) {
        dp::usize total_read = 0;
        while (total_read < count) {
            dp::isize n = ::read(fd, buffer + total_read, count - total_read);
            if (n < 0) {
                if (errno == EINTR) {
                    echo::trace("read interrupted by signal, retrying");
                    continue; // Interrupted by signal, retry
                }
                echo::error("read failed: ", strerror(errno));
                return dp::result::err(dp::Error::io_error(dp::String("read failed: ") + strerror(errno)));
            }
            if (n == 0) {
                echo::debug("connection closed by peer");
                return dp::result::err(dp::Error::not_found("connection closed"));
            }
            total_read += static_cast<dp::usize>(n);
            echo::trace("read ", n, " bytes, total=", total_read, "/", count);
        }
        return dp::result::ok();
    }

    // Helper to write exactly n bytes to a file descriptor
    // Returns dp::Res<void> - ok if all bytes written, error otherwise
    inline dp::Res<void> write_exact(dp::i32 fd, const dp::u8 *buffer, dp::usize count) {
        dp::usize total_written = 0;
        while (total_written < count) {
            dp::isize n = ::write(fd, buffer + total_written, count - total_written);
            if (n < 0) {
                if (errno == EINTR) {
                    echo::trace("write interrupted by signal, retrying");
                    continue; // Interrupted by signal, retry
                }
                echo::error("write failed: ", strerror(errno));
                return dp::result::err(dp::Error::io_error(dp::String("write failed: ") + strerror(errno)));
            }
            total_written += static_cast<dp::usize>(n);
            echo::trace("wrote ", n, " bytes, total=", total_written, "/", count);
        }
        return dp::result::ok();
    }

} // namespace netpipe

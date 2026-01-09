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
    // ERROR CATEGORIZATION:
    // - timeout: EAGAIN/EWOULDBLOCK (expected, recoverable)
    // - not_found: connection closed (ECONNRESET, EPIPE, EOF)
    // - io_error: other I/O errors (unexpected, may be recoverable)
    inline dp::Res<void> read_exact(dp::i32 fd, dp::u8 *buffer, dp::usize count) {
        dp::usize total_read = 0;
        while (total_read < count) {
            dp::isize n = ::read(fd, buffer + total_read, count - total_read);
            if (n < 0) {
                // EINTR: Interrupted by signal - retry transparently
                if (errno == EINTR) {
                    echo::trace("read interrupted by signal, retrying");
                    continue;
                }

                // EAGAIN/EWOULDBLOCK: Timeout - expected behavior with SO_RCVTIMEO
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    echo::trace("read timeout (fd=", fd, ", wanted=", count, ", got=", total_read, ")");
                    return dp::result::err(dp::Error::timeout("read timeout"));
                }

                // Connection closed errors - fatal, not recoverable
                if (errno == ECONNRESET) {
                    echo::trace("read failed: connection reset by peer (fd=", fd, ")");
                    return dp::result::err(dp::Error::not_found("connection reset by peer"));
                }
                if (errno == EPIPE) {
                    echo::trace("read failed: broken pipe (fd=", fd, ")");
                    return dp::result::err(dp::Error::not_found("broken pipe"));
                }
                if (errno == EBADF) {
                    echo::trace("read failed: bad file descriptor (fd=", fd, ")");
                    return dp::result::err(dp::Error::not_found("bad file descriptor"));
                }
                if (errno == ENOTCONN) {
                    echo::trace("read failed: socket not connected (fd=", fd, ")");
                    return dp::result::err(dp::Error::not_found("socket not connected"));
                }

                // Other I/O errors - log errno for debugging
                echo::trace("read failed: ", strerror(errno), " (errno=", errno, ", fd=", fd, ")");
                return dp::result::err(dp::Error::io_error(dp::String("read error: ") + strerror(errno)));
            }

            // n == 0: EOF - connection closed gracefully
            if (n == 0) {
                echo::trace("connection closed by peer (fd=", fd, ", wanted=", count, ", got=", total_read, ")");
                return dp::result::err(dp::Error::not_found("connection closed by peer"));
            }

            total_read += static_cast<dp::usize>(n);
            echo::trace("read ", n, " bytes, total=", total_read, "/", count, " (fd=", fd, ")");
        }
        return dp::result::ok();
    }

    // Helper to write exactly n bytes to a file descriptor
    // Returns dp::Res<void> - ok if all bytes written, error otherwise
    // ERROR CATEGORIZATION:
    // - not_found: connection closed (ECONNRESET, EPIPE, EBADF)
    // - io_error: other I/O errors (unexpected, may be recoverable)
    inline dp::Res<void> write_exact(dp::i32 fd, const dp::u8 *buffer, dp::usize count) {
        dp::usize total_written = 0;
        while (total_written < count) {
            dp::isize n = ::write(fd, buffer + total_written, count - total_written);
            if (n < 0) {
                // EINTR: Interrupted by signal - retry transparently
                if (errno == EINTR) {
                    echo::trace("write interrupted by signal, retrying");
                    continue;
                }

                // Connection closed errors - fatal, not recoverable
                if (errno == ECONNRESET) {
                    echo::trace("write failed: connection reset by peer (fd=", fd, ")");
                    return dp::result::err(dp::Error::not_found("connection reset by peer"));
                }
                if (errno == EPIPE) {
                    echo::trace("write failed: broken pipe (fd=", fd, ")");
                    return dp::result::err(dp::Error::not_found("broken pipe"));
                }
                if (errno == EBADF) {
                    echo::trace("write failed: bad file descriptor (fd=", fd, ")");
                    return dp::result::err(dp::Error::not_found("bad file descriptor"));
                }
                if (errno == ENOTCONN) {
                    echo::trace("write failed: socket not connected (fd=", fd, ")");
                    return dp::result::err(dp::Error::not_found("socket not connected"));
                }

                // Buffer full errors - may be recoverable with retry
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    echo::trace("write would block (fd=", fd, ", wanted=", count, ", wrote=", total_written, ")");
                    return dp::result::err(dp::Error::io_error("write would block"));
                }

                // Other I/O errors - log errno for debugging
                echo::trace("write failed: ", strerror(errno), " (errno=", errno, ", fd=", fd, ")");
                return dp::result::err(dp::Error::io_error(dp::String("write error: ") + strerror(errno)));
            }

            total_written += static_cast<dp::usize>(n);
            echo::trace("wrote ", n, " bytes, total=", total_written, "/", count, " (fd=", fd, ")");
        }
        return dp::result::ok();
    }

} // namespace netpipe

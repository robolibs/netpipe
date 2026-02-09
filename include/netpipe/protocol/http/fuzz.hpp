#pragma once

#include <netpipe/protocol/http11.hpp>
#include <netpipe/protocol/http2.hpp>

namespace netpipe::http::fuzz {

    inline dp::Result<void> fuzz_http11_head_parse(const dp::Vector<dp::u8> &input) {
        dp::String text(reinterpret_cast<const char *>(input.data()), input.size());
        auto req = http11::parse_request_head_with_options(
            text, http11::ParseOptions{.strict = false, .allow_obs_fold = true, .accept_lf_line_endings = true});
        if (req.is_ok()) {
            auto encoded = http11::serialize_request_head(req.value());
            if (encoded.is_err()) {
                return dp::result::err(encoded.error());
            }
        }
        return dp::result::ok();
    }

    inline dp::Result<void> fuzz_http2_frame_parse(const dp::Vector<dp::u8> &input) {
        dp::usize offset = 0;
        while (offset + 9 <= input.size()) {
            auto parsed = http2::parse_frame(input.data() + offset, input.size() - offset);
            if (parsed.is_err()) {
                return dp::result::ok();
            }
            if (parsed.value().second == 0) {
                break;
            }
            offset += parsed.value().second;
        }
        return dp::result::ok();
    }

    inline dp::Result<void> fuzz_http2_hpack_decode(const dp::Vector<dp::u8> &input) {
        http2::HpackContext ctx;
        auto decoded = ctx.decode(input);
        if (decoded.is_ok()) {
            auto reencoded = ctx.encode(decoded.value());
            if (reencoded.is_err()) {
                return dp::result::err(reencoded.error());
            }
        }
        return dp::result::ok();
    }

} // namespace netpipe::http::fuzz

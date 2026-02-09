#include <doctest/doctest.h>

#include <netpipe/protocol/http/fuzz.hpp>

TEST_CASE("HTTP fuzz regression corpus: http11 heads") {
    dp::Vector<dp::Vector<dp::u8>> corpus = {
        {},
        {'G', 'E', 'T', ' ', '/', ' ', 'H', 'T', 'T', 'P', '/', '1', '.', '1', '\n', '\n'},
        {'P', 'O', 'S', 'T', ' ', '/', 'x', ' ', 'H', 'T', 'T', 'P', '/', '1', '.', '1', '\r', '\n', 'C',  'o',
         'n', 't', 'e', 'n', 't', '-', 'L', 'e', 'n', 'g', 't', 'h', ':', ' ', '2', 'a', '\r', '\n', '\r', '\n'},
    };

    for (const auto &input : corpus) {
        CHECK(netpipe::http::fuzz::fuzz_http11_head_parse(input).is_ok());
    }
}

TEST_CASE("HTTP fuzz regression corpus: http2 frame parser") {
    dp::Vector<dp::Vector<dp::u8>> corpus = {
        {},
        {0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00}, // empty SETTINGS
        {0xFF, 0xFF, 0xFF, 0x01, 0x04, 0x00, 0x00, 0x00, 0x01}, // huge length truncated
    };

    for (const auto &input : corpus) {
        CHECK(netpipe::http::fuzz::fuzz_http2_frame_parse(input).is_ok());
    }
}

TEST_CASE("HTTP fuzz regression corpus: http2 hpack") {
    dp::Vector<dp::Vector<dp::u8>> corpus = {
        {}, {0x82}, {0x40, 0x01, 'a', 0x01, 'b'}, {0x40, 0x81, 0x61, 0x01, 0x62}, {0xFF, 0xFF, 0xFF, 0xFF},
    };

    for (const auto &input : corpus) {
        CHECK(netpipe::http::fuzz::fuzz_http2_hpack_decode(input).is_ok());
    }
}

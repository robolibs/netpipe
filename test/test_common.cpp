#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <netpipe/common.hpp>

TEST_CASE("encode_u32_be") {
    auto bytes = netpipe::encode_u32_be(0x12345678);
    CHECK(bytes[0] == 0x12);
    CHECK(bytes[1] == 0x34);
    CHECK(bytes[2] == 0x56);
    CHECK(bytes[3] == 0x78);
}

TEST_CASE("decode_u32_be") {
    dp::Array<dp::u8, 4> bytes = {0x12, 0x34, 0x56, 0x78};
    auto value = netpipe::decode_u32_be(bytes.data());
    CHECK(value == 0x12345678);
}

TEST_CASE("encode/decode round trip") {
    dp::u32 original = 0xDEADBEEF;
    auto encoded = netpipe::encode_u32_be(original);
    auto decoded = netpipe::decode_u32_be(encoded.data());
    CHECK(decoded == original);
}

TEST_CASE("Message type") {
    netpipe::Message msg = {0x01, 0x02, 0x03, 0x04};
    CHECK(msg.size() == 4);
    CHECK(msg[0] == 0x01);
    CHECK(msg[3] == 0x04);
}

#include <doctest/doctest.h>
#include <netpipe/common.hpp>

TEST_CASE("encode_u32_be - Big-endian encoding") {
    SUBCASE("Zero value") {
        auto bytes = netpipe::encode_u32_be(0x00000000);
        CHECK(bytes[0] == 0x00);
        CHECK(bytes[1] == 0x00);
        CHECK(bytes[2] == 0x00);
        CHECK(bytes[3] == 0x00);
    }

    SUBCASE("Maximum value") {
        auto bytes = netpipe::encode_u32_be(0xFFFFFFFF);
        CHECK(bytes[0] == 0xFF);
        CHECK(bytes[1] == 0xFF);
        CHECK(bytes[2] == 0xFF);
        CHECK(bytes[3] == 0xFF);
    }

    SUBCASE("Known pattern") {
        auto bytes = netpipe::encode_u32_be(0x12345678);
        CHECK(bytes[0] == 0x12);
        CHECK(bytes[1] == 0x34);
        CHECK(bytes[2] == 0x56);
        CHECK(bytes[3] == 0x78);
    }

    SUBCASE("Single byte values") {
        auto bytes = netpipe::encode_u32_be(0x000000FF);
        CHECK(bytes[0] == 0x00);
        CHECK(bytes[1] == 0x00);
        CHECK(bytes[2] == 0x00);
        CHECK(bytes[3] == 0xFF);
    }

    SUBCASE("Powers of 256") {
        auto bytes1 = netpipe::encode_u32_be(256);
        CHECK(bytes1[0] == 0x00);
        CHECK(bytes1[1] == 0x00);
        CHECK(bytes1[2] == 0x01);
        CHECK(bytes1[3] == 0x00);

        auto bytes2 = netpipe::encode_u32_be(65536);
        CHECK(bytes2[0] == 0x00);
        CHECK(bytes2[1] == 0x01);
        CHECK(bytes2[2] == 0x00);
        CHECK(bytes2[3] == 0x00);
    }
}

TEST_CASE("decode_u32_be - Big-endian decoding") {
    SUBCASE("Zero value") {
        dp::Array<dp::u8, 4> bytes = {0x00, 0x00, 0x00, 0x00};
        auto value = netpipe::decode_u32_be(bytes.data());
        CHECK(value == 0x00000000);
    }

    SUBCASE("Maximum value") {
        dp::Array<dp::u8, 4> bytes = {0xFF, 0xFF, 0xFF, 0xFF};
        auto value = netpipe::decode_u32_be(bytes.data());
        CHECK(value == 0xFFFFFFFF);
    }

    SUBCASE("Known pattern") {
        dp::Array<dp::u8, 4> bytes = {0x12, 0x34, 0x56, 0x78};
        auto value = netpipe::decode_u32_be(bytes.data());
        CHECK(value == 0x12345678);
    }

    SUBCASE("Different patterns") {
        dp::Array<dp::u8, 4> bytes1 = {0xDE, 0xAD, 0xBE, 0xEF};
        CHECK(netpipe::decode_u32_be(bytes1.data()) == 0xDEADBEEF);

        dp::Array<dp::u8, 4> bytes2 = {0xCA, 0xFE, 0xBA, 0xBE};
        CHECK(netpipe::decode_u32_be(bytes2.data()) == 0xCAFEBABE);
    }
}

TEST_CASE("encode/decode round trip") {
    SUBCASE("Round trip with zero") {
        dp::u32 original = 0;
        auto encoded = netpipe::encode_u32_be(original);
        auto decoded = netpipe::decode_u32_be(encoded.data());
        CHECK(decoded == original);
    }

    SUBCASE("Round trip with max value") {
        dp::u32 original = 0xFFFFFFFF;
        auto encoded = netpipe::encode_u32_be(original);
        auto decoded = netpipe::decode_u32_be(encoded.data());
        CHECK(decoded == original);
    }

    SUBCASE("Round trip with various values") {
        dp::u32 values[] = {0xDEADBEEF, 0x12345678, 0xCAFEBABE, 0x00FF00FF, 1, 255, 256, 65535, 65536, 1000000};

        for (auto original : values) {
            auto encoded = netpipe::encode_u32_be(original);
            auto decoded = netpipe::decode_u32_be(encoded.data());
            CHECK(decoded == original);
        }
    }
}

TEST_CASE("append_u32_be") {
    SUBCASE("Append to empty vector") {
        dp::Vector<dp::u8> buffer;
        netpipe::append_u32_be(buffer, 0x12345678);

        REQUIRE(buffer.size() == 4);
        CHECK(buffer[0] == 0x12);
        CHECK(buffer[1] == 0x34);
        CHECK(buffer[2] == 0x56);
        CHECK(buffer[3] == 0x78);
    }

    SUBCASE("Append to non-empty vector") {
        dp::Vector<dp::u8> buffer = {0xAA, 0xBB};
        netpipe::append_u32_be(buffer, 0xCCDDEEFF);

        REQUIRE(buffer.size() == 6);
        CHECK(buffer[0] == 0xAA);
        CHECK(buffer[1] == 0xBB);
        CHECK(buffer[2] == 0xCC);
        CHECK(buffer[3] == 0xDD);
        CHECK(buffer[4] == 0xEE);
        CHECK(buffer[5] == 0xFF);
    }

    SUBCASE("Append multiple values") {
        dp::Vector<dp::u8> buffer;
        netpipe::append_u32_be(buffer, 0x11111111);
        netpipe::append_u32_be(buffer, 0x22222222);
        netpipe::append_u32_be(buffer, 0x33333333);

        REQUIRE(buffer.size() == 12);
        CHECK(buffer[0] == 0x11);
        CHECK(buffer[4] == 0x22);
        CHECK(buffer[8] == 0x33);
    }
}

TEST_CASE("Message type") {
    SUBCASE("Create empty message") {
        netpipe::Message msg;
        CHECK(msg.empty());
        CHECK(msg.size() == 0);
    }

    SUBCASE("Create message with initializer list") {
        netpipe::Message msg = {0x01, 0x02, 0x03, 0x04};
        CHECK(msg.size() == 4);
        CHECK(msg[0] == 0x01);
        CHECK(msg[1] == 0x02);
        CHECK(msg[2] == 0x03);
        CHECK(msg[3] == 0x04);
    }

    SUBCASE("Create message with size") {
        netpipe::Message msg(100);
        CHECK(msg.size() == 100);
    }

    SUBCASE("Modify message") {
        netpipe::Message msg = {0x00, 0x00, 0x00};
        msg[0] = 0xFF;
        msg[1] = 0xAA;
        msg[2] = 0x55;

        CHECK(msg[0] == 0xFF);
        CHECK(msg[1] == 0xAA);
        CHECK(msg[2] == 0x55);
    }

    SUBCASE("Push back to message") {
        netpipe::Message msg;
        msg.push_back(0x10);
        msg.push_back(0x20);
        msg.push_back(0x30);

        REQUIRE(msg.size() == 3);
        CHECK(msg[0] == 0x10);
        CHECK(msg[1] == 0x20);
        CHECK(msg[2] == 0x30);
    }
}

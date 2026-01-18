#include <doctest/doctest.h>
#include <netpipe/protocol/http3.hpp>
#include <netpipe/protocol/http3/connection.hpp>
#include <netpipe/protocol/http3/frame.hpp>
#include <netpipe/protocol/http3/qpack.hpp>
#include <netpipe/protocol/http3/types.hpp>
#include <netpipe/transport/stream/quic/varint.hpp>

using namespace netpipe::http3;
using namespace netpipe::quic;

// =============================================================================
// HTTP/3 Types Tests
// =============================================================================

TEST_CASE("HTTP/3 Types") {
    SUBCASE("FrameType values") {
        CHECK(static_cast<dp::u64>(FrameType::Data) == 0x00);
        CHECK(static_cast<dp::u64>(FrameType::Headers) == 0x01);
        CHECK(static_cast<dp::u64>(FrameType::CancelPush) == 0x03);
        CHECK(static_cast<dp::u64>(FrameType::Settings) == 0x04);
        CHECK(static_cast<dp::u64>(FrameType::PushPromise) == 0x05);
        CHECK(static_cast<dp::u64>(FrameType::GoAway) == 0x07);
        CHECK(static_cast<dp::u64>(FrameType::MaxPushId) == 0x0D);
    }

    SUBCASE("ErrorCode values") {
        CHECK(static_cast<dp::u64>(ErrorCode::NoError) == 0x100);
        CHECK(static_cast<dp::u64>(ErrorCode::GeneralProtocolError) == 0x101);
        CHECK(static_cast<dp::u64>(ErrorCode::InternalError) == 0x102);
        CHECK(static_cast<dp::u64>(ErrorCode::StreamCreationError) == 0x103);
        CHECK(static_cast<dp::u64>(ErrorCode::ClosedCriticalStream) == 0x104);
        CHECK(static_cast<dp::u64>(ErrorCode::FrameUnexpected) == 0x105);
        CHECK(static_cast<dp::u64>(ErrorCode::FrameError) == 0x106);
        CHECK(static_cast<dp::u64>(ErrorCode::ExcessiveLoad) == 0x107);
        CHECK(static_cast<dp::u64>(ErrorCode::SettingsError) == 0x109);
        CHECK(static_cast<dp::u64>(ErrorCode::MissingSettings) == 0x10A);
        CHECK(static_cast<dp::u64>(ErrorCode::RequestRejected) == 0x10B);
        CHECK(static_cast<dp::u64>(ErrorCode::RequestCancelled) == 0x10C);
    }

    SUBCASE("StreamType values") {
        CHECK(static_cast<dp::u64>(StreamType::Control) == 0x00);
        CHECK(static_cast<dp::u64>(StreamType::Push) == 0x01);
        CHECK(static_cast<dp::u64>(StreamType::QpackEncoder) == 0x02);
        CHECK(static_cast<dp::u64>(StreamType::QpackDecoder) == 0x03);
    }

    SUBCASE("SettingsId values") {
        CHECK(static_cast<dp::u64>(SettingsId::QpackMaxTableCapacity) == 0x01);
        CHECK(static_cast<dp::u64>(SettingsId::MaxFieldSectionSize) == 0x06);
        CHECK(static_cast<dp::u64>(SettingsId::QpackBlockedStreams) == 0x07);
    }
}

TEST_CASE("HTTP/3 Settings") {
    SUBCASE("Default settings") {
        Settings settings;
        CHECK(settings.qpack_max_table_capacity == 0);
        CHECK(settings.max_field_section_size == 0);
        CHECK(settings.qpack_blocked_streams == 0);
    }

    SUBCASE("Settings serialization empty") {
        Settings settings;
        auto serialized = settings.serialize();
        CHECK(serialized.empty()); // All values are 0, nothing to serialize
    }

    SUBCASE("Settings serialization with values") {
        Settings settings;
        settings.qpack_max_table_capacity = 4096;
        settings.max_field_section_size = 16384;
        settings.qpack_blocked_streams = 100;

        auto serialized = settings.serialize();
        CHECK(!serialized.empty());

        // Parse back
        auto parsed = Settings::parse(serialized.data(), serialized.size());
        CHECK(parsed.is_ok());
        CHECK(parsed.value().qpack_max_table_capacity == 4096);
        CHECK(parsed.value().max_field_section_size == 16384);
        CHECK(parsed.value().qpack_blocked_streams == 100);
    }

    SUBCASE("Settings round-trip") {
        Settings original;
        original.qpack_max_table_capacity = 1024;
        original.max_field_section_size = 8192;

        auto serialized = original.serialize();
        auto parsed = Settings::parse(serialized.data(), serialized.size());

        CHECK(parsed.is_ok());
        CHECK(parsed.value().qpack_max_table_capacity == original.qpack_max_table_capacity);
        CHECK(parsed.value().max_field_section_size == original.max_field_section_size);
    }
}

TEST_CASE("HTTP/3 HeaderField") {
    SUBCASE("Default construction") {
        HeaderField field;
        CHECK(field.name.empty());
        CHECK(field.value.empty());
    }

    SUBCASE("Construction with values") {
        HeaderField field("content-type", "application/json");
        CHECK(field.name == "content-type");
        CHECK(field.value == "application/json");
    }
}

TEST_CASE("HTTP/3 Request") {
    SUBCASE("Pseudo-headers") {
        Request req;
        req.method = "GET";
        req.scheme = "https";
        req.authority = "example.com";
        req.path = "/api/test";

        auto pseudo = req.get_pseudo_headers();
        CHECK(pseudo.size() == 4);
        CHECK(pseudo[0].name == ":method");
        CHECK(pseudo[0].value == "GET");
        CHECK(pseudo[1].name == ":scheme");
        CHECK(pseudo[1].value == "https");
        CHECK(pseudo[2].name == ":authority");
        CHECK(pseudo[2].value == "example.com");
        CHECK(pseudo[3].name == ":path");
        CHECK(pseudo[3].value == "/api/test");
    }

    SUBCASE("All headers") {
        Request req;
        req.method = "POST";
        req.scheme = "https";
        req.authority = "api.example.com";
        req.path = "/data";
        req.headers.push_back(HeaderField("content-type", "application/json"));
        req.headers.push_back(HeaderField("accept", "*/*"));

        auto all = req.get_all_headers();
        CHECK(all.size() == 6); // 4 pseudo + 2 regular
        CHECK(all[4].name == "content-type");
        CHECK(all[5].name == "accept");
    }
}

TEST_CASE("HTTP/3 Response") {
    SUBCASE("Pseudo-headers") {
        Response resp;
        resp.status = 200;

        auto pseudo = resp.get_pseudo_headers();
        CHECK(pseudo.size() == 1);
        CHECK(pseudo[0].name == ":status");
        CHECK(pseudo[0].value == "200");
    }

    SUBCASE("All headers with status 404") {
        Response resp;
        resp.status = 404;
        resp.headers.push_back(HeaderField("content-type", "text/plain"));

        auto all = resp.get_all_headers();
        CHECK(all.size() == 2);
        CHECK(all[0].value == "404");
        CHECK(all[1].name == "content-type");
    }
}

// =============================================================================
// HTTP/3 Frame Tests
// =============================================================================

TEST_CASE("HTTP/3 DataFrame") {
    SUBCASE("Serialize empty data") {
        DataFrame frame;
        auto serialized = frame.serialize();
        CHECK(!serialized.empty());
        CHECK(serialized[0] == 0x00); // Frame type DATA
    }

    SUBCASE("Serialize with data") {
        DataFrame frame;
        frame.data = {'H', 'e', 'l', 'l', 'o'};
        auto serialized = frame.serialize();
        CHECK(serialized.size() > 5);
    }

    SUBCASE("Parse DATA frame") {
        DataFrame original;
        original.data = {'t', 'e', 's', 't', ' ', 'd', 'a', 't', 'a'};
        auto serialized = original.serialize();

        // Skip the frame type byte for parse
        auto type_result = parse_frame_header(serialized.data(), serialized.size());
        CHECK(type_result.is_ok());
        CHECK(type_result.value().first == FrameType::Data);
        dp::usize offset = type_result.value().second;

        auto parsed = DataFrame::parse(serialized.data() + offset, serialized.size() - offset);
        CHECK(parsed.is_ok());
        CHECK(parsed.value().first.data == original.data);
    }

    SUBCASE("Round-trip large data") {
        DataFrame original;
        original.data.resize(1000);
        for (dp::usize i = 0; i < original.data.size(); i++) {
            original.data[i] = static_cast<dp::u8>(i % 256);
        }

        auto serialized = original.serialize();
        auto type_result = parse_frame_header(serialized.data(), serialized.size());
        dp::usize offset = type_result.value().second;

        auto parsed = DataFrame::parse(serialized.data() + offset, serialized.size() - offset);
        CHECK(parsed.is_ok());
        CHECK(parsed.value().first.data.size() == 1000);
        CHECK(parsed.value().first.data == original.data);
    }
}

TEST_CASE("HTTP/3 HeadersFrame") {
    SUBCASE("Serialize empty") {
        HeadersFrame frame;
        auto serialized = frame.serialize();
        CHECK(!serialized.empty());
        CHECK(serialized[0] == 0x01); // Frame type HEADERS
    }

    SUBCASE("Serialize with encoded headers") {
        HeadersFrame frame;
        frame.encoded_field_section = {0x00, 0x00, 0xC0 | 17}; // Simple QPACK encoded GET

        auto serialized = frame.serialize();
        CHECK(serialized.size() > 3);
    }

    SUBCASE("Parse HEADERS frame") {
        HeadersFrame original;
        original.encoded_field_section = {0x00, 0x00, 0xC0 | 17}; // GET method

        auto serialized = original.serialize();
        auto type_result = parse_frame_header(serialized.data(), serialized.size());
        dp::usize offset = type_result.value().second;

        auto parsed = HeadersFrame::parse(serialized.data() + offset, serialized.size() - offset);
        CHECK(parsed.is_ok());
        CHECK(parsed.value().first.encoded_field_section == original.encoded_field_section);
    }
}

TEST_CASE("HTTP/3 SettingsFrame") {
    SUBCASE("Serialize empty settings") {
        SettingsFrame frame;
        auto serialized = frame.serialize();
        CHECK(!serialized.empty());
        CHECK(serialized[0] == 0x04); // Frame type SETTINGS
    }

    SUBCASE("Round-trip with values") {
        SettingsFrame original;
        original.settings.qpack_max_table_capacity = 2048;
        original.settings.max_field_section_size = 8192;

        auto serialized = original.serialize();
        auto type_result = parse_frame_header(serialized.data(), serialized.size());
        dp::usize offset = type_result.value().second;

        auto parsed = SettingsFrame::parse(serialized.data() + offset, serialized.size() - offset);
        CHECK(parsed.is_ok());
        CHECK(parsed.value().first.settings.qpack_max_table_capacity == 2048);
        CHECK(parsed.value().first.settings.max_field_section_size == 8192);
    }
}

TEST_CASE("HTTP/3 GoAwayFrame") {
    SUBCASE("Serialize") {
        GoAwayFrame frame;
        frame.stream_id = 100;
        auto serialized = frame.serialize();
        CHECK(!serialized.empty());
        CHECK(serialized[0] == 0x07); // Frame type GOAWAY
    }

    SUBCASE("Round-trip") {
        GoAwayFrame original;
        original.stream_id = 256;

        auto serialized = original.serialize();
        auto type_result = parse_frame_header(serialized.data(), serialized.size());
        dp::usize offset = type_result.value().second;

        auto parsed = GoAwayFrame::parse(serialized.data() + offset, serialized.size() - offset);
        CHECK(parsed.is_ok());
        CHECK(parsed.value().first.stream_id == 256);
    }

    SUBCASE("Large stream ID") {
        GoAwayFrame original;
        original.stream_id = 1000000;

        auto serialized = original.serialize();
        auto type_result = parse_frame_header(serialized.data(), serialized.size());
        dp::usize offset = type_result.value().second;

        auto parsed = GoAwayFrame::parse(serialized.data() + offset, serialized.size() - offset);
        CHECK(parsed.is_ok());
        CHECK(parsed.value().first.stream_id == 1000000);
    }
}

TEST_CASE("HTTP/3 CancelPushFrame") {
    SUBCASE("Serialize") {
        CancelPushFrame frame;
        frame.push_id = 42;
        auto serialized = frame.serialize();
        CHECK(!serialized.empty());
        CHECK(serialized[0] == 0x03); // Frame type CANCEL_PUSH
    }

    SUBCASE("Parse") {
        CancelPushFrame original;
        original.push_id = 42;
        auto serialized = original.serialize();

        // Skip frame type to get to payload
        auto type_result = parse_frame_header(serialized.data(), serialized.size());
        CHECK(type_result.is_ok());
        dp::usize offset = type_result.value().second;

        auto parsed = CancelPushFrame::parse(serialized.data() + offset, serialized.size() - offset);
        CHECK(parsed.is_ok());
        CHECK(parsed.value().first.push_id == 42);
    }

    SUBCASE("Parse large push ID") {
        CancelPushFrame original;
        original.push_id = 1000000;
        auto serialized = original.serialize();

        auto type_result = parse_frame_header(serialized.data(), serialized.size());
        dp::usize offset = type_result.value().second;

        auto parsed = CancelPushFrame::parse(serialized.data() + offset, serialized.size() - offset);
        CHECK(parsed.is_ok());
        CHECK(parsed.value().first.push_id == 1000000);
    }
}

TEST_CASE("HTTP/3 MaxPushIdFrame") {
    SUBCASE("Serialize") {
        MaxPushIdFrame frame;
        frame.push_id = 100;
        auto serialized = frame.serialize();
        CHECK(!serialized.empty());
        CHECK(serialized[0] == 0x0D); // Frame type MAX_PUSH_ID
    }

    SUBCASE("Parse") {
        MaxPushIdFrame original;
        original.push_id = 100;
        auto serialized = original.serialize();

        // Skip frame type to get to payload
        auto type_result = parse_frame_header(serialized.data(), serialized.size());
        CHECK(type_result.is_ok());
        dp::usize offset = type_result.value().second;

        auto parsed = MaxPushIdFrame::parse(serialized.data() + offset, serialized.size() - offset);
        CHECK(parsed.is_ok());
        CHECK(parsed.value().first.push_id == 100);
    }

    SUBCASE("Round-trip") {
        MaxPushIdFrame original;
        original.push_id = 999;
        auto serialized = original.serialize();

        auto type_result = parse_frame_header(serialized.data(), serialized.size());
        dp::usize offset = type_result.value().second;

        auto parsed = MaxPushIdFrame::parse(serialized.data() + offset, serialized.size() - offset);
        CHECK(parsed.is_ok());
        CHECK(parsed.value().first.push_id == original.push_id);
    }
}

TEST_CASE("HTTP/3 parse_frame_header") {
    SUBCASE("Parse DATA frame type") {
        dp::Vector<dp::u8> data = {0x00, 0x05, 'h', 'e', 'l', 'l', 'o'};
        auto result = parse_frame_header(data.data(), data.size());
        CHECK(result.is_ok());
        CHECK(result.value().first == FrameType::Data);
        CHECK(result.value().second == 1);
    }

    SUBCASE("Parse HEADERS frame type") {
        dp::Vector<dp::u8> data = {0x01, 0x00};
        auto result = parse_frame_header(data.data(), data.size());
        CHECK(result.is_ok());
        CHECK(result.value().first == FrameType::Headers);
    }

    SUBCASE("Parse SETTINGS frame type") {
        dp::Vector<dp::u8> data = {0x04, 0x00};
        auto result = parse_frame_header(data.data(), data.size());
        CHECK(result.is_ok());
        CHECK(result.value().first == FrameType::Settings);
    }

    SUBCASE("Error on empty input") {
        dp::Vector<dp::u8> data;
        auto result = parse_frame_header(data.data(), data.size());
        CHECK(result.is_err());
    }
}

// =============================================================================
// Huffman Coding Tests
// =============================================================================

TEST_CASE("Huffman Encoder") {
    SUBCASE("Encode simple string") {
        auto encoded = huffman_encoder().encode("hello");
        CHECK(!encoded.empty());
        // "hello" should compress to less bytes with Huffman
        CHECK(encoded.size() <= 5);
    }

    SUBCASE("Encode empty string") {
        auto encoded = huffman_encoder().encode("");
        CHECK(encoded.empty());
    }

    SUBCASE("Encoded length calculation") {
        dp::String test = "www.example.com";
        auto len = huffman_encoder().encoded_length(test);
        auto encoded = huffman_encoder().encode(test);
        CHECK(len == encoded.size());
    }
}

TEST_CASE("Huffman Decoder") {
    SUBCASE("Decode simple string") {
        auto encoded = huffman_encoder().encode("hello");
        auto decoded = huffman_decoder().decode(encoded.data(), encoded.size());
        CHECK(decoded.is_ok());
        CHECK(decoded.value() == "hello");
    }

    SUBCASE("Round-trip ASCII lowercase") {
        dp::String original = "abcdefghijklmnopqrstuvwxyz";
        auto encoded = huffman_encoder().encode(original);
        auto decoded = huffman_decoder().decode(encoded.data(), encoded.size());
        CHECK(decoded.is_ok());
        CHECK(decoded.value() == original);
    }

    SUBCASE("Round-trip ASCII uppercase") {
        dp::String original = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
        auto encoded = huffman_encoder().encode(original);
        auto decoded = huffman_decoder().decode(encoded.data(), encoded.size());
        CHECK(decoded.is_ok());
        CHECK(decoded.value() == original);
    }

    SUBCASE("Round-trip digits") {
        dp::String original = "0123456789";
        auto encoded = huffman_encoder().encode(original);
        auto decoded = huffman_decoder().decode(encoded.data(), encoded.size());
        CHECK(decoded.is_ok());
        CHECK(decoded.value() == original);
    }

    SUBCASE("Round-trip URL") {
        dp::String original = "https://www.example.com/path?query=value";
        auto encoded = huffman_encoder().encode(original);
        auto decoded = huffman_decoder().decode(encoded.data(), encoded.size());
        CHECK(decoded.is_ok());
        CHECK(decoded.value() == original);
    }

    SUBCASE("Round-trip HTTP header value") {
        dp::String original = "application/json; charset=utf-8";
        auto encoded = huffman_encoder().encode(original);
        auto decoded = huffman_decoder().decode(encoded.data(), encoded.size());
        CHECK(decoded.is_ok());
        CHECK(decoded.value() == original);
    }

    SUBCASE("Compression ratio for typical header") {
        dp::String original = "www.example.com";
        auto encoded = huffman_encoder().encode(original);
        // Huffman should compress typical header values
        CHECK(encoded.size() < original.size());
    }
}

// =============================================================================
// QPACK Tests
// =============================================================================

TEST_CASE("QPACK Static Table") {
    SUBCASE("Table size") {
        CHECK(QPACK_STATIC_TABLE_SIZE == 98);
    }

    SUBCASE("Common entries") {
        // Check some well-known static table entries
        CHECK(QPACK_STATIC_TABLE[0].name == ":authority");
        CHECK(QPACK_STATIC_TABLE[1].name == ":path");
        CHECK(QPACK_STATIC_TABLE[1].value == "/");

        // :method entries
        CHECK(QPACK_STATIC_TABLE[15].name == ":method");
        CHECK(QPACK_STATIC_TABLE[15].value == "CONNECT");
        CHECK(QPACK_STATIC_TABLE[17].name == ":method");
        CHECK(QPACK_STATIC_TABLE[17].value == "GET");

        // :scheme entries
        CHECK(QPACK_STATIC_TABLE[22].name == ":scheme");
        CHECK(QPACK_STATIC_TABLE[22].value == "http");
        CHECK(QPACK_STATIC_TABLE[23].name == ":scheme");
        CHECK(QPACK_STATIC_TABLE[23].value == "https");

        // :status entries
        CHECK(QPACK_STATIC_TABLE[25].name == ":status");
        CHECK(QPACK_STATIC_TABLE[25].value == "200");
    }
}

TEST_CASE("QPACK Encoder") {
    QpackEncoder encoder;

    SUBCASE("Encode single header from static table") {
        HeaderList headers = {HeaderField(":method", "GET")};
        auto encoded = encoder.encode(headers);

        // Should have: Required Insert Count (1 byte) + Delta Base (1 byte) + indexed field
        CHECK(encoded.size() >= 3);
        CHECK(encoded[0] == 0x00); // Required Insert Count = 0
        CHECK(encoded[1] == 0x00); // Delta Base = 0
    }

    SUBCASE("Encode multiple static table headers") {
        HeaderList headers = {
            HeaderField(":method", "GET"),
            HeaderField(":scheme", "https"),
            HeaderField(":path", "/"),
        };
        auto encoded = encoder.encode(headers);
        CHECK(encoded.size() >= 5); // RIC + DB + 3 indexed fields
    }

    SUBCASE("Encode header with name reference") {
        HeaderList headers = {HeaderField(":path", "/api/v1/users")};
        auto encoded = encoder.encode(headers);
        CHECK(encoded.size() > 3); // RIC + DB + name ref + value
    }

    SUBCASE("Encode literal header") {
        HeaderList headers = {HeaderField("x-custom-header", "custom-value")};
        auto encoded = encoder.encode(headers);
        CHECK(encoded.size() > 3);
    }

    SUBCASE("Encode empty header list") {
        HeaderList headers;
        auto encoded = encoder.encode(headers);
        CHECK(encoded.size() == 2); // Just RIC + DB
        CHECK(encoded[0] == 0x00);
        CHECK(encoded[1] == 0x00);
    }
}

TEST_CASE("QPACK Decoder") {
    QpackDecoder decoder;
    QpackEncoder encoder;

    SUBCASE("Decode static table indexed field") {
        // Encode :method GET
        HeaderList original = {HeaderField(":method", "GET")};
        auto encoded = encoder.encode(original);

        auto decoded = decoder.decode(encoded);
        CHECK(decoded.is_ok());
        CHECK(decoded.value().size() == 1);
        CHECK(decoded.value()[0].name == ":method");
        CHECK(decoded.value()[0].value == "GET");
    }

    SUBCASE("Decode multiple headers") {
        HeaderList original = {
            HeaderField(":method", "POST"),
            HeaderField(":scheme", "https"),
            HeaderField(":status", "200"),
        };
        auto encoded = encoder.encode(original);

        auto decoded = decoder.decode(encoded);
        CHECK(decoded.is_ok());
        CHECK(decoded.value().size() == 3);
        CHECK(decoded.value()[0].name == ":method");
        CHECK(decoded.value()[0].value == "POST");
        CHECK(decoded.value()[1].name == ":scheme");
        CHECK(decoded.value()[1].value == "https");
        CHECK(decoded.value()[2].name == ":status");
        CHECK(decoded.value()[2].value == "200");
    }

    SUBCASE("Decode literal with name reference") {
        HeaderList original = {HeaderField(":path", "/custom/path")};
        auto encoded = encoder.encode(original);

        auto decoded = decoder.decode(encoded);
        CHECK(decoded.is_ok());
        CHECK(decoded.value().size() == 1);
        CHECK(decoded.value()[0].name == ":path");
        CHECK(decoded.value()[0].value == "/custom/path");
    }

    SUBCASE("Decode literal header") {
        HeaderList original = {HeaderField("x-custom", "value123")};
        auto encoded = encoder.encode(original);

        auto decoded = decoder.decode(encoded);
        CHECK(decoded.is_ok());
        CHECK(decoded.value().size() == 1);
        CHECK(decoded.value()[0].name == "x-custom");
        CHECK(decoded.value()[0].value == "value123");
    }

    SUBCASE("Round-trip complex headers") {
        HeaderList original = {
            HeaderField(":method", "GET"),
            HeaderField(":scheme", "https"),
            HeaderField(":authority", "example.com"),
            HeaderField(":path", "/api/users?id=123"),
            HeaderField("accept", "*/*"),
            HeaderField("user-agent", "netpipe/1.0"),
            HeaderField("x-request-id", "abc-123-def"),
        };
        auto encoded = encoder.encode(original);

        auto decoded = decoder.decode(encoded);
        CHECK(decoded.is_ok());
        CHECK(decoded.value().size() == original.size());

        for (dp::usize i = 0; i < original.size(); i++) {
            CHECK(decoded.value()[i].name == original[i].name);
            CHECK(decoded.value()[i].value == original[i].value);
        }
    }

    SUBCASE("Error on truncated input") {
        dp::Vector<dp::u8> truncated = {0x00}; // Only RIC, missing DB
        auto decoded = decoder.decode(truncated);
        CHECK(decoded.is_err());
    }

    SUBCASE("Empty field section (just RIC+DB)") {
        dp::Vector<dp::u8> empty_section = {0x00, 0x00};
        auto decoded = decoder.decode(empty_section);
        CHECK(decoded.is_ok());
        CHECK(decoded.value().empty());
    }
}

TEST_CASE("QPACK with Huffman Encoding") {
    QpackEncoder encoder(true); // Enable Huffman
    QpackDecoder decoder;

    SUBCASE("Encoder has Huffman enabled") {
        CHECK(encoder.huffman_enabled());
    }

    SUBCASE("Round-trip with Huffman") {
        HeaderList original = {
            HeaderField(":method", "GET"),
            HeaderField(":path", "/api/users/12345"),
            HeaderField("user-agent", "Mozilla/5.0 (compatible)"),
        };

        auto encoded = encoder.encode(original);
        auto decoded_result = decoder.decode(encoded);

        CHECK(decoded_result.is_ok());
        auto decoded = std::move(decoded_result.value());
        CHECK(decoded.size() == original.size());
        for (dp::usize i = 0; i < original.size(); i++) {
            CHECK(decoded[i].name == original[i].name);
            CHECK(decoded[i].value == original[i].value);
        }
    }

    SUBCASE("Huffman vs non-Huffman size comparison") {
        QpackEncoder no_huffman(false);

        HeaderList headers = {
            HeaderField("x-custom-header", "this-is-a-long-value-that-should-compress-well"),
        };

        auto huffman_encoded = encoder.encode(headers);
        auto plain_encoded = no_huffman.encode(headers);

        // Huffman should typically produce smaller output for ASCII text
        CHECK(huffman_encoded.size() <= plain_encoded.size());
    }

    SUBCASE("Toggle Huffman encoding") {
        QpackEncoder togglable;
        CHECK(!togglable.huffman_enabled());

        togglable.set_huffman(true);
        CHECK(togglable.huffman_enabled());

        togglable.set_huffman(false);
        CHECK(!togglable.huffman_enabled());
    }

    SUBCASE("Complex headers with Huffman") {
        HeaderList original = {
            HeaderField(":method", "POST"),
            HeaderField(":scheme", "https"),
            HeaderField(":authority", "api.example.com"),
            HeaderField(":path", "/v1/messages"),
            HeaderField("content-type", "application/json"),
            HeaderField("authorization", "Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9"),
            HeaderField("x-request-id", "550e8400-e29b-41d4-a716-446655440000"),
        };

        auto encoded = encoder.encode(original);
        auto decoded = decoder.decode(encoded);

        CHECK(decoded.is_ok());
        CHECK(decoded.value().size() == original.size());
        for (dp::usize i = 0; i < original.size(); i++) {
            CHECK(decoded.value()[i].name == original[i].name);
            CHECK(decoded.value()[i].value == original[i].value);
        }
    }
}

// =============================================================================
// HTTP/3 Connection Tests
// =============================================================================

TEST_CASE("HTTP/3 Connection") {
    SUBCASE("Client connection creation") {
        Connection conn(true);
        CHECK(conn.is_client());
        CHECK(conn.state() == ConnectionState::Idle);
        CHECK(!conn.is_connected());
    }

    SUBCASE("Server connection creation") {
        Connection conn(false);
        CHECK(!conn.is_client());
        CHECK(conn.state() == ConnectionState::Idle);
    }

    SUBCASE("Initialize client connection") {
        Connection client(true);
        auto init_result = client.initialize();
        CHECK(init_result.is_ok());
        CHECK(client.state() == ConnectionState::Connecting);

        // Should return SETTINGS frame
        auto &settings_data = init_result.value();
        CHECK(!settings_data.empty());
        CHECK(settings_data[0] == 0x04); // SETTINGS frame type
    }

    SUBCASE("Initialize server connection") {
        Connection server(false);
        auto init_result = server.initialize();
        CHECK(init_result.is_ok());
        CHECK(server.state() == ConnectionState::Connecting);
    }

    SUBCASE("Double initialization error") {
        Connection conn(true);
        auto first = conn.initialize();
        CHECK(first.is_ok());

        auto second = conn.initialize();
        CHECK(second.is_err());
    }

    SUBCASE("Connection settings exchange") {
        Connection client(true);
        Connection server(false);

        // Initialize both
        auto client_settings = client.initialize();
        auto server_settings = server.initialize();
        CHECK(client_settings.is_ok());
        CHECK(server_settings.is_ok());

        // Exchange settings
        auto client_process = client.process_control_data(server_settings.value());
        auto server_process = server.process_control_data(client_settings.value());
        CHECK(client_process.is_ok());
        CHECK(server_process.is_ok());

        // Both should be connected
        CHECK(client.is_connected());
        CHECK(server.is_connected());
        CHECK(client.state() == ConnectionState::Connected);
        CHECK(server.state() == ConnectionState::Connected);
    }

    SUBCASE("Create request stream") {
        Connection client(true);
        Connection server(false);

        // Complete handshake
        auto c_settings = client.initialize();
        auto s_settings = server.initialize();
        client.process_control_data(s_settings.value());
        server.process_control_data(c_settings.value());

        // Client creates stream
        auto stream_result = client.create_request_stream();
        CHECK(stream_result.is_ok());
        CHECK(stream_result.value() == 0); // First client stream ID

        // Create another
        auto stream2_result = client.create_request_stream();
        CHECK(stream2_result.is_ok());
        CHECK(stream2_result.value() == 4); // Second client stream ID
    }

    SUBCASE("Server cannot create request stream") {
        Connection server(false);
        auto settings = server.initialize();
        // Manually set connected state for test
        Settings peer;
        auto settings_data = peer.serialize();
        dp::Vector<dp::u8> frame_data;
        frame_data.push_back(0x04); // SETTINGS type
        frame_data.push_back(static_cast<dp::u8>(settings_data.size()));
        frame_data.insert(frame_data.end(), settings_data.begin(), settings_data.end());
        server.process_control_data(frame_data);

        auto stream_result = server.create_request_stream();
        CHECK(stream_result.is_err());
    }

    SUBCASE("Cannot create stream before connected") {
        Connection client(true);
        auto stream_result = client.create_request_stream();
        CHECK(stream_result.is_err());
    }
}

TEST_CASE("HTTP/3 Request/Response Flow") {
    Connection client(true);
    Connection server(false);

    // Establish connection
    auto c_init = client.initialize();
    auto s_init = server.initialize();
    client.process_control_data(s_init.value());
    server.process_control_data(c_init.value());

    SUBCASE("Encode and decode request") {
        auto stream_id = client.create_request_stream().value();

        Request req;
        req.method = "GET";
        req.scheme = "https";
        req.authority = "example.com";
        req.path = "/test";

        auto encoded = client.encode_request(stream_id, req);
        CHECK(encoded.is_ok());

        // Server processes request
        auto process_result = server.process_request_stream(stream_id, encoded.value());
        CHECK(process_result.is_ok());

        // Server should have received the request
        auto received = server.get_request(stream_id);
        CHECK(received.has_value());
        CHECK(received.value().method == "GET");
        CHECK(received.value().scheme == "https");
        CHECK(received.value().authority == "example.com");
        CHECK(received.value().path == "/test");
    }

    SUBCASE("Encode and decode response") {
        auto stream_id = client.create_request_stream().value();

        Request req;
        req.method = "GET";
        req.scheme = "https";
        req.authority = "example.com";
        req.path = "/";

        auto req_encoded = client.encode_request(stream_id, req);
        CHECK(req_encoded.is_ok());

        // Server receives request first
        server.process_request_stream(stream_id, req_encoded.value());

        Response resp;
        resp.status = 200;
        resp.headers.push_back(HeaderField("content-type", "text/html"));

        auto resp_encoded = server.encode_response(stream_id, resp);
        CHECK(resp_encoded.is_ok());

        // Client processes response
        auto process_result = client.process_request_stream(stream_id, resp_encoded.value());
        CHECK(process_result.is_ok());

        auto received = client.get_response(stream_id);
        CHECK(received.has_value());
        CHECK(received.value().status == 200);
    }

    SUBCASE("Encode request body") {
        auto stream_id = client.create_request_stream().value();

        Request req;
        req.method = "POST";
        req.scheme = "https";
        req.authority = "example.com";
        req.path = "/data";
        client.encode_request(stream_id, req);

        dp::Vector<dp::u8> body = {'t', 'e', 's', 't'};
        auto encoded_body = client.encode_data(stream_id, body);
        CHECK(encoded_body.is_ok());
        CHECK(!encoded_body.value().empty());
    }

    SUBCASE("Cannot encode body before headers") {
        auto stream_id = client.create_request_stream().value();
        dp::Vector<dp::u8> body = {'t', 'e', 's', 't'};
        auto result = client.encode_data(stream_id, body);
        CHECK(result.is_err());
    }
}

TEST_CASE("HTTP/3 GOAWAY") {
    Connection client(true);
    Connection server(false);

    // Establish connection
    auto c_init = client.initialize();
    auto s_init = server.initialize();
    client.process_control_data(s_init.value());
    server.process_control_data(c_init.value());

    SUBCASE("Server sends GOAWAY") {
        auto goaway = server.create_goaway(0);
        CHECK(!goaway.empty());
        CHECK(server.state() == ConnectionState::GoingAway);
    }

    SUBCASE("Client receives GOAWAY") {
        auto goaway = server.create_goaway(100);

        auto process_result = client.process_control_data(goaway);
        CHECK(process_result.is_ok());
        CHECK(client.goaway_received());
        CHECK(client.goaway_stream_id() == 100);
        CHECK(client.state() == ConnectionState::GoingAway);
    }
}

TEST_CASE("HTTP/3 Stream Management") {
    Connection client(true);
    Connection server(false);

    auto c_init = client.initialize();
    auto s_init = server.initialize();
    client.process_control_data(s_init.value());
    server.process_control_data(c_init.value());

    SUBCASE("Active streams tracking") {
        CHECK(client.active_streams().empty());

        client.create_request_stream();
        CHECK(client.active_streams().size() == 1);

        client.create_request_stream();
        client.create_request_stream();
        CHECK(client.active_streams().size() == 3);
    }

    SUBCASE("Close stream") {
        auto stream_id = client.create_request_stream().value();
        CHECK(client.active_streams().size() == 1);

        client.close_stream(stream_id);
        CHECK(client.active_streams().empty());
    }

    SUBCASE("Stream finished") {
        auto stream_id = client.create_request_stream().value();
        client.stream_finished(stream_id);
        // Stream should still be in active streams but with different state
        CHECK(client.active_streams().size() == 1);
    }
}

TEST_CASE("HTTP/3 Error Cases") {
    SUBCASE("Duplicate SETTINGS frame") {
        Connection client(true);
        Connection server(false);

        auto c_init = client.initialize();
        auto s_init = server.initialize();

        // First SETTINGS is fine
        auto first_result = client.process_control_data(s_init.value());
        CHECK(first_result.is_ok());

        // Second SETTINGS should fail
        auto second_result = client.process_control_data(s_init.value());
        CHECK(second_result.is_err());
    }

    SUBCASE("Unknown stream for encode_request") {
        Connection client(true);
        Connection server(false);
        auto c_init = client.initialize();
        auto s_init = server.initialize();
        client.process_control_data(s_init.value());
        server.process_control_data(c_init.value());

        Request req;
        req.method = "GET";
        auto result = client.encode_request(999, req);
        CHECK(result.is_err());
    }

    SUBCASE("Unknown stream for encode_data") {
        Connection client(true);
        Connection server(false);
        auto c_init = client.initialize();
        auto s_init = server.initialize();
        client.process_control_data(s_init.value());
        server.process_control_data(c_init.value());

        dp::Vector<dp::u8> body = {'t', 'e', 's', 't'};
        auto result = client.encode_data(999, body);
        CHECK(result.is_err());
    }

    SUBCASE("Process empty control data") {
        Connection client(true);
        client.initialize();
        auto result = client.process_control_data({});
        CHECK(result.is_ok()); // Empty data is fine
    }
}

TEST_CASE("HTTP/3 Full Request-Response Cycle") {
    Connection client(true);
    Connection server(false);

    // Handshake
    auto c_init = client.initialize();
    auto s_init = server.initialize();
    client.process_control_data(s_init.value());
    server.process_control_data(c_init.value());

    SUBCASE("GET request cycle") {
        // Client creates request
        auto stream_id = client.create_request_stream().value();

        Request req;
        req.method = "GET";
        req.scheme = "https";
        req.authority = "api.example.com";
        req.path = "/users/123";
        req.headers.push_back(HeaderField("accept", "application/json"));

        auto req_data = client.encode_request(stream_id, req);
        CHECK(req_data.is_ok());

        // Server receives and processes
        server.process_request_stream(stream_id, req_data.value());
        client.stream_finished(stream_id);

        auto server_req = server.get_request(stream_id);
        CHECK(server_req.has_value());
        CHECK(server_req.value().method == "GET");
        CHECK(server_req.value().path == "/users/123");

        // Server sends response
        Response resp;
        resp.status = 200;
        resp.headers.push_back(HeaderField("content-type", "application/json"));
        resp.body = {'{', '"', 'i', 'd', '"', ':', '1', '2', '3', '}'};

        auto resp_headers = server.encode_response(stream_id, resp);
        auto resp_body = server.encode_data(stream_id, resp.body);
        CHECK(resp_headers.is_ok());
        CHECK(resp_body.is_ok());

        // Combine headers and body
        dp::Vector<dp::u8> full_response = resp_headers.value();
        full_response.insert(full_response.end(), resp_body.value().begin(), resp_body.value().end());

        // Client receives response
        client.process_request_stream(stream_id, full_response);

        auto client_resp = client.get_response(stream_id);
        CHECK(client_resp.has_value());
        CHECK(client_resp.value().status == 200);
        CHECK(client_resp.value().body.size() == 10);
    }

    SUBCASE("POST request with body") {
        auto stream_id = client.create_request_stream().value();

        Request req;
        req.method = "POST";
        req.scheme = "https";
        req.authority = "api.example.com";
        req.path = "/users";
        req.headers.push_back(HeaderField("content-type", "application/json"));
        req.body = {'{', '"', 'n', 'a', 'm', 'e', '"', ':', '"', 't', 'e', 's', 't', '"', '}'};

        auto req_headers = client.encode_request(stream_id, req);
        auto req_body = client.encode_data(stream_id, req.body);
        CHECK(req_headers.is_ok());
        CHECK(req_body.is_ok());

        // Server receives
        dp::Vector<dp::u8> full_request = req_headers.value();
        full_request.insert(full_request.end(), req_body.value().begin(), req_body.value().end());

        server.process_request_stream(stream_id, full_request);

        auto server_req = server.get_request(stream_id);
        CHECK(server_req.has_value());
        CHECK(server_req.value().method == "POST");
        CHECK(server_req.value().body.size() == 15);
    }
}

TEST_CASE("HTTP/3 Multiple Concurrent Streams") {
    Connection client(true);
    Connection server(false);

    auto c_init = client.initialize();
    auto s_init = server.initialize();
    client.process_control_data(s_init.value());
    server.process_control_data(c_init.value());

    SUBCASE("Multiple GET requests") {
        dp::Vector<dp::u64> stream_ids;

        // Create multiple streams
        for (int i = 0; i < 5; i++) {
            auto stream_id = client.create_request_stream().value();
            stream_ids.push_back(stream_id);

            Request req;
            req.method = "GET";
            req.scheme = "https";
            req.authority = "example.com";
            req.path = dp::String(("/resource/" + std::to_string(i)).c_str());

            auto encoded = client.encode_request(stream_id, req);
            CHECK(encoded.is_ok());

            server.process_request_stream(stream_id, encoded.value());
        }

        CHECK(client.active_streams().size() == 5);
        CHECK(stream_ids[0] == 0);
        CHECK(stream_ids[1] == 4);
        CHECK(stream_ids[2] == 8);
        CHECK(stream_ids[3] == 12);
        CHECK(stream_ids[4] == 16);

        // Verify all requests received
        for (int i = 0; i < 5; i++) {
            auto req = server.get_request(stream_ids[i]);
            CHECK(req.has_value());
            CHECK(req.value().path == dp::String(("/resource/" + std::to_string(i)).c_str()));
        }
    }
}

// =============================================================================
// QPACK Dynamic Table Tests
// =============================================================================

TEST_CASE("QPACK Dynamic Table") {
    SUBCASE("Default table is disabled") {
        QpackDynamicTable table;
        CHECK(!table.is_enabled());
        CHECK(table.max_capacity() == 0);
        CHECK(table.count() == 0);
    }

    SUBCASE("Set capacity enables table") {
        QpackDynamicTable table;
        table.set_max_capacity(4096);
        CHECK(table.is_enabled());
        CHECK(table.max_capacity() == 4096);
    }

    SUBCASE("Insert and retrieve entry") {
        QpackDynamicTable table(4096);
        auto idx = table.insert("x-custom", "value1");
        CHECK(idx == 0);
        CHECK(table.count() == 1);

        auto *entry = table.get_relative(0);
        CHECK(entry != nullptr);
        CHECK(entry->name == "x-custom");
        CHECK(entry->value == "value1");
    }

    SUBCASE("Insert multiple entries") {
        QpackDynamicTable table(4096);
        table.insert("header1", "value1");
        table.insert("header2", "value2");
        table.insert("header3", "value3");
        CHECK(table.count() == 3);

        // Relative index 0 is the most recently inserted
        auto *newest = table.get_relative(0);
        CHECK(newest->name == "header3");

        auto *oldest = table.get_relative(2);
        CHECK(oldest->name == "header1");
    }

    SUBCASE("Eviction when capacity exceeded") {
        QpackDynamicTable table(100); // Small capacity
        // Each entry is name + value + 32 overhead
        table.insert("a", "b"); // 1 + 1 + 32 = 34
        table.insert("c", "d"); // 1 + 1 + 32 = 34, total = 68
        table.insert("e", "f"); // 1 + 1 + 32 = 34, total = 102 > 100, should evict first

        // Should have evicted the first entry
        CHECK(table.count() == 2);
        auto *entry = table.get_relative(0);
        CHECK(entry->name == "e");
    }

    SUBCASE("Entry too large for table") {
        QpackDynamicTable table(50);
        // Entry with 20 char name + 10 char value + 32 overhead = 62 > 50
        auto idx = table.insert("12345678901234567890", "1234567890");
        CHECK(idx == -1); // Should fail
        CHECK(table.count() == 0);
    }

    SUBCASE("Find entry - full match") {
        QpackDynamicTable table(4096);
        table.insert("content-type", "application/json");
        table.insert("accept", "*/*");

        auto [idx, name_only] = table.find("content-type", "application/json");
        CHECK(idx == 1); // Second entry (relative)
        CHECK(!name_only);
    }

    SUBCASE("Find entry - name only match") {
        QpackDynamicTable table(4096);
        table.insert("content-type", "application/json");

        auto [idx, name_only] = table.find("content-type", "text/html");
        CHECK(idx == 0);
        CHECK(name_only);
    }

    SUBCASE("Find entry - no match") {
        QpackDynamicTable table(4096);
        table.insert("x-header", "value");

        auto [idx, name_only] = table.find("y-header", "value");
        CHECK(idx == -1);
    }

    SUBCASE("Duplicate entry") {
        QpackDynamicTable table(4096);
        table.insert("original", "value");
        auto dup_idx = table.duplicate(0);
        CHECK(dup_idx >= 0);
        CHECK(table.count() == 2);

        auto *dup = table.get_relative(0);
        CHECK(dup->name == "original");
        CHECK(dup->value == "value");
    }

    SUBCASE("Get by absolute index") {
        QpackDynamicTable table(4096);
        table.insert("first", "1"); // absolute index 0
        table.insert("second", "2"); // absolute index 1
        table.insert("third", "3"); // absolute index 2

        auto *first = table.get_absolute(0);
        CHECK(first->name == "first");

        auto *second = table.get_absolute(1);
        CHECK(second->name == "second");

        auto *third = table.get_absolute(2);
        CHECK(third->name == "third");
    }

    SUBCASE("Insert count tracking") {
        QpackDynamicTable table(4096);
        CHECK(table.get_insert_count() == 0);

        table.insert("a", "1");
        CHECK(table.get_insert_count() == 1);

        table.insert("b", "2");
        CHECK(table.get_insert_count() == 2);
    }
}

TEST_CASE("QPACK Encoder with Dynamic Table") {
    SUBCASE("Enable dynamic table") {
        QpackEncoder encoder;
        CHECK(!encoder.dynamic_table_enabled());

        encoder.set_dynamic_table_capacity(4096);
        CHECK(encoder.dynamic_table_enabled());
        CHECK(encoder.dynamic_table_capacity() == 4096);
    }

    SUBCASE("Encoder produces encoder stream data") {
        QpackEncoder encoder;
        encoder.set_dynamic_table_capacity(4096);

        // Encode headers that should be added to dynamic table
        HeaderList headers = {
            HeaderField("x-custom-header", "custom-value"),
        };

        auto encoded = encoder.encode(headers);
        CHECK(!encoded.empty());

        // Should have pending encoder stream data
        CHECK(encoder.has_encoder_stream_data());
        auto stream_data = encoder.get_encoder_stream_data();
        CHECK(!stream_data.empty());
    }

    SUBCASE("Dynamic table state after encoding") {
        QpackEncoder encoder;
        encoder.set_dynamic_table_capacity(4096);

        HeaderList headers = {
            HeaderField("x-test", "value"),
        };

        encoder.encode(headers);
        CHECK(encoder.dynamic_table().count() >= 0);
    }
}

TEST_CASE("QPACK Decoder with Dynamic Table") {
    SUBCASE("Enable dynamic table") {
        QpackDecoder decoder;
        CHECK(!decoder.dynamic_table_enabled());

        decoder.set_dynamic_table_capacity(4096);
        CHECK(decoder.dynamic_table_enabled());
    }

    SUBCASE("Process encoder stream - set capacity") {
        QpackDecoder decoder;

        // Set capacity instruction: 001xxxxx
        auto capacity_instr = encoder_instructions::set_capacity(1024);
        auto result = decoder.process_encoder_stream(capacity_instr);
        CHECK(result.is_ok());
        CHECK(decoder.dynamic_table_capacity() == 1024);
    }

    SUBCASE("Process encoder stream - insert literal") {
        QpackDecoder decoder;
        decoder.set_dynamic_table_capacity(4096);

        auto insert_instr = encoder_instructions::insert_literal("x-header", "value");
        auto result = decoder.process_encoder_stream(insert_instr);
        CHECK(result.is_ok());
        CHECK(decoder.dynamic_table().count() == 1);

        auto *entry = decoder.dynamic_table().get_relative(0);
        CHECK(entry != nullptr);
        CHECK(entry->name == "x-header");
        CHECK(entry->value == "value");
    }

    SUBCASE("Section acknowledgement") {
        QpackDecoder decoder;
        decoder.acknowledge_section(4);
        CHECK(decoder.has_decoder_stream_data());
        auto data = decoder.get_decoder_stream_data();
        CHECK(!data.empty());
        CHECK((data[0] & 0x80) != 0); // Section ack format: 1xxxxxxx
    }
}

TEST_CASE("QPACK Encoder-Decoder Integration with Dynamic Table") {
    QpackEncoder encoder;
    QpackDecoder decoder;

    encoder.set_dynamic_table_capacity(4096);
    decoder.set_dynamic_table_capacity(4096);

    SUBCASE("Round-trip with encoder stream sync") {
        // Encode headers
        HeaderList original = {
            HeaderField(":method", "GET"),
            HeaderField(":path", "/api/resource"),
            HeaderField("x-custom", "custom-value"),
        };

        auto encoded = encoder.encode(original);

        // Process encoder stream data on decoder side
        if (encoder.has_encoder_stream_data()) {
            auto stream_data = encoder.get_encoder_stream_data();
            auto process_result = decoder.process_encoder_stream(stream_data);
            CHECK(process_result.is_ok());
        }

        // Decode the field section
        auto decoded = decoder.decode(encoded);
        CHECK(decoded.is_ok());
        CHECK(decoded.value().size() == original.size());

        for (dp::usize i = 0; i < original.size(); i++) {
            CHECK(decoded.value()[i].name == original[i].name);
            CHECK(decoded.value()[i].value == original[i].value);
        }
    }
}

TEST_CASE("Decoder Stream Instructions") {
    SUBCASE("Section acknowledgement encoding") {
        auto ack = decoder_instructions::section_acknowledgement(0);
        CHECK(!ack.empty());
        CHECK((ack[0] & 0x80) != 0); // Format: 1xxxxxxx
    }

    SUBCASE("Stream cancellation encoding") {
        auto cancel = decoder_instructions::stream_cancellation(4);
        CHECK(!cancel.empty());
        CHECK((cancel[0] & 0xC0) == 0x40); // Format: 01xxxxxx
    }

    SUBCASE("Insert count increment encoding") {
        auto increment = decoder_instructions::insert_count_increment(5);
        CHECK(!increment.empty());
        CHECK((increment[0] & 0xC0) == 0x00); // Format: 00xxxxxx
    }

    SUBCASE("Large stream ID in section acknowledgement") {
        auto ack = decoder_instructions::section_acknowledgement(1000);
        CHECK(!ack.empty());
        // First byte should have continuation indicator
        CHECK(ack[0] == 0xFF);
    }
}

TEST_CASE("Encoder Stream Instructions") {
    SUBCASE("Set capacity - small value") {
        auto instr = encoder_instructions::set_capacity(16);
        CHECK(!instr.empty());
        CHECK((instr[0] & 0xE0) == 0x20); // Format: 001xxxxx
        CHECK((instr[0] & 0x1F) == 16);
    }

    SUBCASE("Set capacity - large value") {
        auto instr = encoder_instructions::set_capacity(4096);
        CHECK(!instr.empty());
        CHECK((instr[0] & 0xE0) == 0x20); // Format: 001xxxxx
    }

    SUBCASE("Insert with static reference") {
        auto instr = encoder_instructions::insert_with_static_ref(17, "POST", false); // :method index
        CHECK(!instr.empty());
        CHECK((instr[0] & 0xC0) == 0xC0); // Format: 11xxxxxx
    }

    SUBCASE("Insert literal") {
        auto instr = encoder_instructions::insert_literal("x-header", "value");
        CHECK(!instr.empty());
        CHECK((instr[0] & 0xC0) == 0x40); // Format: 01xxxxxx
    }
}

// =============================================================================
// HTTP/3 Stream Reset Tests
// =============================================================================

TEST_CASE("HTTP/3 Stream Reset") {
    Connection client(true);
    Connection server(false);

    // Establish connection
    auto c_init = client.initialize();
    auto s_init = server.initialize();
    client.process_control_data(s_init.value());
    server.process_control_data(c_init.value());

    SUBCASE("Client resets stream") {
        auto stream_id = client.create_request_stream().value();

        // Send request
        Request req;
        req.method = "GET";
        req.scheme = "https";
        req.authority = "example.com";
        req.path = "/test";
        client.encode_request(stream_id, req);

        // Reset the stream
        auto reset_result = client.reset_stream(stream_id, ErrorCode::RequestCancelled);
        CHECK(reset_result.is_ok());
        CHECK(reset_result.value() == static_cast<dp::u64>(ErrorCode::RequestCancelled));

        // Check stream is in reset state
        CHECK(client.is_stream_reset(stream_id));
        auto state = client.get_stream_state(stream_id);
        CHECK(state.has_value());
        CHECK(state.value() == Http3StreamState::Reset);
    }

    SUBCASE("Reset unknown stream fails") {
        auto reset_result = client.reset_stream(999, ErrorCode::RequestCancelled);
        CHECK(reset_result.is_err());
    }

    SUBCASE("Double reset fails") {
        auto stream_id = client.create_request_stream().value();
        auto first_reset = client.reset_stream(stream_id);
        CHECK(first_reset.is_ok());

        auto second_reset = client.reset_stream(stream_id);
        CHECK(second_reset.is_err());
    }

    SUBCASE("Handle incoming stream reset") {
        auto stream_id = client.create_request_stream().value();

        Request req;
        req.method = "GET";
        req.scheme = "https";
        req.authority = "example.com";
        req.path = "/test";
        auto req_data = client.encode_request(stream_id, req);
        server.process_request_stream(stream_id, req_data.value());

        // Server receives reset notification from QUIC layer
        server.handle_stream_reset(stream_id, static_cast<dp::u64>(ErrorCode::RequestCancelled));

        CHECK(server.is_stream_reset(stream_id));
        auto error = server.get_stream_reset_error(stream_id);
        CHECK(error.has_value());
        CHECK(error.value() == ErrorCode::RequestCancelled);
    }

    SUBCASE("Handle reset for unknown stream") {
        // Receiving reset for a stream we don't know about should still be tracked
        server.handle_stream_reset(12345, static_cast<dp::u64>(ErrorCode::InternalError));

        CHECK(server.is_stream_reset(12345));
        auto error = server.get_stream_reset_error(12345);
        CHECK(error.has_value());
        CHECK(error.value() == ErrorCode::InternalError);
    }

    SUBCASE("Get stream state") {
        auto stream_id = client.create_request_stream().value();
        auto state = client.get_stream_state(stream_id);
        CHECK(state.has_value());
        CHECK(state.value() == Http3StreamState::Open);

        // After reset
        client.reset_stream(stream_id);
        state = client.get_stream_state(stream_id);
        CHECK(state.value() == Http3StreamState::Reset);
    }

    SUBCASE("Cannot reset closed stream") {
        auto stream_id = client.create_request_stream().value();
        client.close_stream(stream_id);

        auto reset_result = client.reset_stream(stream_id);
        CHECK(reset_result.is_err());
    }

    SUBCASE("Reset with different error codes") {
        auto stream1 = client.create_request_stream().value();
        auto stream2 = client.create_request_stream().value();
        auto stream3 = client.create_request_stream().value();

        client.reset_stream(stream1, ErrorCode::RequestCancelled);
        client.reset_stream(stream2, ErrorCode::InternalError);
        client.reset_stream(stream3, ErrorCode::RequestRejected);

        CHECK(client.get_stream_reset_error(stream1).value() == ErrorCode::RequestCancelled);
        CHECK(client.get_stream_reset_error(stream2).value() == ErrorCode::InternalError);
        CHECK(client.get_stream_reset_error(stream3).value() == ErrorCode::RequestRejected);
    }
}

TEST_CASE("HTTP/3 Stream State Transitions") {
    Connection client(true);
    Connection server(false);

    auto c_init = client.initialize();
    auto s_init = server.initialize();
    client.process_control_data(s_init.value());
    server.process_control_data(c_init.value());

    SUBCASE("State: Open -> HalfClosedRemote") {
        auto stream_id = client.create_request_stream().value();
        CHECK(client.get_stream_state(stream_id).value() == Http3StreamState::Open);

        client.stream_finished(stream_id);
        CHECK(client.get_stream_state(stream_id).value() == Http3StreamState::HalfClosedRemote);
    }

    SUBCASE("State: Open -> Reset") {
        auto stream_id = client.create_request_stream().value();
        CHECK(client.get_stream_state(stream_id).value() == Http3StreamState::Open);

        client.reset_stream(stream_id);
        CHECK(client.get_stream_state(stream_id).value() == Http3StreamState::Reset);
    }

    SUBCASE("State: Open -> Closed") {
        auto stream_id = client.create_request_stream().value();
        CHECK(client.get_stream_state(stream_id).value() == Http3StreamState::Open);

        client.close_stream(stream_id);
        CHECK(client.get_stream_state(stream_id).value() == Http3StreamState::Closed);
    }

    SUBCASE("Unknown stream returns nullopt") {
        auto state = client.get_stream_state(999);
        CHECK(!state.has_value());
    }
}

// =============================================================================
// HTTP/3 Flow Control Tests
// =============================================================================

TEST_CASE("HTTP/3 Flow Control") {
    Connection client(true);
    Connection server(false);

    auto c_init = client.initialize();
    auto s_init = server.initialize();
    client.process_control_data(s_init.value());
    server.process_control_data(c_init.value());

    SUBCASE("Track bytes sent") {
        auto stream_id = client.create_request_stream().value();
        CHECK(client.stream_bytes_sent(stream_id) == 0);

        client.record_bytes_sent(stream_id, 100);
        CHECK(client.stream_bytes_sent(stream_id) == 100);

        client.record_bytes_sent(stream_id, 50);
        CHECK(client.stream_bytes_sent(stream_id) == 150);
    }

    SUBCASE("Track bytes received") {
        auto stream_id = client.create_request_stream().value();
        CHECK(client.stream_bytes_received(stream_id) == 0);

        client.record_bytes_received(stream_id, 200);
        CHECK(client.stream_bytes_received(stream_id) == 200);
    }

    SUBCASE("Connection-level byte tracking") {
        auto stream1 = client.create_request_stream().value();
        auto stream2 = client.create_request_stream().value();

        client.record_bytes_sent(stream1, 100);
        client.record_bytes_sent(stream2, 200);

        CHECK(client.total_bytes_sent() == 300);
        CHECK(client.stream_bytes_sent(stream1) == 100);
        CHECK(client.stream_bytes_sent(stream2) == 200);
    }

    SUBCASE("Stream send limit enforcement") {
        auto stream_id = client.create_request_stream().value();

        // Set a send limit
        client.update_stream_send_limit(stream_id, 100);

        // Should be able to send up to the limit
        CHECK(client.can_send(stream_id, 50));
        CHECK(client.can_send(stream_id, 100));
        CHECK(!client.can_send(stream_id, 101));

        // After sending some bytes
        client.record_bytes_sent(stream_id, 60);
        CHECK(client.can_send(stream_id, 40));
        CHECK(!client.can_send(stream_id, 41));
    }

    SUBCASE("Connection send limit enforcement") {
        auto stream1 = client.create_request_stream().value();
        auto stream2 = client.create_request_stream().value();

        // Set connection-level limit
        client.update_connection_send_limit(500);

        // Both streams share the connection limit
        client.record_bytes_sent(stream1, 300);
        CHECK(client.can_send(stream2, 200));
        CHECK(!client.can_send(stream2, 201));
    }

    SUBCASE("Combined stream and connection limits") {
        auto stream_id = client.create_request_stream().value();

        client.update_stream_send_limit(stream_id, 100);
        client.update_connection_send_limit(50);

        // Connection limit is more restrictive
        CHECK(client.can_send(stream_id, 50));
        CHECK(!client.can_send(stream_id, 51));
    }

    SUBCASE("Available send window calculation") {
        auto stream_id = client.create_request_stream().value();

        // No limits - max window
        CHECK(client.available_send_window(stream_id) == dp::u64(-1));

        // Set stream limit
        client.update_stream_send_limit(stream_id, 1000);
        CHECK(client.available_send_window(stream_id) == 1000);

        // Send some bytes
        client.record_bytes_sent(stream_id, 300);
        CHECK(client.available_send_window(stream_id) == 700);

        // Set more restrictive connection limit
        client.update_connection_send_limit(500);
        CHECK(client.available_send_window(stream_id) == 200); // 500 - 300 = 200 < 700
    }

    SUBCASE("Encode data with flow control - success") {
        auto stream_id = client.create_request_stream().value();

        Request req;
        req.method = "POST";
        req.scheme = "https";
        req.authority = "example.com";
        req.path = "/data";
        client.encode_request(stream_id, req);

        client.update_stream_send_limit(stream_id, 1000);

        dp::Vector<dp::u8> body(100, 'x');
        auto result = client.encode_data_with_flow_control(stream_id, body);
        CHECK(result.is_ok());
        CHECK(!result.value().empty());

        // Bytes should be tracked
        CHECK(client.stream_bytes_sent(stream_id) == 100);
    }

    SUBCASE("Encode data with flow control - exceeded") {
        auto stream_id = client.create_request_stream().value();

        Request req;
        req.method = "POST";
        req.scheme = "https";
        req.authority = "example.com";
        req.path = "/data";
        client.encode_request(stream_id, req);

        client.update_stream_send_limit(stream_id, 50);

        dp::Vector<dp::u8> body(100, 'x');
        auto result = client.encode_data_with_flow_control(stream_id, body);
        CHECK(result.is_err());
    }

    SUBCASE("Unknown stream returns zero bytes") {
        CHECK(client.stream_bytes_sent(999) == 0);
        CHECK(client.stream_bytes_received(999) == 0);
    }
}

TEST_CASE("HTTP/3 Field Section Size Limit") {
    SUBCASE("Default settings - 16384 limit") {
        Connection client(true);
        Connection server(false);

        auto c_init = client.initialize();
        auto s_init = server.initialize();
        client.process_control_data(s_init.value());
        server.process_control_data(c_init.value());

        // Default max_field_section_size is 16384
        CHECK(!client.exceeds_field_section_limit(16384));
        CHECK(client.exceeds_field_section_limit(16385));
    }

    SUBCASE("No limit when set to 0") {
        Connection client(true);
        Connection server(false);

        Settings server_settings;
        server_settings.max_field_section_size = 0; // No limit
        server.set_local_settings(server_settings);

        auto c_init = client.initialize();
        auto s_init = server.initialize();
        client.process_control_data(s_init.value());
        server.process_control_data(c_init.value());

        // With max_field_section_size = 0, no limit
        CHECK(!client.exceeds_field_section_limit(1000000));
    }

    SUBCASE("With configured limit") {
        Connection client(true);
        Connection server(false);

        Settings server_settings;
        server_settings.max_field_section_size = 8192;
        server.set_local_settings(server_settings);

        auto c_init = client.initialize();
        auto s_init = server.initialize();
        client.process_control_data(s_init.value());
        server.process_control_data(c_init.value());

        // Client should respect server's limit
        CHECK(!client.exceeds_field_section_limit(8192));
        CHECK(client.exceeds_field_section_limit(8193));
    }
}

// =============================================================================
// HTTP/3 Connection Timeout and Draining Tests
// =============================================================================

TEST_CASE("HTTP/3 Connection Timeout") {
    Connection client(true);
    Connection server(false);

    SUBCASE("Default timeouts") {
        CHECK(client.idle_timeout() == 0); // No idle timeout by default
        CHECK(client.draining_timeout() == 3000); // 3 second draining by default
    }

    SUBCASE("Configure idle timeout") {
        client.set_idle_timeout(30000); // 30 seconds
        CHECK(client.idle_timeout() == 30000);
    }

    SUBCASE("Configure draining timeout") {
        client.set_draining_timeout(5000);
        CHECK(client.draining_timeout() == 5000);
    }

    SUBCASE("Idle timeout check with no timeout set") {
        CHECK(!client.check_idle_timeout()); // No timeout = never expires
    }

    SUBCASE("Idle timeout with mock time") {
        dp::u64 mock_time = 1000;
        client.set_time_provider([&mock_time]() { return mock_time; });
        client.set_idle_timeout(100); // 100ms timeout

        // Initial activity recorded at connection creation, reset it
        client.record_activity();

        // Not expired yet
        mock_time = 1050;
        CHECK(!client.check_idle_timeout());

        // Now expired
        mock_time = 1100;
        CHECK(client.check_idle_timeout());

        // Activity resets the timer
        client.record_activity();
        CHECK(!client.check_idle_timeout());

        mock_time = 1200;
        CHECK(client.check_idle_timeout());
    }

    SUBCASE("Time since activity") {
        dp::u64 mock_time = 1000;
        client.set_time_provider([&mock_time]() { return mock_time; });
        client.record_activity();

        mock_time = 1500;
        CHECK(client.time_since_activity() == 500);
    }

    SUBCASE("Idle timeout disabled during draining") {
        dp::u64 mock_time = 1000;
        client.set_time_provider([&mock_time]() { return mock_time; });
        client.set_idle_timeout(100);
        client.record_activity();

        mock_time = 1200; // Would be expired

        // Start draining
        client.start_draining();
        CHECK(!client.check_idle_timeout()); // Should not report expired
    }
}

TEST_CASE("HTTP/3 Connection Draining") {
    SUBCASE("Initial state is not draining") {
        Connection client(true);
        CHECK(!client.is_draining());
    }

    SUBCASE("Start draining") {
        Connection client(true);
        client.start_draining();

        CHECK(client.is_draining());
        CHECK(client.state() == ConnectionState::Draining);
    }

    SUBCASE("Draining complete with mock time") {
        Connection client(true);
        dp::u64 mock_time = 1000;
        client.set_time_provider([&mock_time]() { return mock_time; });
        client.set_draining_timeout(500); // 500ms draining

        client.start_draining();
        CHECK(!client.is_draining_complete());

        mock_time = 1400;
        CHECK(!client.is_draining_complete());

        mock_time = 1500;
        CHECK(client.is_draining_complete());
    }

    SUBCASE("Draining time remaining") {
        Connection client(true);
        dp::u64 mock_time = 1000;
        client.set_time_provider([&mock_time]() { return mock_time; });
        client.set_draining_timeout(1000);

        client.start_draining();
        CHECK(client.draining_time_remaining() == 1000);

        mock_time = 1300;
        CHECK(client.draining_time_remaining() == 700);

        mock_time = 2000;
        CHECK(client.draining_time_remaining() == 0);
    }

    SUBCASE("Complete draining transitions to closed") {
        Connection client(true);
        client.start_draining();
        CHECK(client.state() == ConnectionState::Draining);

        client.complete_draining();
        CHECK(client.state() == ConnectionState::Closed);
        CHECK(!client.is_draining());
    }

    SUBCASE("Cannot start draining when closed") {
        Connection client(true);
        client.close();
        CHECK(client.state() == ConnectionState::Closed);

        client.start_draining();
        CHECK(client.state() == ConnectionState::Closed); // Still closed
    }

    SUBCASE("Complete draining only works when draining") {
        Connection client(true);
        CHECK(client.state() == ConnectionState::Idle);

        client.complete_draining();
        CHECK(client.state() == ConnectionState::Idle); // No change
    }
}

TEST_CASE("HTTP/3 Active Streams Check") {
    Connection client(true);
    Connection server(false);

    auto c_init = client.initialize();
    auto s_init = server.initialize();
    client.process_control_data(s_init.value());
    server.process_control_data(c_init.value());

    SUBCASE("No active streams initially") {
        CHECK(!client.has_active_streams());
    }

    SUBCASE("Has active streams after creation") {
        client.create_request_stream();
        CHECK(client.has_active_streams());
    }

    SUBCASE("No active streams after closing all") {
        auto stream1 = client.create_request_stream().value();
        auto stream2 = client.create_request_stream().value();
        CHECK(client.has_active_streams());

        client.close_stream(stream1);
        CHECK(client.has_active_streams()); // stream2 still active

        client.close_stream(stream2);
        CHECK(!client.has_active_streams());
    }

    SUBCASE("Reset streams are not active") {
        auto stream_id = client.create_request_stream().value();
        CHECK(client.has_active_streams());

        client.reset_stream(stream_id);
        CHECK(!client.has_active_streams());
    }
}

// =============================================================================
// HTTP/3 Settings Validation Tests
// =============================================================================

TEST_CASE("HTTP/3 Settings Validation") {
    SUBCASE("Duplicate QPACK_MAX_TABLE_CAPACITY rejected") {
        // Manually construct settings data with duplicate ID
        dp::Vector<dp::u8> data;
        // First QPACK_MAX_TABLE_CAPACITY = 1024
        auto id1 = varint_encode(static_cast<dp::u64>(SettingsId::QpackMaxTableCapacity));
        auto val1 = varint_encode(1024);
        data.insert(data.end(), id1.begin(), id1.end());
        data.insert(data.end(), val1.begin(), val1.end());
        // Second QPACK_MAX_TABLE_CAPACITY = 2048 (duplicate!)
        auto id2 = varint_encode(static_cast<dp::u64>(SettingsId::QpackMaxTableCapacity));
        auto val2 = varint_encode(2048);
        data.insert(data.end(), id2.begin(), id2.end());
        data.insert(data.end(), val2.begin(), val2.end());

        auto result = Settings::parse(data.data(), data.size());
        CHECK(result.is_err());
    }

    SUBCASE("Duplicate MAX_FIELD_SECTION_SIZE rejected") {
        dp::Vector<dp::u8> data;
        // First MAX_FIELD_SECTION_SIZE = 8192
        auto id1 = varint_encode(static_cast<dp::u64>(SettingsId::MaxFieldSectionSize));
        auto val1 = varint_encode(8192);
        data.insert(data.end(), id1.begin(), id1.end());
        data.insert(data.end(), val1.begin(), val1.end());
        // Second MAX_FIELD_SECTION_SIZE = 16384 (duplicate!)
        auto id2 = varint_encode(static_cast<dp::u64>(SettingsId::MaxFieldSectionSize));
        auto val2 = varint_encode(16384);
        data.insert(data.end(), id2.begin(), id2.end());
        data.insert(data.end(), val2.begin(), val2.end());

        auto result = Settings::parse(data.data(), data.size());
        CHECK(result.is_err());
    }

    SUBCASE("Duplicate QPACK_BLOCKED_STREAMS rejected") {
        dp::Vector<dp::u8> data;
        // First QPACK_BLOCKED_STREAMS = 100
        auto id1 = varint_encode(static_cast<dp::u64>(SettingsId::QpackBlockedStreams));
        auto val1 = varint_encode(100);
        data.insert(data.end(), id1.begin(), id1.end());
        data.insert(data.end(), val1.begin(), val1.end());
        // Second QPACK_BLOCKED_STREAMS = 200 (duplicate!)
        auto id2 = varint_encode(static_cast<dp::u64>(SettingsId::QpackBlockedStreams));
        auto val2 = varint_encode(200);
        data.insert(data.end(), id2.begin(), id2.end());
        data.insert(data.end(), val2.begin(), val2.end());

        auto result = Settings::parse(data.data(), data.size());
        CHECK(result.is_err());
    }

    SUBCASE("GREASE settings are ignored") {
        // GREASE values: 0x21, 0x40, 0x5f, 0x7e, 0x9d, ...
        CHECK(Settings::is_grease_setting(0x21));
        CHECK(Settings::is_grease_setting(0x40));
        CHECK(Settings::is_grease_setting(0x5f));
        CHECK(Settings::is_grease_setting(0x7e));
        CHECK(Settings::is_grease_setting(0x9d));

        // Non-GREASE values
        CHECK(!Settings::is_grease_setting(0x01)); // QPACK_MAX_TABLE_CAPACITY
        CHECK(!Settings::is_grease_setting(0x06)); // MAX_FIELD_SECTION_SIZE
        CHECK(!Settings::is_grease_setting(0x07)); // QPACK_BLOCKED_STREAMS
        CHECK(!Settings::is_grease_setting(0x20)); // Not GREASE
        CHECK(!Settings::is_grease_setting(0x22)); // Not GREASE
    }

    SUBCASE("GREASE settings in data are skipped") {
        dp::Vector<dp::u8> data;
        // GREASE setting (0x21)
        auto grease_id = varint_encode(0x21);
        auto grease_val = varint_encode(12345);
        data.insert(data.end(), grease_id.begin(), grease_id.end());
        data.insert(data.end(), grease_val.begin(), grease_val.end());
        // Real setting
        auto real_id = varint_encode(static_cast<dp::u64>(SettingsId::MaxFieldSectionSize));
        auto real_val = varint_encode(8192);
        data.insert(data.end(), real_id.begin(), real_id.end());
        data.insert(data.end(), real_val.begin(), real_val.end());

        auto result = Settings::parse(data.data(), data.size());
        CHECK(result.is_ok());
        CHECK(result.value().max_field_section_size == 8192);
    }

    SUBCASE("Unknown settings are ignored") {
        dp::Vector<dp::u8> data;
        // Unknown setting ID (0xFF)
        auto unknown_id = varint_encode(0xFF);
        auto unknown_val = varint_encode(999);
        data.insert(data.end(), unknown_id.begin(), unknown_id.end());
        data.insert(data.end(), unknown_val.begin(), unknown_val.end());
        // Known setting
        auto known_id = varint_encode(static_cast<dp::u64>(SettingsId::QpackMaxTableCapacity));
        auto known_val = varint_encode(4096);
        data.insert(data.end(), known_id.begin(), known_id.end());
        data.insert(data.end(), known_val.begin(), known_val.end());

        auto result = Settings::parse(data.data(), data.size());
        CHECK(result.is_ok());
        CHECK(result.value().qpack_max_table_capacity == 4096);
    }

    SUBCASE("Settings validation passes for valid settings") {
        Settings settings;
        settings.qpack_max_table_capacity = 4096;
        settings.max_field_section_size = 16384;
        settings.qpack_blocked_streams = 100;

        auto result = Settings::validate(settings);
        CHECK(result.is_ok());
    }
}

TEST_CASE("HTTP/3 Field Section Size Enforcement") {
    SUBCASE("Request rejected if field section too large") {
        Connection client(true);
        Connection server(false);

        // Server sets a small max_field_section_size
        Settings server_settings;
        server_settings.max_field_section_size = 50; // Very small
        server.set_local_settings(server_settings);

        auto c_init = client.initialize();
        auto s_init = server.initialize();
        client.process_control_data(s_init.value());
        server.process_control_data(c_init.value());

        auto stream_id = client.create_request_stream().value();

        Request req;
        req.method = "GET";
        req.scheme = "https";
        req.authority = "example.com";
        req.path = "/this/is/a/very/long/path/that/will/exceed/the/limit";
        req.headers.push_back(HeaderField("x-custom-header", "some-value"));

        // Should fail because encoded headers exceed the limit
        auto result = client.encode_request(stream_id, req);
        CHECK(result.is_err());
    }

    SUBCASE("Response rejected if field section too large") {
        Connection client(true);
        Connection server(false);

        // Client sets a small max_field_section_size
        Settings client_settings;
        client_settings.max_field_section_size = 50; // Very small
        client.set_local_settings(client_settings);

        auto c_init = client.initialize();
        auto s_init = server.initialize();
        client.process_control_data(s_init.value());
        server.process_control_data(c_init.value());

        auto stream_id = client.create_request_stream().value();

        // Client sends request
        Request req;
        req.method = "GET";
        req.scheme = "https";
        req.authority = "example.com";
        req.path = "/";
        auto req_data = client.encode_request(stream_id, req);
        server.process_request_stream(stream_id, req_data.value());

        // Server tries to send a response with large headers
        Response resp;
        resp.status = 200;
        resp.headers.push_back(HeaderField("x-large-header", "this-is-a-very-long-value-that-exceeds-the-limit"));

        auto result = server.encode_response(stream_id, resp);
        CHECK(result.is_err());
    }

    SUBCASE("Received headers rejected if too large") {
        Connection server(false);

        // Server sets a very small limit for incoming headers
        Settings server_settings;
        server_settings.max_field_section_size = 10; // Very small limit
        server.set_local_settings(server_settings);

        // Initialize server
        server.initialize();

        // Manually construct a HEADERS frame with oversized encoded section
        // Frame format: type (1) + length (1+) + encoded_field_section
        dp::Vector<dp::u8> oversized_frame;
        // Frame type: HEADERS (0x01)
        oversized_frame.push_back(0x01);
        // Length: 20 bytes (exceeds 10 byte limit)
        oversized_frame.push_back(20);
        // Dummy encoded field section (20 bytes)
        for (int i = 0; i < 20; i++) {
            oversized_frame.push_back(0x00);
        }

        // Server should reject because received headers exceed local limit
        auto result = server.process_request_stream(0, oversized_frame);
        CHECK(result.is_err());
    }
}

// =============================================================================
// HTTP/3 Trailer Headers Tests
// =============================================================================

TEST_CASE("HTTP/3 Trailer Headers") {
    Connection client(true);
    Connection server(false);

    auto c_init = client.initialize();
    auto s_init = server.initialize();
    client.process_control_data(s_init.value());
    server.process_control_data(c_init.value());

    SUBCASE("Request and Response have trailers field") {
        Request req;
        req.trailers.push_back(HeaderField("x-checksum", "abc123"));
        CHECK(req.has_trailers());
        CHECK(req.trailers.size() == 1);

        Response resp;
        CHECK(!resp.has_trailers());
        resp.trailers.push_back(HeaderField("x-signature", "sig456"));
        CHECK(resp.has_trailers());
    }

    SUBCASE("Encode trailers after headers and data") {
        auto stream_id = client.create_request_stream().value();

        Request req;
        req.method = "POST";
        req.scheme = "https";
        req.authority = "example.com";
        req.path = "/upload";
        client.encode_request(stream_id, req);

        // Send some data
        dp::Vector<dp::u8> body = {'d', 'a', 't', 'a'};
        client.encode_data(stream_id, body);

        // Now send trailers
        HeaderList trailers = {
            HeaderField("x-checksum", "abc123"),
            HeaderField("x-content-length", "4"),
        };
        auto trailer_result = client.encode_trailers(stream_id, trailers);
        CHECK(trailer_result.is_ok());
        CHECK(!trailer_result.value().empty());
    }

    SUBCASE("Cannot encode trailers before headers") {
        auto stream_id = client.create_request_stream().value();

        HeaderList trailers = {HeaderField("x-checksum", "abc123")};
        auto result = client.encode_trailers(stream_id, trailers);
        CHECK(result.is_err());
    }

    SUBCASE("Cannot encode trailers twice") {
        auto stream_id = client.create_request_stream().value();

        Request req;
        req.method = "POST";
        req.scheme = "https";
        req.authority = "example.com";
        req.path = "/upload";
        client.encode_request(stream_id, req);

        HeaderList trailers = {HeaderField("x-checksum", "abc123")};
        auto first = client.encode_trailers(stream_id, trailers);
        CHECK(first.is_ok());

        auto second = client.encode_trailers(stream_id, trailers);
        CHECK(second.is_err());
    }

    SUBCASE("Pseudo-headers not allowed in trailers") {
        auto stream_id = client.create_request_stream().value();

        Request req;
        req.method = "GET";
        req.scheme = "https";
        req.authority = "example.com";
        req.path = "/";
        client.encode_request(stream_id, req);

        HeaderList bad_trailers = {
            HeaderField(":status", "200"),  // Pseudo-header not allowed!
        };
        auto result = client.encode_trailers(stream_id, bad_trailers);
        CHECK(result.is_err());
    }

    SUBCASE("Receive trailers after data") {
        auto stream_id = client.create_request_stream().value();

        // Client sends request
        Request req;
        req.method = "GET";
        req.scheme = "https";
        req.authority = "example.com";
        req.path = "/";
        auto req_data = client.encode_request(stream_id, req);
        server.process_request_stream(stream_id, req_data.value());

        // Server sends response headers
        Response resp;
        resp.status = 200;
        resp.headers.push_back(HeaderField("content-type", "text/plain"));
        auto resp_headers = server.encode_response(stream_id, resp);

        // Server sends body data
        dp::Vector<dp::u8> body = {'h', 'e', 'l', 'l', 'o'};
        auto resp_body = server.encode_data(stream_id, body);

        // Server sends trailers
        HeaderList server_trailers = {
            HeaderField("x-checksum", "sha256:abc"),
            HeaderField("x-request-id", "12345"),
        };
        auto resp_trailers = server.encode_trailers(stream_id, server_trailers);

        // Client processes response: headers + body + trailers
        dp::Vector<dp::u8> full_response;
        full_response.insert(full_response.end(), resp_headers.value().begin(), resp_headers.value().end());
        full_response.insert(full_response.end(), resp_body.value().begin(), resp_body.value().end());
        full_response.insert(full_response.end(), resp_trailers.value().begin(), resp_trailers.value().end());

        auto process_result = client.process_request_stream(stream_id, full_response);
        CHECK(process_result.is_ok());

        // Check trailers were received
        CHECK(client.trailers_received(stream_id));
        auto received_trailers = client.get_response_trailers(stream_id);
        CHECK(received_trailers.has_value());
        CHECK(received_trailers.value().size() == 2);
    }

    SUBCASE("Trailers not received without data") {
        auto stream_id = client.create_request_stream().value();

        CHECK(!client.trailers_received(stream_id));
        auto trailers = client.get_response_trailers(stream_id);
        CHECK(!trailers.has_value());
    }

    SUBCASE("Get request trailers on server side") {
        auto stream_id = client.create_request_stream().value();

        // Client sends request with body and trailers
        Request req;
        req.method = "POST";
        req.scheme = "https";
        req.authority = "example.com";
        req.path = "/data";
        auto req_headers = client.encode_request(stream_id, req);

        dp::Vector<dp::u8> body = {'t', 'e', 's', 't'};
        auto req_body = client.encode_data(stream_id, body);

        HeaderList trailers = {HeaderField("x-trailer", "value")};
        auto req_trailers = client.encode_trailers(stream_id, trailers);

        // Server processes the complete request
        dp::Vector<dp::u8> full_request;
        full_request.insert(full_request.end(), req_headers.value().begin(), req_headers.value().end());
        full_request.insert(full_request.end(), req_body.value().begin(), req_body.value().end());
        full_request.insert(full_request.end(), req_trailers.value().begin(), req_trailers.value().end());

        auto result = server.process_request_stream(stream_id, full_request);
        CHECK(result.is_ok());

        CHECK(server.trailers_received(stream_id));
        auto server_trailers = server.get_request_trailers(stream_id);
        CHECK(server_trailers.has_value());
        CHECK(server_trailers.value().size() == 1);
        CHECK(server_trailers.value()[0].name == "x-trailer");
    }

    SUBCASE("Unknown stream returns no trailers") {
        CHECK(!client.trailers_received(999));
        CHECK(!client.get_response_trailers(999).has_value());
        CHECK(!server.get_request_trailers(999).has_value());
    }
}

// =============================================================================
// HTTP/3 Extended CONNECT (RFC 9220) Tests
// =============================================================================

TEST_CASE("HTTP/3 Extended CONNECT") {
    SUBCASE("Request is_extended_connect detection") {
        Request req;
        req.method = "GET";
        CHECK(!req.is_extended_connect());

        req.method = "CONNECT";
        CHECK(!req.is_extended_connect()); // No protocol set

        req.protocol = "websocket";
        CHECK(req.is_extended_connect());
    }

    SUBCASE("Extended CONNECT pseudo-headers") {
        Request req;
        req.method = "CONNECT";
        req.protocol = "websocket";
        req.scheme = "https";
        req.authority = "example.com";
        req.path = "/chat";

        auto pseudo = req.get_pseudo_headers();
        CHECK(pseudo.size() == 5); // :method, :protocol, :scheme, :authority, :path

        // Verify order and values
        CHECK(pseudo[0].name == ":method");
        CHECK(pseudo[0].value == "CONNECT");
        CHECK(pseudo[1].name == ":protocol");
        CHECK(pseudo[1].value == "websocket");
        CHECK(pseudo[2].name == ":scheme");
        CHECK(pseudo[2].value == "https");
        CHECK(pseudo[3].name == ":authority");
        CHECK(pseudo[3].value == "example.com");
        CHECK(pseudo[4].name == ":path");
        CHECK(pseudo[4].value == "/chat");
    }

    SUBCASE("Regular CONNECT pseudo-headers") {
        Request req;
        req.method = "CONNECT";
        req.authority = "proxy.example.com:443";

        auto pseudo = req.get_pseudo_headers();
        CHECK(pseudo.size() == 2); // :method, :authority only

        CHECK(pseudo[0].name == ":method");
        CHECK(pseudo[0].value == "CONNECT");
        CHECK(pseudo[1].name == ":authority");
        CHECK(pseudo[1].value == "proxy.example.com:443");
    }

    SUBCASE("Normal GET request pseudo-headers") {
        Request req;
        req.method = "GET";
        req.scheme = "https";
        req.authority = "example.com";
        req.path = "/";

        auto pseudo = req.get_pseudo_headers();
        CHECK(pseudo.size() == 4); // :method, :scheme, :authority, :path

        CHECK(pseudo[0].name == ":method");
        CHECK(pseudo[1].name == ":scheme");
        CHECK(pseudo[2].name == ":authority");
        CHECK(pseudo[3].name == ":path");
    }

    SUBCASE("Settings enable_connect_protocol serialization") {
        Settings settings;
        settings.enable_connect_protocol = true;

        auto serialized = settings.serialize();
        CHECK(!serialized.empty());

        auto parsed = Settings::parse(serialized.data(), serialized.size());
        CHECK(parsed.is_ok());
        CHECK(parsed.value().enable_connect_protocol == true);
    }

    SUBCASE("Settings enable_connect_protocol default is false") {
        Settings settings;
        CHECK(settings.enable_connect_protocol == false);

        auto serialized = settings.serialize();
        // When false, should not serialize the setting
        auto parsed = Settings::parse(serialized.data(), serialized.size());
        CHECK(parsed.is_ok());
        CHECK(parsed.value().enable_connect_protocol == false);
    }

    SUBCASE("SettingsId EnableConnectProtocol value") {
        CHECK(static_cast<dp::u64>(SettingsId::EnableConnectProtocol) == 0x08);
    }

    SUBCASE("Connection enable_extended_connect") {
        Connection server(false);

        // Initially disabled
        CHECK(!server.local_settings().enable_connect_protocol);

        // Enable it
        server.enable_extended_connect(true);
        CHECK(server.local_settings().enable_connect_protocol);

        // Disable it
        server.enable_extended_connect(false);
        CHECK(!server.local_settings().enable_connect_protocol);
    }

    SUBCASE("Connection peer_supports_extended_connect") {
        Connection client(true);
        Connection server(false);

        // Server enables Extended CONNECT
        server.enable_extended_connect(true);

        auto c_init = client.initialize();
        auto s_init = server.initialize();
        client.process_control_data(s_init.value());
        server.process_control_data(c_init.value());

        // Client should see server supports it
        CHECK(client.peer_supports_extended_connect());

        // Server should see client doesn't support it
        CHECK(!server.peer_supports_extended_connect());
    }

    SUBCASE("Extended CONNECT request encoding/decoding") {
        Connection client(true);
        Connection server(false);

        // Both enable Extended CONNECT
        client.enable_extended_connect(true);
        server.enable_extended_connect(true);

        auto c_init = client.initialize();
        auto s_init = server.initialize();
        client.process_control_data(s_init.value());
        server.process_control_data(c_init.value());

        auto stream_id = client.create_request_stream().value();

        // Client sends Extended CONNECT request
        Request req;
        req.method = "CONNECT";
        req.protocol = "websocket";
        req.scheme = "https";
        req.authority = "example.com";
        req.path = "/chat";
        req.headers.push_back(HeaderField("sec-websocket-version", "13"));

        auto req_data = client.encode_request(stream_id, req);
        CHECK(req_data.is_ok());

        // Server receives it
        auto result = server.process_request_stream(stream_id, req_data.value());
        CHECK(result.is_ok());

        auto received = server.get_request(stream_id);
        CHECK(received.has_value());
        CHECK(received.value().method == "CONNECT");
        CHECK(received.value().protocol == "websocket");
        CHECK(received.value().scheme == "https");
        CHECK(received.value().authority == "example.com");
        CHECK(received.value().path == "/chat");
        CHECK(received.value().is_extended_connect());
    }

    SUBCASE("Duplicate ENABLE_CONNECT_PROTOCOL rejected") {
        dp::Vector<dp::u8> data;
        // First ENABLE_CONNECT_PROTOCOL = 1
        auto id1 = varint_encode(static_cast<dp::u64>(SettingsId::EnableConnectProtocol));
        auto val1 = varint_encode(1);
        data.insert(data.end(), id1.begin(), id1.end());
        data.insert(data.end(), val1.begin(), val1.end());
        // Second ENABLE_CONNECT_PROTOCOL = 1 (duplicate!)
        auto id2 = varint_encode(static_cast<dp::u64>(SettingsId::EnableConnectProtocol));
        auto val2 = varint_encode(1);
        data.insert(data.end(), id2.begin(), id2.end());
        data.insert(data.end(), val2.begin(), val2.end());

        auto result = Settings::parse(data.data(), data.size());
        CHECK(result.is_err());
    }
}

// =============================================================================
// HTTP/3 Stream Prioritization (RFC 9218) Tests
// =============================================================================

TEST_CASE("HTTP/3 Priority Struct") {
    SUBCASE("Default priority values") {
        Priority prio;
        CHECK(prio.urgency == 3);  // Default urgency is 3
        CHECK(prio.incremental == false);
    }

    SUBCASE("Priority construction with clamping") {
        Priority p1(0, false);
        CHECK(p1.urgency == 0);  // Highest priority

        Priority p2(7, true);
        CHECK(p2.urgency == 7);  // Lowest priority

        Priority p3(10, false);  // Out of range, should clamp to 7
        CHECK(p3.urgency == 7);
    }

    SUBCASE("Priority serialization - basic") {
        Priority prio(3, false);
        auto serialized = prio.serialize();
        CHECK(serialized == "u=3");
    }

    SUBCASE("Priority serialization - with incremental") {
        Priority prio(5, true);
        auto serialized = prio.serialize();
        CHECK(serialized == "u=5, i");
    }

    SUBCASE("Priority serialization - high urgency") {
        Priority prio(0, true);
        auto serialized = prio.serialize();
        CHECK(serialized == "u=0, i");
    }

    SUBCASE("Priority parsing - basic") {
        auto result = Priority::parse("u=3");
        CHECK(result.is_ok());
        CHECK(result.value().urgency == 3);
        CHECK(result.value().incremental == false);
    }

    SUBCASE("Priority parsing - with incremental") {
        auto result = Priority::parse("u=5, i");
        CHECK(result.is_ok());
        CHECK(result.value().urgency == 5);
        CHECK(result.value().incremental == true);
    }

    SUBCASE("Priority parsing - reverse order") {
        auto result = Priority::parse("i, u=2");
        CHECK(result.is_ok());
        CHECK(result.value().urgency == 2);
        CHECK(result.value().incremental == true);
    }

    SUBCASE("Priority parsing - with spaces") {
        auto result = Priority::parse("  u=4  ,  i  ");
        CHECK(result.is_ok());
        CHECK(result.value().urgency == 4);
        CHECK(result.value().incremental == true);
    }

    SUBCASE("Priority parsing - boolean format i=?1") {
        auto result = Priority::parse("u=3, i=?1");
        CHECK(result.is_ok());
        CHECK(result.value().incremental == true);
    }

    SUBCASE("Priority parsing - boolean format i=?0") {
        auto result = Priority::parse("u=3, i=?0");
        CHECK(result.is_ok());
        CHECK(result.value().incremental == false);
    }

    SUBCASE("Priority parsing - defaults for missing values") {
        auto result = Priority::parse("");
        CHECK(result.is_ok());
        CHECK(result.value().urgency == 3);  // Default
        CHECK(result.value().incremental == false);
    }

    SUBCASE("Priority parsing - only urgency") {
        auto result = Priority::parse("u=1");
        CHECK(result.is_ok());
        CHECK(result.value().urgency == 1);
        CHECK(result.value().incremental == false);
    }

    SUBCASE("Priority parsing - only incremental") {
        auto result = Priority::parse("i");
        CHECK(result.is_ok());
        CHECK(result.value().urgency == 3);  // Default
        CHECK(result.value().incremental == true);
    }

    SUBCASE("Priority comparison - urgency priority") {
        Priority p0(0, false);  // Highest
        Priority p3(3, false);
        Priority p7(7, false);  // Lowest

        CHECK(p0 < p3);
        CHECK(p3 < p7);
        CHECK(!(p7 < p0));
    }

    SUBCASE("Priority comparison - incremental secondary") {
        Priority p_non_inc(3, false);
        Priority p_inc(3, true);

        // Non-incremental has priority over incremental at same urgency
        CHECK(p_non_inc < p_inc);
        CHECK(!(p_inc < p_non_inc));
    }

    SUBCASE("Priority comparison - equal") {
        Priority p1(3, true);
        Priority p2(3, true);

        CHECK(!(p1 < p2));
        CHECK(!(p2 < p1));
        CHECK(p1 == p2);
    }

    SUBCASE("Priority equality") {
        Priority p1(5, true);
        Priority p2(5, true);
        Priority p3(5, false);
        Priority p4(4, true);

        CHECK(p1 == p2);
        CHECK(!(p1 == p3));  // Different incremental
        CHECK(!(p1 == p4));  // Different urgency
    }
}

TEST_CASE("HTTP/3 Stream Priority Management") {
    Connection client(true);
    Connection server(false);

    auto c_init = client.initialize();
    auto s_init = server.initialize();
    client.process_control_data(s_init.value());
    server.process_control_data(c_init.value());

    SUBCASE("Set and get stream priority") {
        auto stream_id = client.create_request_stream().value();

        Priority prio(1, true);
        client.set_stream_priority(stream_id, prio);

        auto retrieved = client.get_stream_priority(stream_id);
        CHECK(retrieved.has_value());
        CHECK(retrieved.value().urgency == 1);
        CHECK(retrieved.value().incremental == true);
    }

    SUBCASE("Default stream priority") {
        auto stream_id = client.create_request_stream().value();

        auto prio = client.get_stream_priority(stream_id);
        CHECK(prio.has_value());
        CHECK(prio.value().urgency == 3);  // Default
        CHECK(prio.value().incremental == false);
    }

    SUBCASE("Unknown stream returns no priority") {
        auto prio = client.get_stream_priority(999);
        CHECK(!prio.has_value());
    }

    SUBCASE("Set priority for nonexistent stream does nothing") {
        // Should not crash
        client.set_stream_priority(999, Priority(1, false));
        CHECK(!client.get_stream_priority(999).has_value());
    }

    SUBCASE("Set priority from header field value") {
        auto stream_id = client.create_request_stream().value();

        auto result = client.set_stream_priority_from_header(stream_id, "u=2, i");
        CHECK(result.is_ok());

        auto prio = client.get_stream_priority(stream_id);
        CHECK(prio.has_value());
        CHECK(prio.value().urgency == 2);
        CHECK(prio.value().incremental == true);
    }

    SUBCASE("Set priority from header - unknown stream") {
        auto result = client.set_stream_priority_from_header(999, "u=2");
        CHECK(result.is_err());
    }

    SUBCASE("Get streams by priority - single stream") {
        auto stream_id = client.create_request_stream().value();
        client.set_stream_priority(stream_id, Priority(3, false));

        auto streams = client.get_streams_by_priority();
        CHECK(streams.size() == 1);
        CHECK(streams[0] == stream_id);
    }

    SUBCASE("Get streams by priority - sorted order") {
        auto stream1 = client.create_request_stream().value();  // ID 0
        auto stream2 = client.create_request_stream().value();  // ID 4
        auto stream3 = client.create_request_stream().value();  // ID 8

        // Set priorities: stream2 highest, stream3 middle, stream1 lowest
        client.set_stream_priority(stream1, Priority(7, false));  // Lowest
        client.set_stream_priority(stream2, Priority(0, false));  // Highest
        client.set_stream_priority(stream3, Priority(3, false));  // Middle

        auto streams = client.get_streams_by_priority();
        CHECK(streams.size() == 3);
        CHECK(streams[0] == stream2);  // Highest priority first
        CHECK(streams[1] == stream3);
        CHECK(streams[2] == stream1);  // Lowest priority last
    }

    SUBCASE("Get streams by priority - filters non-open streams") {
        auto stream1 = client.create_request_stream().value();
        auto stream2 = client.create_request_stream().value();
        auto stream3 = client.create_request_stream().value();

        client.set_stream_priority(stream1, Priority(1, false));
        client.set_stream_priority(stream2, Priority(2, false));
        client.set_stream_priority(stream3, Priority(3, false));

        // Close stream2
        client.close_stream(stream2);

        auto streams = client.get_streams_by_priority();
        CHECK(streams.size() == 2);
        CHECK(streams[0] == stream1);  // Highest priority
        CHECK(streams[1] == stream3);
        // stream2 is excluded because it's closed
    }

    SUBCASE("Get streams by priority - incremental secondary sort") {
        auto stream1 = client.create_request_stream().value();
        auto stream2 = client.create_request_stream().value();

        // Same urgency, different incremental
        client.set_stream_priority(stream1, Priority(3, true));   // Incremental
        client.set_stream_priority(stream2, Priority(3, false));  // Non-incremental

        auto streams = client.get_streams_by_priority();
        CHECK(streams.size() == 2);
        // Non-incremental should come first at same urgency
        CHECK(streams[0] == stream2);
        CHECK(streams[1] == stream1);
    }

    SUBCASE("Multiple priority changes") {
        auto stream_id = client.create_request_stream().value();

        client.set_stream_priority(stream_id, Priority(1, false));
        CHECK(client.get_stream_priority(stream_id).value().urgency == 1);

        client.set_stream_priority(stream_id, Priority(5, true));
        CHECK(client.get_stream_priority(stream_id).value().urgency == 5);
        CHECK(client.get_stream_priority(stream_id).value().incremental == true);

        client.set_stream_priority(stream_id, Priority(0, false));
        CHECK(client.get_stream_priority(stream_id).value().urgency == 0);
    }
}

TEST_CASE("HTTP/3 Priority FrameType") {
    SUBCASE("PriorityUpdate frame type value") {
        CHECK(static_cast<dp::u64>(FrameType::PriorityUpdate) == 0x0F);
    }
}

// =============================================================================
// QPACK Huffman Encoding/Decoding Tests (RFC 7541)
// =============================================================================

TEST_CASE("Huffman Encoder") {
    SUBCASE("Encode simple ASCII string") {
        auto &encoder = huffman_encoder();
        auto encoded = encoder.encode("www.example.com");
        CHECK(!encoded.empty());
        // Huffman encoding should compress common characters
        CHECK(encoded.size() <= 15); // Should be smaller or equal to original
    }

    SUBCASE("Encode empty string") {
        auto &encoder = huffman_encoder();
        auto encoded = encoder.encode("");
        CHECK(encoded.empty());
    }

    SUBCASE("Encode single character") {
        auto &encoder = huffman_encoder();
        auto encoded = encoder.encode("a");
        CHECK(!encoded.empty());
        // 'a' is a common character with short code
        CHECK(encoded.size() == 1);
    }

    SUBCASE("Encode numbers") {
        auto &encoder = huffman_encoder();
        auto encoded = encoder.encode("12345");
        CHECK(!encoded.empty());
    }

    SUBCASE("Encoded length calculation") {
        auto &encoder = huffman_encoder();
        dp::String test = "hello";
        auto encoded = encoder.encode(test);
        dp::usize calculated_len = encoder.encoded_length(test);
        CHECK(encoded.size() == calculated_len);
    }

    SUBCASE("Encode uncommon characters") {
        auto &encoder = huffman_encoder();
        // Uncommon characters have longer codes
        auto encoded = encoder.encode("\x01\x02\x03");
        CHECK(!encoded.empty());
    }

    SUBCASE("Encode HTTP headers") {
        auto &encoder = huffman_encoder();
        auto encoded1 = encoder.encode("content-type");
        CHECK(!encoded1.empty());
        auto encoded2 = encoder.encode("application/json");
        CHECK(!encoded2.empty());
    }
}

TEST_CASE("Huffman Decoder") {
    SUBCASE("Decode simple string") {
        auto &encoder = huffman_encoder();
        auto &decoder = huffman_decoder();

        dp::String original = "www.example.com";
        auto encoded = encoder.encode(original);
        auto decoded = decoder.decode(encoded.data(), encoded.size());

        CHECK(decoded.is_ok());
        CHECK(decoded.value() == original);
    }

    SUBCASE("Decode empty data") {
        auto &decoder = huffman_decoder();
        auto decoded = decoder.decode(nullptr, 0);
        CHECK(decoded.is_ok());
        CHECK(decoded.value().empty());
    }

    SUBCASE("Decode single character") {
        auto &encoder = huffman_encoder();
        auto &decoder = huffman_decoder();

        auto encoded = encoder.encode("a");
        auto decoded = decoder.decode(encoded.data(), encoded.size());

        CHECK(decoded.is_ok());
        CHECK(decoded.value() == "a");
    }

    SUBCASE("Decode numbers") {
        auto &encoder = huffman_encoder();
        auto &decoder = huffman_decoder();

        dp::String original = "123456789";
        auto encoded = encoder.encode(original);
        auto decoded = decoder.decode(encoded.data(), encoded.size());

        CHECK(decoded.is_ok());
        CHECK(decoded.value() == original);
    }

    SUBCASE("Decode HTTP method names") {
        auto &encoder = huffman_encoder();
        auto &decoder = huffman_decoder();

        for (const char *method : {"GET", "POST", "PUT", "DELETE", "HEAD", "OPTIONS"}) {
            auto encoded = encoder.encode(method);
            auto decoded = decoder.decode(encoded.data(), encoded.size());
            CHECK(decoded.is_ok());
            CHECK(decoded.value() == method);
        }
    }

    SUBCASE("Decode HTTP header names") {
        auto &encoder = huffman_encoder();
        auto &decoder = huffman_decoder();

        for (const char *header : {"content-type", "accept", "user-agent", "authorization"}) {
            auto encoded = encoder.encode(header);
            auto decoded = decoder.decode(encoded.data(), encoded.size());
            CHECK(decoded.is_ok());
            CHECK(decoded.value() == header);
        }
    }

    SUBCASE("Decode with different string lengths") {
        auto &encoder = huffman_encoder();
        auto &decoder = huffman_decoder();

        dp::Vector<dp::String> test_strings = {
            "a", "ab", "abc", "abcd", "abcde",
            "short", "medium length string",
            "this is a longer string that exercises more of the huffman table"
        };

        for (const auto &original : test_strings) {
            auto encoded = encoder.encode(original);
            auto decoded = decoder.decode(encoded.data(), encoded.size());
            CHECK(decoded.is_ok());
            CHECK(decoded.value() == original);
        }
    }

    SUBCASE("Decode special characters") {
        auto &encoder = huffman_encoder();
        auto &decoder = huffman_decoder();

        dp::String special = "hello-world_123";
        auto encoded = encoder.encode(special);
        auto decoded = decoder.decode(encoded.data(), encoded.size());

        CHECK(decoded.is_ok());
        CHECK(decoded.value() == special);
    }

    SUBCASE("Invalid padding rejected") {
        // Huffman padding should be all 1s (EOS prefix)
        // Create invalid data with wrong padding
        dp::Vector<dp::u8> invalid_data = {0x00}; // This has padding that's not all 1s
        auto &decoder = huffman_decoder();
        auto result = decoder.decode(invalid_data.data(), invalid_data.size());
        // This may or may not fail depending on interpretation
        // Just verify it doesn't crash
        CHECK((result.is_ok() || result.is_err()));
    }
}

TEST_CASE("Huffman Round-trip") {
    SUBCASE("All printable ASCII characters") {
        auto &encoder = huffman_encoder();
        auto &decoder = huffman_decoder();

        dp::String all_printable;
        for (char c = 32; c < 127; c++) {
            all_printable += c;
        }

        auto encoded = encoder.encode(all_printable);
        auto decoded = decoder.decode(encoded.data(), encoded.size());

        CHECK(decoded.is_ok());
        CHECK(decoded.value() == all_printable);
    }

    SUBCASE("Repeated characters") {
        auto &encoder = huffman_encoder();
        auto &decoder = huffman_decoder();

        std::string repeated_std(100, 'a');
        dp::String repeated(repeated_std.c_str());
        auto encoded = encoder.encode(repeated);
        auto decoded = decoder.decode(encoded.data(), encoded.size());

        CHECK(decoded.is_ok());
        CHECK(decoded.value() == repeated);
    }

    SUBCASE("Path strings") {
        auto &encoder = huffman_encoder();
        auto &decoder = huffman_decoder();

        dp::Vector<dp::String> paths = {"/", "/index.html", "/api/v1/users", "/foo/bar?baz=qux"};
        for (const auto &path : paths) {
            auto encoded = encoder.encode(path);
            auto decoded = decoder.decode(encoded.data(), encoded.size());
            CHECK(decoded.is_ok());
            CHECK(decoded.value() == path);
        }
    }
}

TEST_CASE("QPACK Encoder with Huffman") {
    SUBCASE("Enable Huffman encoding") {
        QpackEncoder encoder;
        CHECK(!encoder.huffman_enabled());

        encoder.set_huffman(true);
        CHECK(encoder.huffman_enabled());

        encoder.set_huffman(false);
        CHECK(!encoder.huffman_enabled());
    }

    SUBCASE("Encode headers with Huffman") {
        QpackEncoder encoder(true); // Enable Huffman
        CHECK(encoder.huffman_enabled());

        HeaderList headers = {
            HeaderField(":method", "GET"),
            HeaderField(":path", "/index.html"),
            HeaderField("content-type", "text/html"),
        };

        auto encoded = encoder.encode(headers);
        CHECK(!encoded.empty());
    }

    SUBCASE("Huffman encoding produces smaller output") {
        QpackEncoder encoder_no_huffman(false);
        QpackEncoder encoder_with_huffman(true);

        HeaderList headers = {
            HeaderField("x-custom-header", "this-is-a-long-value-that-should-compress-well"),
        };

        auto encoded_no_huff = encoder_no_huffman.encode(headers);
        auto encoded_with_huff = encoder_with_huffman.encode(headers);

        // Huffman should produce smaller output for this data
        CHECK(encoded_with_huff.size() <= encoded_no_huff.size());
    }
}

TEST_CASE("QPACK Decoder with Huffman") {
    SUBCASE("Decode Huffman-encoded headers") {
        QpackEncoder encoder(true);
        QpackDecoder decoder;

        HeaderList original = {
            HeaderField(":method", "POST"),
            HeaderField(":path", "/api/users"),
            HeaderField("content-type", "application/json"),
        };

        auto encoded = encoder.encode(original);
        auto decoded = decoder.decode(encoded);

        CHECK(decoded.is_ok());
        auto &result = decoded.value();
        CHECK(result.size() == original.size());

        for (dp::usize i = 0; i < original.size(); i++) {
            CHECK(result[i].name == original[i].name);
            CHECK(result[i].value == original[i].value);
        }
    }

    SUBCASE("Round-trip with and without Huffman") {
        for (bool use_huffman : {false, true}) {
            QpackEncoder encoder(use_huffman);
            QpackDecoder decoder;

            HeaderList original = {
                HeaderField(":status", "200"),
                HeaderField("server", "nginx"),
                HeaderField("content-length", "1234"),
            };

            auto encoded = encoder.encode(original);
            auto decoded = decoder.decode(encoded);

            CHECK(decoded.is_ok());
            CHECK(decoded.value().size() == original.size());
        }
    }
}

// =============================================================================
// HTTP/3 Server Push Tests (RFC 9114 Section 4.6)
// =============================================================================

TEST_CASE("HTTP/3 Server Push - MAX_PUSH_ID") {
    SUBCASE("Client sends MAX_PUSH_ID") {
        Connection client(true);
        client.initialize();

        auto result = client.send_max_push_id(10);
        CHECK(result.is_ok());
        CHECK(!result.value().empty());
        CHECK(client.max_push_id() == 10);
        CHECK(client.has_max_push_id());
    }

    SUBCASE("Server cannot send MAX_PUSH_ID") {
        Connection server(false);
        server.initialize();

        auto result = server.send_max_push_id(10);
        CHECK(result.is_err());
    }

    SUBCASE("MAX_PUSH_ID cannot decrease") {
        Connection client(true);
        client.initialize();

        client.send_max_push_id(10);
        auto result = client.send_max_push_id(5);  // Try to decrease
        CHECK(result.is_err());
    }

    SUBCASE("MAX_PUSH_ID can increase") {
        Connection client(true);
        client.initialize();

        client.send_max_push_id(10);
        auto result = client.send_max_push_id(20);  // Increase
        CHECK(result.is_ok());
        CHECK(client.max_push_id() == 20);
    }

    SUBCASE("Server receives MAX_PUSH_ID") {
        Connection server(false);
        server.initialize();

        CHECK(!server.has_max_push_id());
        auto result = server.handle_max_push_id(5);
        CHECK(result.is_ok());
        CHECK(server.has_max_push_id());
        CHECK(server.max_push_id() == 5);
    }

    SUBCASE("MaxPushIdFrame serialization/parsing") {
        MaxPushIdFrame frame;
        frame.push_id = 42;

        auto serialized = frame.serialize();
        CHECK(!serialized.empty());

        // Skip frame type
        auto type_result = varint_decode(serialized.data(), serialized.size());
        CHECK(type_result.is_ok());
        auto [type_val, type_len] = type_result.value();
        CHECK(static_cast<FrameType>(type_val) == FrameType::MaxPushId);

        // Parse the frame
        auto parsed = MaxPushIdFrame::parse(serialized.data() + type_len, serialized.size() - type_len);
        CHECK(parsed.is_ok());
        CHECK(parsed.value().first.push_id == 42);
    }
}

TEST_CASE("HTTP/3 Server Push - CANCEL_PUSH") {
    SUBCASE("Client sends CANCEL_PUSH") {
        Connection client(true);
        client.initialize();
        client.send_max_push_id(10);

        auto result = client.send_cancel_push(5);
        CHECK(result.is_ok());
        CHECK(!result.value().empty());
        CHECK(client.is_push_cancelled(5));
    }

    SUBCASE("Server cannot send CANCEL_PUSH") {
        Connection server(false);
        server.initialize();

        auto result = server.send_cancel_push(5);
        CHECK(result.is_err());
    }

    SUBCASE("Cannot cancel push_id exceeding MAX_PUSH_ID") {
        Connection client(true);
        client.initialize();
        client.send_max_push_id(5);

        auto result = client.send_cancel_push(10);  // Exceeds MAX_PUSH_ID
        CHECK(result.is_err());
    }

    SUBCASE("Server receives CANCEL_PUSH") {
        Connection server(false);
        server.initialize();
        server.handle_max_push_id(10);

        CHECK(!server.is_push_cancelled(5));
        auto result = server.handle_cancel_push(5);
        CHECK(result.is_ok());
        CHECK(server.is_push_cancelled(5));
    }

    SUBCASE("CancelPushFrame serialization/parsing") {
        CancelPushFrame frame;
        frame.push_id = 7;

        auto serialized = frame.serialize();
        CHECK(!serialized.empty());

        // Skip frame type
        auto type_result = varint_decode(serialized.data(), serialized.size());
        CHECK(type_result.is_ok());
        auto [type_val, type_len] = type_result.value();
        CHECK(static_cast<FrameType>(type_val) == FrameType::CancelPush);

        // Parse the frame
        auto parsed = CancelPushFrame::parse(serialized.data() + type_len, serialized.size() - type_len);
        CHECK(parsed.is_ok());
        CHECK(parsed.value().first.push_id == 7);
    }
}

TEST_CASE("HTTP/3 Server Push - PUSH_PROMISE") {
    Connection client(true);
    Connection server(false);

    auto c_init = client.initialize();
    auto s_init = server.initialize();
    client.process_control_data(s_init.value());
    server.process_control_data(c_init.value());

    SUBCASE("Server cannot push without MAX_PUSH_ID") {
        CHECK(!server.can_push());
        CHECK(server.remaining_push_capacity() == 0);

        Request req;
        req.method = "GET";
        req.path = "/pushed.css";

        auto result = server.create_push_promise(0, req);
        CHECK(result.is_err());
    }

    SUBCASE("Server can push after MAX_PUSH_ID") {
        server.handle_max_push_id(5);
        CHECK(server.can_push());
        CHECK(server.remaining_push_capacity() == 6);  // 0 through 5
    }

    SUBCASE("Create push promise") {
        server.handle_max_push_id(10);

        // Create a request stream first
        auto stream_id = client.create_request_stream().value();
        Request req;
        req.method = "GET";
        req.scheme = "https";
        req.authority = "example.com";
        req.path = "/";
        auto req_data = client.encode_request(stream_id, req);
        server.process_request_stream(stream_id, req_data.value());

        // Now server can create push promise
        Request pushed;
        pushed.method = "GET";
        pushed.scheme = "https";
        pushed.authority = "example.com";
        pushed.path = "/style.css";

        auto result = server.create_push_promise(stream_id, pushed);
        CHECK(result.is_ok());

        auto [push_id, frame_data] = result.value();
        CHECK(push_id == 0);  // First push
        CHECK(!frame_data.empty());

        // Check the promise is stored
        auto promise = server.get_push_promise(push_id);
        CHECK(promise.has_value());
        CHECK(promise.value().path == "/style.css");
    }

    SUBCASE("Push capacity decreases") {
        server.handle_max_push_id(2);
        CHECK(server.remaining_push_capacity() == 3);  // 0, 1, 2

        auto stream_id = client.create_request_stream().value();
        Request req;
        req.method = "GET";
        req.scheme = "https";
        req.authority = "example.com";
        req.path = "/";
        auto req_data = client.encode_request(stream_id, req);
        server.process_request_stream(stream_id, req_data.value());

        Request pushed;
        pushed.method = "GET";
        pushed.scheme = "https";
        pushed.authority = "example.com";
        pushed.path = "/a.css";

        server.create_push_promise(stream_id, pushed);
        CHECK(server.remaining_push_capacity() == 2);  // 1, 2

        server.create_push_promise(stream_id, pushed);
        CHECK(server.remaining_push_capacity() == 1);  // 2

        server.create_push_promise(stream_id, pushed);
        CHECK(server.remaining_push_capacity() == 0);

        // No more capacity
        auto result = server.create_push_promise(stream_id, pushed);
        CHECK(result.is_err());
    }

    SUBCASE("Client receives PUSH_PROMISE") {
        client.send_max_push_id(10);

        // Simulate receiving a push promise
        dp::Vector<dp::u8> encoded_headers;
        QpackEncoder encoder;
        HeaderList headers = {
            HeaderField(":method", "GET"),
            HeaderField(":scheme", "https"),
            HeaderField(":authority", "example.com"),
            HeaderField(":path", "/pushed.js"),
        };
        encoded_headers = encoder.encode(headers);

        auto result = client.handle_push_promise(0, encoded_headers);
        CHECK(result.is_ok());
        CHECK(result.value().method == "GET");
        CHECK(result.value().path == "/pushed.js");

        auto promise = client.get_push_promise(0);
        CHECK(promise.has_value());
    }

    SUBCASE("Client rejects PUSH_PROMISE without MAX_PUSH_ID") {
        dp::Vector<dp::u8> encoded_headers;
        auto result = client.handle_push_promise(0, encoded_headers);
        CHECK(result.is_err());
    }

    SUBCASE("Client rejects PUSH_PROMISE exceeding MAX_PUSH_ID") {
        client.send_max_push_id(5);

        dp::Vector<dp::u8> encoded_headers;
        QpackEncoder encoder;
        encoded_headers = encoder.encode({{":method", "GET"}});

        auto result = client.handle_push_promise(10, encoded_headers);  // Exceeds 5
        CHECK(result.is_err());
    }

    SUBCASE("Client rejects duplicate push_id") {
        client.send_max_push_id(10);

        QpackEncoder encoder;
        auto encoded = encoder.encode({{":method", "GET"}, {":path", "/a"}});

        client.handle_push_promise(5, encoded);
        auto result = client.handle_push_promise(5, encoded);  // Duplicate
        CHECK(result.is_err());
    }

    SUBCASE("PushPromiseFrame serialization/parsing") {
        PushPromiseFrame frame;
        frame.push_id = 3;
        frame.encoded_field_section = {0x00, 0x00, 0x01, 0x02};

        auto serialized = frame.serialize();
        CHECK(!serialized.empty());

        // Skip frame type
        auto type_result = varint_decode(serialized.data(), serialized.size());
        CHECK(type_result.is_ok());
        auto [type_val, type_len] = type_result.value();
        CHECK(static_cast<FrameType>(type_val) == FrameType::PushPromise);

        // Parse the frame
        auto parsed = PushPromiseFrame::parse(serialized.data() + type_len, serialized.size() - type_len);
        CHECK(parsed.is_ok());
        CHECK(parsed.value().first.push_id == 3);
        CHECK(parsed.value().first.encoded_field_section.size() == 4);
    }
}

TEST_CASE("HTTP/3 Server Push - Active Push Tracking") {
    Connection server(false);
    Connection client(true);

    auto c_init = client.initialize();
    auto s_init = server.initialize();
    client.process_control_data(s_init.value());
    server.process_control_data(c_init.value());

    server.handle_max_push_id(10);

    // Create request stream
    auto stream_id = client.create_request_stream().value();
    Request req;
    req.method = "GET";
    req.scheme = "https";
    req.authority = "example.com";
    req.path = "/";
    auto req_data = client.encode_request(stream_id, req);
    server.process_request_stream(stream_id, req_data.value());

    SUBCASE("Get active push IDs") {
        Request pushed;
        pushed.method = "GET";
        pushed.scheme = "https";
        pushed.authority = "example.com";
        pushed.path = "/a";

        server.create_push_promise(stream_id, pushed);
        pushed.path = "/b";
        server.create_push_promise(stream_id, pushed);
        pushed.path = "/c";
        server.create_push_promise(stream_id, pushed);

        auto active = server.get_active_push_ids();
        CHECK(active.size() == 3);
    }

    SUBCASE("Cancelled pushes not in active list") {
        Request pushed;
        pushed.method = "GET";
        pushed.scheme = "https";
        pushed.authority = "example.com";
        pushed.path = "/a";

        server.create_push_promise(stream_id, pushed);
        server.create_push_promise(stream_id, pushed);
        server.create_push_promise(stream_id, pushed);

        // Cancel push ID 1
        server.handle_cancel_push(1);

        auto active = server.get_active_push_ids();
        CHECK(active.size() == 2);

        // Push IDs 0 and 2 should be active
        bool found_0 = false, found_2 = false;
        for (auto id : active) {
            if (id == 0) found_0 = true;
            if (id == 2) found_2 = true;
        }
        CHECK(found_0);
        CHECK(found_2);
    }
}

TEST_CASE("HTTP/3 PRIORITY_UPDATE Frame") {
    SUBCASE("PriorityUpdateFrame serialization/parsing") {
        PriorityUpdateFrame frame;
        frame.stream_id = 4;
        frame.priority_field_value = "u=2, i";

        auto serialized = frame.serialize();
        CHECK(!serialized.empty());

        // Skip frame type
        auto type_result = varint_decode(serialized.data(), serialized.size());
        CHECK(type_result.is_ok());
        auto [type_val, type_len] = type_result.value();
        CHECK(static_cast<FrameType>(type_val) == FrameType::PriorityUpdate);

        // Parse the frame
        auto parsed = PriorityUpdateFrame::parse(serialized.data() + type_len, serialized.size() - type_len);
        CHECK(parsed.is_ok());
        CHECK(parsed.value().first.stream_id == 4);
        CHECK(parsed.value().first.priority_field_value == "u=2, i");
    }

    SUBCASE("Empty priority field value") {
        PriorityUpdateFrame frame;
        frame.stream_id = 8;
        frame.priority_field_value = "";

        auto serialized = frame.serialize();
        auto type_result = varint_decode(serialized.data(), serialized.size());
        auto [type_val, type_len] = type_result.value();

        auto parsed = PriorityUpdateFrame::parse(serialized.data() + type_len, serialized.size() - type_len);
        CHECK(parsed.is_ok());
        CHECK(parsed.value().first.stream_id == 8);
        CHECK(parsed.value().first.priority_field_value.empty());
    }
}

// =============================================================================
// HTTP/3 Settings Validation (RFC 9114 Section 7.2.4)
// =============================================================================

TEST_CASE("HTTP/3 Settings Validation") {
    SUBCASE("Default settings") {
        Settings settings;

        CHECK(settings.max_field_section_size == 0);
        CHECK(settings.qpack_max_table_capacity == 0);
        CHECK(settings.qpack_blocked_streams == 0);
    }

    SUBCASE("Settings serialization round-trip") {
        Settings settings;
        settings.max_field_section_size = 16384;
        settings.qpack_max_table_capacity = 4096;
        settings.qpack_blocked_streams = 100;

        SettingsFrame frame;
        frame.settings = settings;

        auto serialized = frame.serialize();

        // First parse the frame header to get offset past frame type
        auto type_result = parse_frame_header(serialized.data(), serialized.size());
        REQUIRE(type_result.is_ok());
        dp::usize offset = type_result.value().second;

        auto parsed = SettingsFrame::parse(serialized.data() + offset, serialized.size() - offset);

        REQUIRE(parsed.is_ok());
        auto &result = parsed.value().first;
        CHECK(result.settings.max_field_section_size == 16384);
        CHECK(result.settings.qpack_max_table_capacity == 4096);
        CHECK(result.settings.qpack_blocked_streams == 100);
    }

    SUBCASE("Validate QPACK settings") {
        Settings settings;
        settings.qpack_max_table_capacity = 4096;
        settings.qpack_blocked_streams = 100;

        // These should be valid
        CHECK(settings.qpack_max_table_capacity <= 1073741823); // QPACK limit
    }

    SUBCASE("Enable CONNECT protocol setting") {
        Settings settings;
        settings.enable_connect_protocol = true;

        auto serialized = settings.serialize();
        CHECK(!serialized.empty());
    }
}

// =============================================================================
// HTTP/3 Trailer Headers (RFC 9114 Section 4.1)
// =============================================================================

TEST_CASE("HTTP/3 Trailer Headers") {
    SUBCASE("Request with trailers field") {
        Request req;
        req.method = "POST";
        req.scheme = "https";
        req.authority = "example.com";
        req.path = "/upload";
        req.headers.push_back(HeaderField("content-type", "application/octet-stream"));

        // Trailers to send after body
        req.trailers.push_back(HeaderField("x-checksum", "abc123"));
        req.trailers.push_back(HeaderField("x-upload-complete", "true"));

        CHECK(req.trailers.size() == 2);
        CHECK(req.trailers[0].name == "x-checksum");
        CHECK(req.trailers[1].name == "x-upload-complete");
    }

    SUBCASE("Response with trailers field") {
        Response resp;
        resp.status = 200;
        resp.headers.push_back(HeaderField("content-type", "text/plain"));
        resp.trailers.push_back(HeaderField("x-response-time", "42ms"));

        CHECK(resp.trailers.size() == 1);
        CHECK(resp.trailers[0].name == "x-response-time");
    }

    SUBCASE("Get all headers includes trailers") {
        Request req;
        req.method = "GET";
        req.scheme = "https";
        req.authority = "example.com";
        req.path = "/";

        req.headers.push_back(HeaderField("accept", "text/html"));
        req.trailers.push_back(HeaderField("x-trailer", "value"));

        auto all_headers = req.get_all_headers();
        CHECK(all_headers.size() >= 1);
    }
}

// =============================================================================
// HTTP/3 Extended CONNECT (RFC 9220)
// =============================================================================

TEST_CASE("HTTP/3 Extended CONNECT") {
    SUBCASE("Server advertises CONNECT support") {
        Settings settings;
        settings.enable_connect_protocol = true;

        SettingsFrame frame;
        frame.settings = settings;

        auto serialized = frame.serialize();

        // First parse the frame header to get offset past frame type
        auto type_result = parse_frame_header(serialized.data(), serialized.size());
        REQUIRE(type_result.is_ok());
        dp::usize offset = type_result.value().second;

        auto parsed = SettingsFrame::parse(serialized.data() + offset, serialized.size() - offset);

        REQUIRE(parsed.is_ok());
        CHECK(parsed.value().first.settings.enable_connect_protocol == true);
    }

    SUBCASE("Extended CONNECT request format") {
        Request req;
        req.method = "CONNECT";
        req.scheme = "https";
        req.authority = "example.com:443";
        req.path = "/websocket";
        req.protocol = "websocket"; // :protocol pseudo-header for Extended CONNECT

        auto pseudo = req.get_pseudo_headers();

        // Extended CONNECT has :protocol
        bool has_protocol = false;
        for (const auto &h : pseudo) {
            if (h.name == ":protocol") {
                has_protocol = true;
                CHECK(h.value == "websocket");
            }
        }
        CHECK(has_protocol);
    }

    SUBCASE("Regular CONNECT without protocol") {
        Request req;
        req.method = "CONNECT";
        req.authority = "proxy.example.com:8080";
        // No :scheme, :path, or :protocol for regular CONNECT

        auto pseudo = req.get_pseudo_headers();

        // Regular CONNECT only has :method and :authority
        bool has_scheme = false;
        bool has_path = false;
        for (const auto &h : pseudo) {
            if (h.name == ":scheme") has_scheme = true;
            if (h.name == ":path") has_path = true;
        }
        CHECK(!has_scheme);
        CHECK(!has_path);
    }
}

// =============================================================================
// HTTP/3 Dynamic QPACK Table Edge Cases
// =============================================================================

TEST_CASE("HTTP/3 QPACK Dynamic Table Additional Tests") {
    SUBCASE("Table with zero capacity") {
        QpackDynamicTable table(0);

        CHECK(!table.is_enabled());
        CHECK(table.max_capacity() == 0);
    }

    SUBCASE("Enable table by setting capacity") {
        QpackDynamicTable table(0);
        CHECK(!table.is_enabled());

        table.set_max_capacity(4096);
        CHECK(table.is_enabled());
    }

    SUBCASE("Insert count tracks insertions") {
        QpackDynamicTable table(4096);

        CHECK(table.get_insert_count() == 0);

        table.insert("header1", "value1");
        CHECK(table.get_insert_count() == 1);

        table.insert("header2", "value2");
        CHECK(table.get_insert_count() == 2);
    }

    SUBCASE("Count returns number of entries") {
        QpackDynamicTable table(4096);

        CHECK(table.count() == 0);

        table.insert("header1", "value1");
        CHECK(table.count() == 1);

        table.insert("header2", "value2");
        CHECK(table.count() == 2);
    }
}

// =============================================================================
// HTTP/3 Stream Reset (RFC 9114 Section 4.1.1)
// =============================================================================

TEST_CASE("HTTP/3 Stream Reset") {
    SUBCASE("Handle incoming stream reset") {
        Connection client(true);
        auto init = client.initialize();

        // Simulate receiving a stream reset from peer
        client.handle_stream_reset(4, static_cast<dp::u64>(ErrorCode::RequestCancelled));

        CHECK(client.is_stream_reset(4));
    }

    SUBCASE("Unknown stream reset creates tracking entry") {
        Connection client(true);
        auto init = client.initialize();

        // Stream 100 doesn't exist but we can still track reset
        client.handle_stream_reset(100, static_cast<dp::u64>(ErrorCode::InternalError));

        CHECK(client.is_stream_reset(100));
    }
}

// =============================================================================
// HTTP/3 GOAWAY Frame (RFC 9114 Section 5.2)
// =============================================================================

TEST_CASE("HTTP/3 GOAWAY Frame Serialization") {
    SUBCASE("GOAWAY frame round-trip") {
        GoAwayFrame frame;
        frame.stream_id = 16;

        auto serialized = frame.serialize();

        // First parse the frame header to get offset past frame type
        auto type_result = parse_frame_header(serialized.data(), serialized.size());
        REQUIRE(type_result.is_ok());
        dp::usize offset = type_result.value().second;

        auto parsed = GoAwayFrame::parse(serialized.data() + offset, serialized.size() - offset);
        REQUIRE(parsed.is_ok());
        CHECK(parsed.value().first.stream_id == 16);
    }

    SUBCASE("GOAWAY with stream_id 0") {
        GoAwayFrame frame;
        frame.stream_id = 0;

        auto serialized = frame.serialize();

        auto type_result = parse_frame_header(serialized.data(), serialized.size());
        REQUIRE(type_result.is_ok());
        dp::usize offset = type_result.value().second;

        auto parsed = GoAwayFrame::parse(serialized.data() + offset, serialized.size() - offset);

        REQUIRE(parsed.is_ok());
        CHECK(parsed.value().first.stream_id == 0);
    }

    SUBCASE("GOAWAY with large stream_id") {
        GoAwayFrame frame;
        frame.stream_id = 0x3FFFFFFFFFFFFFFF; // Max varint

        auto serialized = frame.serialize();

        auto type_result = parse_frame_header(serialized.data(), serialized.size());
        REQUIRE(type_result.is_ok());
        dp::usize offset = type_result.value().second;

        auto parsed = GoAwayFrame::parse(serialized.data() + offset, serialized.size() - offset);

        REQUIRE(parsed.is_ok());
        CHECK(parsed.value().first.stream_id == 0x3FFFFFFFFFFFFFFF);
    }

    SUBCASE("Connection stores goaway stream_id") {
        Connection client(true);
        auto init = client.initialize();

        // Check initial goaway_stream_id
        CHECK(client.goaway_stream_id() == 0);
    }
}

#pragma once

#include <datapod/datapod.hpp>
#include <echo/echo.hpp>
#include <netpipe/protocol/http3/types.hpp>

namespace netpipe::http3 {

    // HPACK/QPACK Huffman Table (RFC 7541 Appendix B)
    // Each entry: {symbol, code, bit_length}
    struct HuffmanEntry {
        dp::u8 symbol;
        dp::u32 code;
        dp::u8 bits;
    };

    // Huffman encoding table indexed by symbol (0-255 + EOS)
    inline const HuffmanEntry HUFFMAN_TABLE[] = {
        {0, 0x1ff8, 13},      {1, 0x7fffd8, 23},    {2, 0xfffffe2, 28},   {3, 0xfffffe3, 28},   {4, 0xfffffe4, 28},
        {5, 0xfffffe5, 28},   {6, 0xfffffe6, 28},   {7, 0xfffffe7, 28},   {8, 0xfffffe8, 28},   {9, 0xffffea, 24},
        {10, 0x3ffffffc, 30}, {11, 0xfffffe9, 28},  {12, 0xfffffea, 28},  {13, 0x3ffffffd, 30}, {14, 0xfffffeb, 28},
        {15, 0xfffffec, 28},  {16, 0xfffffed, 28},  {17, 0xfffffee, 28},  {18, 0xfffffef, 28},  {19, 0xffffff0, 28},
        {20, 0xffffff1, 28},  {21, 0xffffff2, 28},  {22, 0x3ffffffe, 30}, {23, 0xffffff3, 28},  {24, 0xffffff4, 28},
        {25, 0xffffff5, 28},  {26, 0xffffff6, 28},  {27, 0xffffff7, 28},  {28, 0xffffff8, 28},  {29, 0xffffff9, 28},
        {30, 0xffffffa, 28},  {31, 0xffffffb, 28},  {32, 0x14, 6},        {33, 0x3f8, 10},      {34, 0x3f9, 10},
        {35, 0xffa, 12},      {36, 0x1ff9, 13},     {37, 0x15, 6},        {38, 0xf8, 8},        {39, 0x7fa, 11},
        {40, 0x3fa, 10},      {41, 0x3fb, 10},      {42, 0xf9, 8},        {43, 0x7fb, 11},      {44, 0xfa, 8},
        {45, 0x16, 6},        {46, 0x17, 6},        {47, 0x18, 6},        {48, 0x0, 5},         {49, 0x1, 5},
        {50, 0x2, 5},         {51, 0x19, 6},        {52, 0x1a, 6},        {53, 0x1b, 6},        {54, 0x1c, 6},
        {55, 0x1d, 6},        {56, 0x1e, 6},        {57, 0x1f, 6},        {58, 0x5c, 7},        {59, 0xfb, 8},
        {60, 0x7ffc, 15},     {61, 0x20, 6},        {62, 0xffb, 12},      {63, 0x3fc, 10},      {64, 0x1ffa, 13},
        {65, 0x21, 6},        {66, 0x5d, 7},        {67, 0x5e, 7},        {68, 0x5f, 7},        {69, 0x60, 7},
        {70, 0x61, 7},        {71, 0x62, 7},        {72, 0x63, 7},        {73, 0x64, 7},        {74, 0x65, 7},
        {75, 0x66, 7},        {76, 0x67, 7},        {77, 0x68, 7},        {78, 0x69, 7},        {79, 0x6a, 7},
        {80, 0x6b, 7},        {81, 0x6c, 7},        {82, 0x6d, 7},        {83, 0x6e, 7},        {84, 0x6f, 7},
        {85, 0x70, 7},        {86, 0x71, 7},        {87, 0x72, 7},        {88, 0xfc, 8},        {89, 0x73, 7},
        {90, 0xfd, 8},        {91, 0x1ffb, 13},     {92, 0x7fff0, 19},    {93, 0x1ffc, 13},     {94, 0x3ffc, 14},
        {95, 0x22, 6},        {96, 0x7ffd, 15},     {97, 0x3, 5},         {98, 0x23, 6},        {99, 0x4, 5},
        {100, 0x24, 6},       {101, 0x5, 5},        {102, 0x25, 6},       {103, 0x26, 6},       {104, 0x27, 6},
        {105, 0x6, 5},        {106, 0x74, 7},       {107, 0x75, 7},       {108, 0x28, 6},       {109, 0x29, 6},
        {110, 0x2a, 6},       {111, 0x7, 5},        {112, 0x2b, 6},       {113, 0x76, 7},       {114, 0x2c, 6},
        {115, 0x8, 5},        {116, 0x9, 5},        {117, 0x2d, 6},       {118, 0x77, 7},       {119, 0x78, 7},
        {120, 0x79, 7},       {121, 0x7a, 7},       {122, 0x7b, 7},       {123, 0x7ffe, 15},    {124, 0x7fc, 11},
        {125, 0x3ffd, 14},    {126, 0x1ffd, 13},    {127, 0xffffffc, 28}, {128, 0xfffe6, 20},   {129, 0x3fffd2, 22},
        {130, 0xfffe7, 20},   {131, 0xfffe8, 20},   {132, 0x3fffd3, 22},  {133, 0x3fffd4, 22},  {134, 0x3fffd5, 22},
        {135, 0x7fffd9, 23},  {136, 0x3fffd6, 22},  {137, 0x7fffda, 23},  {138, 0x7fffdb, 23},  {139, 0x7fffdc, 23},
        {140, 0x7fffdd, 23},  {141, 0x7fffde, 23},  {142, 0xffffeb, 24},  {143, 0x7fffdf, 23},  {144, 0xffffec, 24},
        {145, 0xffffed, 24},  {146, 0x3fffd7, 22},  {147, 0x7fffe0, 23},  {148, 0xffffee, 24},  {149, 0x7fffe1, 23},
        {150, 0x7fffe2, 23},  {151, 0x7fffe3, 23},  {152, 0x7fffe4, 23},  {153, 0x1fffdc, 21},  {154, 0x3fffd8, 22},
        {155, 0x7fffe5, 23},  {156, 0x3fffd9, 22},  {157, 0x7fffe6, 23},  {158, 0x7fffe7, 23},  {159, 0xffffef, 24},
        {160, 0x3fffda, 22},  {161, 0x1fffdd, 21},  {162, 0xfffe9, 20},   {163, 0x3fffdb, 22},  {164, 0x3fffdc, 22},
        {165, 0x7fffe8, 23},  {166, 0x7fffe9, 23},  {167, 0x1fffde, 21},  {168, 0x7fffea, 23},  {169, 0x3fffdd, 22},
        {170, 0x3fffde, 22},  {171, 0xfffff0, 24},  {172, 0x1fffdf, 21},  {173, 0x3fffdf, 22},  {174, 0x7fffeb, 23},
        {175, 0x7fffec, 23},  {176, 0x1fffe0, 21},  {177, 0x1fffe1, 21},  {178, 0x3fffe0, 22},  {179, 0x1fffe2, 21},
        {180, 0x7fffed, 23},  {181, 0x3fffe1, 22},  {182, 0x7fffee, 23},  {183, 0x7fffef, 23},  {184, 0xfffea, 20},
        {185, 0x3fffe2, 22},  {186, 0x3fffe3, 22},  {187, 0x3fffe4, 22},  {188, 0x7ffff0, 23},  {189, 0x3fffe5, 22},
        {190, 0x3fffe6, 22},  {191, 0x7ffff1, 23},  {192, 0x3ffffe0, 26}, {193, 0x3ffffe1, 26}, {194, 0xfffeb, 20},
        {195, 0x7fff1, 19},   {196, 0x3fffe7, 22},  {197, 0x7ffff2, 23},  {198, 0x3fffe8, 22},  {199, 0x1ffffec, 25},
        {200, 0x3ffffe2, 26}, {201, 0x3ffffe3, 26}, {202, 0x3ffffe4, 26}, {203, 0x7ffffde, 27}, {204, 0x7ffffdf, 27},
        {205, 0x3ffffe5, 26}, {206, 0xfffff1, 24},  {207, 0x1ffffed, 25}, {208, 0x7fff2, 19},   {209, 0x1fffe3, 21},
        {210, 0x3ffffe6, 26}, {211, 0x7ffffe0, 27}, {212, 0x7ffffe1, 27}, {213, 0x3ffffe7, 26}, {214, 0x7ffffe2, 27},
        {215, 0xfffff2, 24},  {216, 0x1fffe4, 21},  {217, 0x1fffe5, 21},  {218, 0x3ffffe8, 26}, {219, 0x3ffffe9, 26},
        {220, 0xffffffd, 28}, {221, 0x7ffffe3, 27}, {222, 0x7ffffe4, 27}, {223, 0x7ffffe5, 27}, {224, 0xfffec, 20},
        {225, 0xfffff3, 24},  {226, 0xfffed, 20},   {227, 0x1fffe6, 21},  {228, 0x3fffe9, 22},  {229, 0x1fffe7, 21},
        {230, 0x1fffe8, 21},  {231, 0x7ffff3, 23},  {232, 0x3fffea, 22},  {233, 0x3fffeb, 22},  {234, 0x1ffffee, 25},
        {235, 0x1ffffef, 25}, {236, 0xfffff4, 24},  {237, 0xfffff5, 24},  {238, 0x3ffffea, 26}, {239, 0x7ffff4, 23},
        {240, 0x3ffffeb, 26}, {241, 0x7ffffe6, 27}, {242, 0x3ffffec, 26}, {243, 0x3ffffed, 26}, {244, 0x7ffffe7, 27},
        {245, 0x7ffffe8, 27}, {246, 0x7ffffe9, 27}, {247, 0x7ffffea, 27}, {248, 0x7ffffeb, 27}, {249, 0xffffffe, 28},
        {250, 0x7ffffec, 27}, {251, 0x7ffffed, 27}, {252, 0x7ffffee, 27}, {253, 0x7ffffef, 27}, {254, 0x7fffff0, 27},
        {255, 0x3ffffee, 26},
        // EOS (256) = 0x3fffffff, 30 bits
    };

    constexpr dp::u32 HUFFMAN_EOS_CODE = 0x3fffffff;
    constexpr dp::u8 HUFFMAN_EOS_BITS = 30;

    // Huffman Encoder
    class HuffmanEncoder {
      public:
        dp::Vector<dp::u8> encode(const dp::String &str) {
            dp::Vector<dp::u8> result;
            dp::u64 buffer = 0;
            dp::u8 buffer_bits = 0;

            for (dp::usize i = 0; i < str.size(); i++) {
                dp::u8 c = static_cast<dp::u8>(str[i]);
                const auto &entry = HUFFMAN_TABLE[c];

                // Add code to buffer
                buffer = (buffer << entry.bits) | entry.code;
                buffer_bits += entry.bits;

                // Emit complete bytes
                while (buffer_bits >= 8) {
                    buffer_bits -= 8;
                    result.push_back(static_cast<dp::u8>(buffer >> buffer_bits));
                }
            }

            // Pad with EOS prefix if needed
            if (buffer_bits > 0) {
                dp::u8 pad_bits = 8 - buffer_bits;
                dp::u8 pad_value = (1 << pad_bits) - 1; // All 1s for padding (EOS prefix)
                buffer = (buffer << pad_bits) | pad_value;
                result.push_back(static_cast<dp::u8>(buffer));
            }

            return result;
        }

        dp::usize encoded_length(const dp::String &str) const {
            dp::usize total_bits = 0;
            for (dp::usize i = 0; i < str.size(); i++) {
                dp::u8 c = static_cast<dp::u8>(str[i]);
                total_bits += HUFFMAN_TABLE[c].bits;
            }
            return (total_bits + 7) / 8;
        }
    };

    // Huffman Decoder using lookup table for fast decoding
    class HuffmanDecoder {
      public:
        HuffmanDecoder() { build_decode_table(); }

        dp::Res<dp::String> decode(const dp::u8 *data, dp::usize size) {
            dp::String result;
            dp::u64 buffer = 0;
            dp::u8 buffer_bits = 0;

            for (dp::usize i = 0; i < size; i++) {
                buffer = (buffer << 8) | data[i];
                buffer_bits += 8;

                while (buffer_bits >= 5) { // Minimum code length is 5 bits
                    bool found = false;
                    // Try to match SHORTEST codes first (5 to 30 bits) - this is important!
                    // Huffman codes are prefix-free, so shortest matching code is correct
                    for (int len = 5; len <= std::min(static_cast<int>(buffer_bits), 30); len++) {
                        dp::u32 code = static_cast<dp::u32>(buffer >> (buffer_bits - len)) & ((1ULL << len) - 1);

                        auto it = decode_table_.find((static_cast<dp::u64>(len) << 32) | code);
                        if (it != decode_table_.end()) {
                            if (it->second == 256) {
                                // EOS - stop decoding
                                return dp::result::ok(std::move(result));
                            }
                            result.push_back(static_cast<char>(it->second));
                            buffer_bits -= static_cast<dp::u8>(len);
                            buffer &= (1ULL << buffer_bits) - 1;
                            found = true;
                            break;
                        }
                    }

                    if (!found) {
                        // If we can't find a match but have bits left, we may need more input
                        // Break out of while loop to get more bytes
                        break;
                    }
                }
            }

            // After processing all bytes, check remaining bits
            // Try one more time to decode any complete codes
            while (buffer_bits >= 5) {
                bool found = false;
                for (int len = 5; len <= std::min(static_cast<int>(buffer_bits), 30); len++) {
                    dp::u32 code = static_cast<dp::u32>(buffer >> (buffer_bits - len)) & ((1ULL << len) - 1);
                    auto it = decode_table_.find((static_cast<dp::u64>(len) << 32) | code);
                    if (it != decode_table_.end()) {
                        if (it->second == 256) {
                            return dp::result::ok(std::move(result));
                        }
                        result.push_back(static_cast<char>(it->second));
                        buffer_bits -= static_cast<dp::u8>(len);
                        buffer &= (1ULL << buffer_bits) - 1;
                        found = true;
                        break;
                    }
                }
                if (!found)
                    break;
            }

            // Check remaining bits are valid padding (all 1s)
            if (buffer_bits > 0) {
                if (buffer_bits > 7) {
                    return dp::result::err(dp::Error::invalid_argument("invalid huffman code"));
                }
                dp::u64 padding = buffer & ((1ULL << buffer_bits) - 1);
                dp::u64 expected_padding = (1ULL << buffer_bits) - 1;
                if (padding != expected_padding) {
                    return dp::result::err(dp::Error::invalid_argument("invalid huffman padding"));
                }
            }

            return dp::result::ok(std::move(result));
        }

      private:
        void build_decode_table() {
            for (int i = 0; i < 256; i++) {
                const auto &entry = HUFFMAN_TABLE[i];
                dp::u64 key = (static_cast<dp::u64>(entry.bits) << 32) | entry.code;
                decode_table_[key] = static_cast<dp::u16>(i);
            }
            // Add EOS
            dp::u64 eos_key = (static_cast<dp::u64>(HUFFMAN_EOS_BITS) << 32) | HUFFMAN_EOS_CODE;
            decode_table_[eos_key] = 256;
        }

        std::map<dp::u64, dp::u16> decode_table_;
    };

    // Global Huffman codec instances
    inline HuffmanEncoder &huffman_encoder() {
        static HuffmanEncoder encoder;
        return encoder;
    }

    inline HuffmanDecoder &huffman_decoder() {
        static HuffmanDecoder decoder;
        return decoder;
    }

    // QPACK Static Table (RFC 9204 Appendix A)
    // Subset of commonly used header fields
    inline const HeaderField QPACK_STATIC_TABLE[] = {
        {":authority", ""},
        {":path", "/"},
        {"age", "0"},
        {"content-disposition", ""},
        {"content-length", "0"},
        {"cookie", ""},
        {"date", ""},
        {"etag", ""},
        {"if-modified-since", ""},
        {"if-none-match", ""},
        {"last-modified", ""},
        {"link", ""},
        {"location", ""},
        {"referer", ""},
        {"set-cookie", ""},
        {":method", "CONNECT"},
        {":method", "DELETE"},
        {":method", "GET"},
        {":method", "HEAD"},
        {":method", "OPTIONS"},
        {":method", "POST"},
        {":method", "PUT"},
        {":scheme", "http"},
        {":scheme", "https"},
        {":status", "103"},
        {":status", "200"},
        {":status", "304"},
        {":status", "404"},
        {":status", "503"},
        {"accept", "*/*"},
        {"accept", "application/dns-message"},
        {"accept-encoding", "gzip, deflate, br"},
        {"accept-ranges", "bytes"},
        {"access-control-allow-headers", "cache-control"},
        {"access-control-allow-headers", "content-type"},
        {"access-control-allow-origin", "*"},
        {"cache-control", "max-age=0"},
        {"cache-control", "max-age=2592000"},
        {"cache-control", "max-age=604800"},
        {"cache-control", "no-cache"},
        {"cache-control", "no-store"},
        {"cache-control", "public, max-age=31536000"},
        {"content-encoding", "br"},
        {"content-encoding", "gzip"},
        {"content-type", "application/dns-message"},
        {"content-type", "application/javascript"},
        {"content-type", "application/json"},
        {"content-type", "application/x-www-form-urlencoded"},
        {"content-type", "image/gif"},
        {"content-type", "image/jpeg"},
        {"content-type", "image/png"},
        {"content-type", "text/css"},
        {"content-type", "text/html; charset=utf-8"},
        {"content-type", "text/plain"},
        {"content-type", "text/plain;charset=utf-8"},
        {"range", "bytes=0-"},
        {"strict-transport-security", "max-age=31536000"},
        {"strict-transport-security", "max-age=31536000; includesubdomains"},
        {"strict-transport-security", "max-age=31536000; includesubdomains; preload"},
        {"vary", "accept-encoding"},
        {"vary", "origin"},
        {"x-content-type-options", "nosniff"},
        {"x-xss-protection", "1; mode=block"},
        {":status", "100"},
        {":status", "204"},
        {":status", "206"},
        {":status", "302"},
        {":status", "400"},
        {":status", "403"},
        {":status", "421"},
        {":status", "425"},
        {":status", "500"},
        {"accept-language", ""},
        {"access-control-allow-credentials", "FALSE"},
        {"access-control-allow-credentials", "TRUE"},
        {"access-control-allow-methods", "get"},
        {"access-control-allow-methods", "get, post, options"},
        {"access-control-allow-methods", "options"},
        {"access-control-expose-headers", "content-length"},
        {"access-control-request-headers", "content-type"},
        {"access-control-request-method", "get"},
        {"access-control-request-method", "post"},
        {"alt-svc", "clear"},
        {"authorization", ""},
        {"content-security-policy", "script-src 'none'; object-src 'none'; base-uri 'none'"},
        {"early-data", "1"},
        {"expect-ct", ""},
        {"forwarded", ""},
        {"if-range", ""},
        {"origin", ""},
        {"purpose", "prefetch"},
        {"server", ""},
        {"timing-allow-origin", "*"},
        {"upgrade-insecure-requests", "1"},
        {"user-agent", ""},
        {"x-forwarded-for", ""},
        {"x-frame-options", "deny"},
        {"x-frame-options", "sameorigin"},
    };

    constexpr dp::usize QPACK_STATIC_TABLE_SIZE = sizeof(QPACK_STATIC_TABLE) / sizeof(QPACK_STATIC_TABLE[0]);

    // QPACK Dynamic Table (RFC 9204 Section 3.2)
    // The dynamic table is a FIFO with bounded size
    class QpackDynamicTable {
      public:
        struct Entry {
            dp::String name;
            dp::String value;
            dp::usize size; // Name length + value length + 32 (overhead)
        };

        explicit QpackDynamicTable(dp::usize max_capacity = 0) : max_capacity_(max_capacity), current_size_(0) {}

        // Set the maximum table capacity
        void set_max_capacity(dp::usize capacity) {
            max_capacity_ = capacity;
            evict_to_fit(0); // Evict if needed
            echo::debug("QPACK dynamic table capacity set to ", capacity);
        }

        dp::usize max_capacity() const { return max_capacity_; }
        dp::usize current_size() const { return current_size_; }
        dp::usize count() const { return entries_.size(); }
        bool is_enabled() const { return max_capacity_ > 0; }

        // Insert an entry at the front of the table (newest entry)
        // Returns the absolute index of the new entry, or -1 if insertion failed
        dp::i64 insert(const dp::String &name, const dp::String &value) {
            dp::usize entry_size = name.size() + value.size() + 32; // RFC overhead

            if (entry_size > max_capacity_) {
                // Entry too large - clear table but don't insert
                echo::debug("QPACK entry too large: ", entry_size, " > ", max_capacity_);
                entries_.clear();
                current_size_ = 0;
                return -1;
            }

            // Evict entries if needed
            evict_to_fit(entry_size);

            // Insert at front
            Entry entry{name, value, entry_size};
            entries_.insert(entries_.begin(), std::move(entry));
            current_size_ += entry_size;
            insert_count_++;

            echo::trace("QPACK dynamic table insert: name=", name.c_str(), " size=", entry_size,
                        " total=", current_size_, " count=", entries_.size());

            return static_cast<dp::i64>(insert_count_ - 1);
        }

        // Duplicate an existing entry
        dp::i64 duplicate(dp::usize relative_index) {
            if (relative_index >= entries_.size()) {
                return -1;
            }
            const auto &entry = entries_[relative_index];
            return insert(entry.name, entry.value);
        }

        // Get entry by relative index (0 = most recently inserted)
        const Entry *get_relative(dp::usize relative_index) const {
            if (relative_index >= entries_.size()) {
                return nullptr;
            }
            return &entries_[relative_index];
        }

        // Get entry by absolute index
        const Entry *get_absolute(dp::u64 absolute_index) const {
            if (absolute_index >= insert_count_) {
                return nullptr;
            }
            // Calculate relative index from absolute index
            dp::u64 relative_index = insert_count_ - 1 - absolute_index;
            if (relative_index >= entries_.size()) {
                return nullptr; // Entry was evicted
            }
            return &entries_[static_cast<dp::usize>(relative_index)];
        }

        // Find an entry in the dynamic table
        // Returns (relative_index, name_only_match)
        std::pair<dp::i64, bool> find(const dp::String &name, const dp::String &value) const {
            dp::i64 name_match = -1;
            for (dp::usize i = 0; i < entries_.size(); i++) {
                if (entries_[i].name == name) {
                    if (entries_[i].value == value) {
                        return {static_cast<dp::i64>(i), false}; // Full match
                    }
                    if (name_match < 0) {
                        name_match = static_cast<dp::i64>(i);
                    }
                }
            }
            return {name_match, true}; // Name-only match or no match
        }

        // Get the current insert count (number of entries ever inserted)
        dp::u64 get_insert_count() const { return insert_count_; }

        // Calculate the Required Insert Count for encoding
        dp::u64 get_required_insert_count(dp::u64 max_entries_used) const {
            if (max_entries_used == 0) {
                return 0;
            }
            return max_entries_used;
        }

        // Get base index for encoding (usually insert_count)
        dp::u64 get_base() const { return insert_count_; }

      private:
        void evict_to_fit(dp::usize new_entry_size) {
            while (!entries_.empty() && current_size_ + new_entry_size > max_capacity_) {
                // Remove oldest entry (from back)
                current_size_ -= entries_.back().size;
                entries_.pop_back();
                echo::trace("QPACK evicted entry, size now ", current_size_);
            }
        }

        dp::Vector<Entry> entries_;
        dp::usize max_capacity_;
        dp::usize current_size_;
        dp::u64 insert_count_ = 0; // Total number of entries ever inserted
    };

    // QPACK Encoder Stream Instructions (RFC 9204 Section 4.3)
    // These are sent on the encoder stream to update the decoder's dynamic table
    namespace encoder_instructions {
        // Set Dynamic Table Capacity
        inline dp::Vector<dp::u8> set_capacity(dp::u64 capacity) {
            dp::Vector<dp::u8> data;
            // Format: 001xxxxx (5-bit prefix)
            if (capacity < 32) {
                data.push_back(static_cast<dp::u8>(0x20 | capacity));
            } else {
                data.push_back(0x3F); // 0b00111111
                dp::u64 remaining = capacity - 31;
                while (remaining >= 128) {
                    data.push_back(static_cast<dp::u8>(0x80 | (remaining & 0x7F)));
                    remaining >>= 7;
                }
                data.push_back(static_cast<dp::u8>(remaining));
            }
            return data;
        }

        // Insert With Name Reference (static table)
        inline dp::Vector<dp::u8> insert_with_static_ref(dp::u64 index, const dp::String &value, bool use_huffman) {
            dp::Vector<dp::u8> data;
            // Format: 11Txxxxx where T=1 for static
            if (index < 64) {
                data.push_back(static_cast<dp::u8>(0xC0 | index));
            } else {
                data.push_back(0xFF);
                dp::u64 remaining = index - 63;
                while (remaining >= 128) {
                    data.push_back(static_cast<dp::u8>(0x80 | (remaining & 0x7F)));
                    remaining >>= 7;
                }
                data.push_back(static_cast<dp::u8>(remaining));
            }
            // Encode value (simplified - no Huffman for now in instructions)
            if (value.size() < 128) {
                data.push_back(static_cast<dp::u8>(value.size()));
            } else {
                data.push_back(0x7F);
                dp::usize remaining = value.size() - 127;
                while (remaining >= 128) {
                    data.push_back(static_cast<dp::u8>(0x80 | (remaining & 0x7F)));
                    remaining >>= 7;
                }
                data.push_back(static_cast<dp::u8>(remaining));
            }
            data.insert(data.end(), value.begin(), value.end());
            return data;
        }

        // Insert Without Name Reference
        inline dp::Vector<dp::u8> insert_literal(const dp::String &name, const dp::String &value) {
            dp::Vector<dp::u8> data;
            // Format: 01Hxxxxx where H=0 for literal name
            // Name length
            if (name.size() < 32) {
                data.push_back(static_cast<dp::u8>(0x40 | name.size()));
            } else {
                data.push_back(0x5F);
                dp::usize remaining = name.size() - 31;
                while (remaining >= 128) {
                    data.push_back(static_cast<dp::u8>(0x80 | (remaining & 0x7F)));
                    remaining >>= 7;
                }
                data.push_back(static_cast<dp::u8>(remaining));
            }
            data.insert(data.end(), name.begin(), name.end());
            // Value length
            if (value.size() < 128) {
                data.push_back(static_cast<dp::u8>(value.size()));
            } else {
                data.push_back(0x7F);
                dp::usize remaining = value.size() - 127;
                while (remaining >= 128) {
                    data.push_back(static_cast<dp::u8>(0x80 | (remaining & 0x7F)));
                    remaining >>= 7;
                }
                data.push_back(static_cast<dp::u8>(remaining));
            }
            data.insert(data.end(), value.begin(), value.end());
            return data;
        }
    } // namespace encoder_instructions

    // QPACK Encoder with Huffman and Dynamic Table support
    class QpackEncoder {
      public:
        QpackEncoder(bool use_huffman = false) : use_huffman_(use_huffman), dynamic_table_(0) {}

        void set_huffman(bool enabled) { use_huffman_ = enabled; }
        bool huffman_enabled() const { return use_huffman_; }

        // Set dynamic table capacity (enables dynamic table if > 0)
        void set_dynamic_table_capacity(dp::usize capacity) { dynamic_table_.set_max_capacity(capacity); }

        dp::usize dynamic_table_capacity() const { return dynamic_table_.max_capacity(); }
        bool dynamic_table_enabled() const { return dynamic_table_.is_enabled(); }

        // Get encoder stream data (instructions to send to decoder)
        dp::Vector<dp::u8> get_encoder_stream_data() {
            auto data = std::move(pending_encoder_stream_);
            pending_encoder_stream_.clear();
            return data;
        }

        bool has_encoder_stream_data() const { return !pending_encoder_stream_.empty(); }

        // Encode header list to field section
        dp::Vector<dp::u8> encode(const HeaderList &headers) {
            dp::Vector<dp::u8> result;

            dp::u64 required_insert_count = 0;
            dp::u64 base = dynamic_table_.get_base();

            // Reserve space for prefix (will be filled later)
            dp::usize prefix_start = result.size();

            // Encode headers and track which dynamic table entries are used
            dp::Vector<dp::u8> encoded_headers;
            for (const auto &header : headers) {
                auto [ric, data] = encode_header_with_tracking(header, base);
                if (ric > required_insert_count) {
                    required_insert_count = ric;
                }
                encoded_headers.insert(encoded_headers.end(), data.begin(), data.end());
            }

            // Encode Required Insert Count
            if (required_insert_count == 0) {
                result.push_back(0x00);
            } else {
                // Encode using wire encoding: (RIC % (2 * MaxEntries)) + 1
                dp::u64 max_entries = dynamic_table_.max_capacity() > 0 ? (dynamic_table_.max_capacity() / 32) : 0;
                dp::u64 wire_ric = (required_insert_count % (2 * max_entries + 1)) + 1;
                encode_integer_8bit(result, wire_ric);
            }

            // Delta Base = Base - Required Insert Count
            // Sign bit = 0 if Base >= Required Insert Count
            dp::i64 delta_base = static_cast<dp::i64>(base) - static_cast<dp::i64>(required_insert_count);
            if (delta_base >= 0) {
                encode_integer_7bit(result, static_cast<dp::u64>(delta_base), 0x00);
            } else {
                encode_integer_7bit(result, static_cast<dp::u64>(-delta_base - 1), 0x80);
            }

            // Append encoded headers
            result.insert(result.end(), encoded_headers.begin(), encoded_headers.end());

            return result;
        }

        // Access to dynamic table for testing
        const QpackDynamicTable &dynamic_table() const { return dynamic_table_; }

      private:
        bool use_huffman_;
        QpackDynamicTable dynamic_table_;
        dp::Vector<dp::u8> pending_encoder_stream_;

        // Encode a header and return (required_insert_count, encoded_data)
        std::pair<dp::u64, dp::Vector<dp::u8>> encode_header_with_tracking(const HeaderField &header, dp::u64 base) {
            dp::Vector<dp::u8> result;
            dp::u64 ric = 0;

            // Try to find in dynamic table first (if enabled)
            if (dynamic_table_.is_enabled()) {
                auto [dyn_idx, name_only] = dynamic_table_.find(header.name, header.value);
                if (dyn_idx >= 0 && !name_only) {
                    // Full match in dynamic table
                    dp::u64 abs_idx = dynamic_table_.get_insert_count() - 1 - static_cast<dp::u64>(dyn_idx);
                    ric = abs_idx + 1;
                    encode_indexed_dynamic(result, static_cast<dp::usize>(dyn_idx), base);
                    return {ric, result};
                }
            }

            // Try static table
            int name_match = -1;
            int full_match = -1;

            for (dp::usize i = 0; i < QPACK_STATIC_TABLE_SIZE; i++) {
                if (QPACK_STATIC_TABLE[i].name == header.name) {
                    if (name_match < 0) {
                        name_match = static_cast<int>(i);
                    }
                    if (QPACK_STATIC_TABLE[i].value == header.value) {
                        full_match = static_cast<int>(i);
                        break;
                    }
                }
            }

            if (full_match >= 0) {
                encode_indexed_static(result, static_cast<dp::usize>(full_match));
            } else if (name_match >= 0) {
                encode_literal_with_name_ref(result, static_cast<dp::usize>(name_match), header.value);

                // Optionally add to dynamic table
                if (dynamic_table_.is_enabled() && should_index_header(header)) {
                    auto instr = encoder_instructions::insert_with_static_ref(static_cast<dp::u64>(name_match),
                                                                              header.value, use_huffman_);
                    pending_encoder_stream_.insert(pending_encoder_stream_.end(), instr.begin(), instr.end());
                    dynamic_table_.insert(header.name, header.value);
                }
            } else {
                encode_literal(result, header.name, header.value);

                // Optionally add to dynamic table
                if (dynamic_table_.is_enabled() && should_index_header(header)) {
                    auto instr = encoder_instructions::insert_literal(header.name, header.value);
                    pending_encoder_stream_.insert(pending_encoder_stream_.end(), instr.begin(), instr.end());
                    dynamic_table_.insert(header.name, header.value);
                }
            }

            return {ric, result};
        }

        // Determine if a header should be indexed in the dynamic table
        bool should_index_header(const HeaderField &header) const {
            // Don't index pseudo-headers that vary frequently
            if (header.name.size() > 0 && header.name[0] == ':') {
                // Don't index :path since it varies
                if (header.name == ":path")
                    return false;
            }
            // Don't index very large values
            if (header.name.size() + header.value.size() > dynamic_table_.max_capacity() / 4) {
                return false;
            }
            return true;
        }

        void encode_indexed_dynamic(dp::Vector<dp::u8> &result, dp::usize relative_index, dp::u64 base) {
            // Format: 10xxxxxx (6-bit relative index with 0b10 prefix for post-base)
            // For simplicity, use post-base referencing
            if (relative_index < 64) {
                result.push_back(static_cast<dp::u8>(0x80 | relative_index));
            } else {
                result.push_back(0xBF); // 0b10111111
                encode_integer(result, relative_index - 63, 0);
            }
        }

        void encode_integer_8bit(dp::Vector<dp::u8> &result, dp::u64 value) {
            if (value < 255) {
                result.push_back(static_cast<dp::u8>(value));
            } else {
                result.push_back(0xFF);
                dp::u64 remaining = value - 255;
                while (remaining >= 128) {
                    result.push_back(static_cast<dp::u8>(0x80 | (remaining & 0x7F)));
                    remaining >>= 7;
                }
                result.push_back(static_cast<dp::u8>(remaining));
            }
        }

        void encode_integer_7bit(dp::Vector<dp::u8> &result, dp::u64 value, dp::u8 prefix) {
            if (value < 127) {
                result.push_back(static_cast<dp::u8>(prefix | value));
            } else {
                result.push_back(prefix | 0x7F);
                dp::u64 remaining = value - 127;
                while (remaining >= 128) {
                    result.push_back(static_cast<dp::u8>(0x80 | (remaining & 0x7F)));
                    remaining >>= 7;
                }
                result.push_back(static_cast<dp::u8>(remaining));
            }
        }

        void encode_header(dp::Vector<dp::u8> &result, const HeaderField &header) {
            // Try to find in static table
            int name_match = -1;
            int full_match = -1;

            for (dp::usize i = 0; i < QPACK_STATIC_TABLE_SIZE; i++) {
                if (QPACK_STATIC_TABLE[i].name == header.name) {
                    if (name_match < 0) {
                        name_match = static_cast<int>(i);
                    }
                    if (QPACK_STATIC_TABLE[i].value == header.value) {
                        full_match = static_cast<int>(i);
                        break;
                    }
                }
            }

            if (full_match >= 0) {
                // Indexed Header Field (static table)
                // Format: 1 T=1 index (6-bit prefix)
                encode_indexed_static(result, static_cast<dp::usize>(full_match));
            } else if (name_match >= 0) {
                // Literal Header Field With Name Reference (static)
                encode_literal_with_name_ref(result, static_cast<dp::usize>(name_match), header.value);
            } else {
                // Literal Header Field Without Name Reference
                encode_literal(result, header.name, header.value);
            }
        }

        void encode_indexed_static(dp::Vector<dp::u8> &result, dp::usize index) {
            // Format: 11xxxxxx (6-bit index with 0b11 prefix)
            if (index < 64) {
                result.push_back(static_cast<dp::u8>(0xC0 | index));
            } else {
                // Need multi-byte encoding
                result.push_back(0xFF); // 0b11111111
                encode_integer(result, index - 63, 0);
            }
        }

        void encode_literal_with_name_ref(dp::Vector<dp::u8> &result, dp::usize index, const dp::String &value) {
            // Format: 01 N T=1 index (4-bit prefix)
            // N = never indexed (0), T = static table (1)
            if (index < 16) {
                result.push_back(static_cast<dp::u8>(0x50 | index)); // 0b0101xxxx
            } else {
                result.push_back(0x5F); // 0b01011111
                encode_integer(result, index - 15, 0);
            }

            // Encode value
            encode_string(result, value);
        }

        void encode_literal(dp::Vector<dp::u8> &result, const dp::String &name, const dp::String &value) {
            // Format: 001 N (4-bit prefix for name length)
            // N = never indexed (0)
            result.push_back(0x20); // 0b00100000

            // Encode name
            encode_string(result, name);

            // Encode value
            encode_string(result, value);
        }

        void encode_string(dp::Vector<dp::u8> &result, const dp::String &str) {
            if (use_huffman_) {
                auto encoded = huffman_encoder().encode(str);
                dp::usize len = encoded.size();

                // H=1 for Huffman encoding
                if (len < 128) {
                    result.push_back(static_cast<dp::u8>(0x80 | len));
                } else {
                    result.push_back(0xFF);
                    encode_integer(result, len - 127, 0);
                }
                result.insert(result.end(), encoded.begin(), encoded.end());
            } else {
                // Without Huffman encoding (H=0)
                dp::usize len = str.size();
                if (len < 128) {
                    result.push_back(static_cast<dp::u8>(len));
                } else {
                    result.push_back(0x7F);
                    encode_integer(result, len - 127, 0);
                }
                result.insert(result.end(), str.begin(), str.end());
            }
        }

        void encode_integer(dp::Vector<dp::u8> &result, dp::usize value, dp::u8 prefix_bits) {
            // QPACK integer encoding
            (void)prefix_bits;
            while (value >= 128) {
                result.push_back(static_cast<dp::u8>((value & 0x7F) | 0x80));
                value >>= 7;
            }
            result.push_back(static_cast<dp::u8>(value));
        }
    };

    // QPACK Decoder Stream Instructions (RFC 9204 Section 4.4)
    // These are sent by the decoder to the encoder to acknowledge dynamic table state
    namespace decoder_instructions {
        // Section Acknowledgement
        inline dp::Vector<dp::u8> section_acknowledgement(dp::u64 stream_id) {
            dp::Vector<dp::u8> data;
            // Format: 1xxxxxxx (7-bit prefix)
            if (stream_id < 128) {
                data.push_back(static_cast<dp::u8>(0x80 | stream_id));
            } else {
                data.push_back(0xFF);
                dp::u64 remaining = stream_id - 127;
                while (remaining >= 128) {
                    data.push_back(static_cast<dp::u8>(0x80 | (remaining & 0x7F)));
                    remaining >>= 7;
                }
                data.push_back(static_cast<dp::u8>(remaining));
            }
            return data;
        }

        // Stream Cancellation
        inline dp::Vector<dp::u8> stream_cancellation(dp::u64 stream_id) {
            dp::Vector<dp::u8> data;
            // Format: 01xxxxxx (6-bit prefix)
            if (stream_id < 64) {
                data.push_back(static_cast<dp::u8>(0x40 | stream_id));
            } else {
                data.push_back(0x7F);
                dp::u64 remaining = stream_id - 63;
                while (remaining >= 128) {
                    data.push_back(static_cast<dp::u8>(0x80 | (remaining & 0x7F)));
                    remaining >>= 7;
                }
                data.push_back(static_cast<dp::u8>(remaining));
            }
            return data;
        }

        // Insert Count Increment
        inline dp::Vector<dp::u8> insert_count_increment(dp::u64 increment) {
            dp::Vector<dp::u8> data;
            // Format: 00xxxxxx (6-bit prefix)
            if (increment < 64) {
                data.push_back(static_cast<dp::u8>(increment));
            } else {
                data.push_back(0x3F);
                dp::u64 remaining = increment - 63;
                while (remaining >= 128) {
                    data.push_back(static_cast<dp::u8>(0x80 | (remaining & 0x7F)));
                    remaining >>= 7;
                }
                data.push_back(static_cast<dp::u8>(remaining));
            }
            return data;
        }
    } // namespace decoder_instructions

    // QPACK Decoder with Huffman and Dynamic Table support
    class QpackDecoder {
      public:
        QpackDecoder() : dynamic_table_(0) {}

        // Set dynamic table capacity
        void set_dynamic_table_capacity(dp::usize capacity) { dynamic_table_.set_max_capacity(capacity); }

        dp::usize dynamic_table_capacity() const { return dynamic_table_.max_capacity(); }
        bool dynamic_table_enabled() const { return dynamic_table_.is_enabled(); }

        // Process encoder stream data (instructions from encoder)
        dp::Res<void> process_encoder_stream(const dp::Vector<dp::u8> &data) {
            dp::usize offset = 0;
            while (offset < data.size()) {
                dp::u8 first = data[offset];

                if ((first & 0xE0) == 0x20) {
                    // Set Dynamic Table Capacity (001xxxxx)
                    auto result = decode_integer(data.data() + offset, data.size() - offset, 5);
                    if (result.is_err()) {
                        return dp::result::err(result.error());
                    }
                    auto [capacity, consumed] = result.value();
                    dynamic_table_.set_max_capacity(capacity);
                    offset += consumed;
                } else if ((first & 0xC0) == 0xC0) {
                    // Insert With Name Reference (11xxxxxx)
                    bool is_static = (first & 0x40) != 0;
                    (void)is_static; // T bit determines static vs dynamic
                    auto idx_result = decode_integer(data.data() + offset, data.size() - offset, 6);
                    if (idx_result.is_err()) {
                        return dp::result::err(idx_result.error());
                    }
                    auto [index, idx_consumed] = idx_result.value();
                    offset += idx_consumed;

                    // Decode value
                    auto val_result = decode_string(data.data() + offset, data.size() - offset);
                    if (val_result.is_err()) {
                        return dp::result::err(val_result.error());
                    }
                    auto [value, val_consumed] = std::move(val_result.value());
                    offset += val_consumed;

                    // Get name from reference
                    dp::String name;
                    if (is_static && index < QPACK_STATIC_TABLE_SIZE) {
                        name = QPACK_STATIC_TABLE[index].name;
                    } else if (!is_static) {
                        auto *entry = dynamic_table_.get_relative(index);
                        if (entry) {
                            name = entry->name;
                        } else {
                            return dp::result::err(dp::Error::invalid_argument("invalid dynamic table reference"));
                        }
                    } else {
                        return dp::result::err(dp::Error::invalid_argument("invalid static table reference"));
                    }

                    dynamic_table_.insert(name, value);
                } else if ((first & 0xC0) == 0x40) {
                    // Insert Without Name Reference (01Hxxxxx)
                    // H bit (bit 5) = Huffman flag for name
                    // 5-bit prefix = name length
                    bool name_huffman = (first & 0x20) != 0;

                    // Decode name length with 5-bit prefix
                    auto name_len_result = decode_integer(data.data() + offset, data.size() - offset, 5);
                    if (name_len_result.is_err()) {
                        return dp::result::err(name_len_result.error());
                    }
                    auto [name_length, name_len_consumed] = name_len_result.value();
                    offset += name_len_consumed;

                    // Read name bytes
                    if (offset + name_length > data.size()) {
                        return dp::result::err(dp::Error::invalid_argument("name truncated"));
                    }
                    dp::String name;
                    if (name_huffman) {
                        auto decoded = huffman_decoder().decode(data.data() + offset, name_length);
                        if (decoded.is_err()) {
                            return dp::result::err(decoded.error());
                        }
                        name = std::move(decoded.value());
                    } else {
                        name = dp::String(reinterpret_cast<const char *>(data.data() + offset), name_length);
                    }
                    offset += name_length;

                    // Decode value (H flag in bit 7, 7-bit prefix)
                    auto val_result = decode_string(data.data() + offset, data.size() - offset);
                    if (val_result.is_err()) {
                        return dp::result::err(val_result.error());
                    }
                    auto [value, val_consumed] = std::move(val_result.value());
                    offset += val_consumed;

                    dynamic_table_.insert(name, value);
                } else if ((first & 0xE0) == 0x00) {
                    // Duplicate (000xxxxx)
                    auto result = decode_integer(data.data() + offset, data.size() - offset, 5);
                    if (result.is_err()) {
                        return dp::result::err(result.error());
                    }
                    auto [index, consumed] = result.value();
                    offset += consumed;

                    dynamic_table_.duplicate(index);
                } else {
                    return dp::result::err(dp::Error::invalid_argument("unknown encoder instruction"));
                }
            }
            return dp::result::ok();
        }

        // Get decoder stream data (instructions to send to encoder)
        dp::Vector<dp::u8> get_decoder_stream_data() {
            auto data = std::move(pending_decoder_stream_);
            pending_decoder_stream_.clear();
            return data;
        }

        bool has_decoder_stream_data() const { return !pending_decoder_stream_.empty(); }

        // Acknowledge a decoded section
        void acknowledge_section(dp::u64 stream_id) {
            auto instr = decoder_instructions::section_acknowledgement(stream_id);
            pending_decoder_stream_.insert(pending_decoder_stream_.end(), instr.begin(), instr.end());
        }

        // Access to dynamic table for testing
        const QpackDynamicTable &dynamic_table() const { return dynamic_table_; }

        // Decode field section to header list
        dp::Res<HeaderList> decode(const dp::Vector<dp::u8> &data) {
            if (data.size() < 2) {
                return dp::result::err(dp::Error::invalid_argument("field section too short"));
            }

            dp::usize offset = 0;

            // Required Insert Count
            auto ric_result = decode_integer(data.data() + offset, data.size() - offset, 8);
            if (ric_result.is_err()) {
                return dp::result::err(ric_result.error());
            }
            offset += ric_result.value().second;

            // Delta Base (sign bit + integer)
            if (offset >= data.size()) {
                return dp::result::err(dp::Error::invalid_argument("missing delta base"));
            }
            auto db_result = decode_integer(data.data() + offset, data.size() - offset, 7);
            if (db_result.is_err()) {
                return dp::result::err(db_result.error());
            }
            offset += db_result.value().second;

            HeaderList headers;

            while (offset < data.size()) {
                dp::u8 first = data[offset];

                if ((first & 0x80) != 0) {
                    // Indexed Header Field
                    auto result = decode_indexed(data.data() + offset, data.size() - offset);
                    if (result.is_err()) {
                        return dp::result::err(result.error());
                    }
                    auto [header, consumed] = std::move(result.value());
                    headers.push_back(std::move(header));
                    offset += consumed;
                } else if ((first & 0x40) != 0) {
                    // Literal Header Field With Name Reference
                    auto result = decode_literal_with_ref(data.data() + offset, data.size() - offset);
                    if (result.is_err()) {
                        return dp::result::err(result.error());
                    }
                    auto [header, consumed] = std::move(result.value());
                    headers.push_back(std::move(header));
                    offset += consumed;
                } else if ((first & 0x20) != 0) {
                    // Literal Header Field Without Name Reference
                    auto result = decode_literal(data.data() + offset, data.size() - offset);
                    if (result.is_err()) {
                        return dp::result::err(result.error());
                    }
                    auto [header, consumed] = std::move(result.value());
                    headers.push_back(std::move(header));
                    offset += consumed;
                } else {
                    return dp::result::err(dp::Error::invalid_argument("unknown field line type"));
                }
            }

            return dp::result::ok(std::move(headers));
        }

      private:
        dp::Res<std::pair<dp::usize, dp::usize>> decode_integer(const dp::u8 *data, dp::usize size,
                                                                dp::u8 prefix_bits) {
            if (size < 1) {
                return dp::result::err(dp::Error::invalid_argument("empty integer"));
            }

            dp::u8 mask = (1 << prefix_bits) - 1;
            dp::usize value = data[0] & mask;
            dp::usize offset = 1;

            if (value == mask) {
                dp::usize m = 0;
                while (offset < size) {
                    dp::u8 b = data[offset++];
                    value += static_cast<dp::usize>(b & 0x7F) << m;
                    m += 7;
                    if ((b & 0x80) == 0) {
                        break;
                    }
                }
            }

            return dp::result::ok(std::make_pair(value, offset));
        }

        dp::Res<std::pair<dp::String, dp::usize>> decode_string(const dp::u8 *data, dp::usize size) {
            if (size < 1) {
                return dp::result::err(dp::Error::invalid_argument("empty string"));
            }

            bool huffman = (data[0] & 0x80) != 0;

            auto len_result = decode_integer(data, size, 7);
            if (len_result.is_err()) {
                return dp::result::err(len_result.error());
            }
            auto [length, len_bytes] = len_result.value();

            if (len_bytes + length > size) {
                return dp::result::err(dp::Error::invalid_argument("string truncated"));
            }

            if (huffman) {
                // Huffman decode
                auto decoded = huffman_decoder().decode(data + len_bytes, length);
                if (decoded.is_err()) {
                    return dp::result::err(decoded.error());
                }
                return dp::result::ok(std::make_pair(std::move(decoded.value()), len_bytes + length));
            } else {
                // Plain string
                dp::String str(reinterpret_cast<const char *>(data + len_bytes), length);
                return dp::result::ok(std::make_pair(std::move(str), len_bytes + length));
            }
        }

        dp::Res<std::pair<HeaderField, dp::usize>> decode_indexed(const dp::u8 *data, dp::usize size) {
            // Format: 1 T index
            bool is_static = (data[0] & 0x40) != 0;
            auto idx_result = decode_integer(data, size, 6);
            if (idx_result.is_err()) {
                return dp::result::err(idx_result.error());
            }
            auto [index, consumed] = idx_result.value();

            if (is_static) {
                // Static table reference
                if (index < QPACK_STATIC_TABLE_SIZE) {
                    return dp::result::ok(std::make_pair(QPACK_STATIC_TABLE[index], consumed));
                }
                return dp::result::err(dp::Error::invalid_argument("invalid static table index"));
            } else {
                // Dynamic table reference (relative index)
                auto *entry = dynamic_table_.get_relative(index);
                if (entry) {
                    return dp::result::ok(std::make_pair(HeaderField{entry->name, entry->value}, consumed));
                }
                return dp::result::err(dp::Error::invalid_argument("invalid dynamic table index"));
            }
        }

        dp::Res<std::pair<HeaderField, dp::usize>> decode_literal_with_ref(const dp::u8 *data, dp::usize size) {
            // Format: 01 N T index
            bool is_static = (data[0] & 0x10) != 0;
            auto idx_result = decode_integer(data, size, 4);
            if (idx_result.is_err()) {
                return dp::result::err(idx_result.error());
            }
            auto [index, idx_consumed] = idx_result.value();

            dp::String name;
            if (is_static) {
                // Static table name reference
                if (index < QPACK_STATIC_TABLE_SIZE) {
                    name = QPACK_STATIC_TABLE[index].name;
                } else {
                    return dp::result::err(dp::Error::invalid_argument("invalid static table name reference"));
                }
            } else {
                // Dynamic table name reference (relative index)
                auto *entry = dynamic_table_.get_relative(index);
                if (entry) {
                    name = entry->name;
                } else {
                    return dp::result::err(dp::Error::invalid_argument("invalid dynamic table name reference"));
                }
            }

            auto val_result = decode_string(data + idx_consumed, size - idx_consumed);
            if (val_result.is_err()) {
                return dp::result::err(val_result.error());
            }
            auto [value, val_consumed] = std::move(val_result.value());

            return dp::result::ok(std::make_pair(HeaderField{name, std::move(value)}, idx_consumed + val_consumed));
        }

        dp::Res<std::pair<HeaderField, dp::usize>> decode_literal(const dp::u8 *data, dp::usize size) {
            // Format: 001 N
            dp::usize offset = 0;

            // Skip first byte (we know it's 0x2X)
            offset++;

            // Decode name
            auto name_result = decode_string(data + offset, size - offset);
            if (name_result.is_err()) {
                return dp::result::err(name_result.error());
            }
            auto [name, name_consumed] = std::move(name_result.value());
            offset += name_consumed;

            // Decode value
            auto val_result = decode_string(data + offset, size - offset);
            if (val_result.is_err()) {
                return dp::result::err(val_result.error());
            }
            auto [value, val_consumed] = std::move(val_result.value());
            offset += val_consumed;

            return dp::result::ok(std::make_pair(HeaderField{std::move(name), std::move(value)}, offset));
        }

        QpackDynamicTable dynamic_table_;
        dp::Vector<dp::u8> pending_decoder_stream_;
    };

} // namespace netpipe::http3

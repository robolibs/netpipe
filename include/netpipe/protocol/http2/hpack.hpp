#pragma once

#include <netpipe/protocol/http2/types.hpp>
#include <netpipe/protocol/http3/qpack.hpp>

namespace netpipe::http2 {

    class HpackContext {
      public:
        HpackContext() = default;

        void set_max_table_size(dp::usize size) {
            max_table_size_ = size;
            evict_to_limit();
        }

        dp::usize max_table_size() const { return max_table_size_; }
        dp::usize dynamic_table_size() const { return dynamic_table_size_; }

        dp::Result<dp::Vector<dp::u8>> encode(const http::HeaderList &headers) {
            dp::Vector<dp::u8> out;

            for (const auto &header : headers) {
                auto exact_idx = find_exact_index(header);
                if (exact_idx.has_value()) {
                    encode_integer(out, exact_idx.value(), 7, 0x80);
                    continue;
                }

                auto name_idx = find_name_index(header.name);
                if (name_idx.has_value()) {
                    encode_integer(out, name_idx.value(), 6, 0x40);
                } else {
                    encode_integer(out, 0, 6, 0x40);
                    encode_string(out, header.name);
                }

                encode_string(out, header.value);
                add_dynamic(header);
            }

            return dp::result::ok(std::move(out));
        }

        dp::Result<http::HeaderList> decode(const dp::Vector<dp::u8> &block) {
            http::HeaderList headers;
            dp::usize pos = 0;

            while (pos < block.size()) {
                dp::u8 first = block[pos];

                if ((first & 0x80) != 0) {
                    auto idx = decode_integer(block, pos, 7);
                    if (idx.is_err()) {
                        return dp::result::err(idx.error());
                    }
                    auto header = index_to_header(idx.value());
                    if (header.is_err()) {
                        return dp::result::err(header.error());
                    }
                    headers.push_back(header.value());
                    continue;
                }

                if ((first & 0xC0) == 0x40) {
                    auto name_res = decode_name_from_index_or_literal(block, pos, 6);
                    if (name_res.is_err()) {
                        return dp::result::err(name_res.error());
                    }
                    auto value_res = decode_string(block, pos);
                    if (value_res.is_err()) {
                        return dp::result::err(value_res.error());
                    }

                    http::HeaderField header{name_res.value(), value_res.value()};
                    headers.push_back(header);
                    add_dynamic(header);
                    continue;
                }

                if ((first & 0xF0) == 0x00 || (first & 0xF0) == 0x10) {
                    auto name_res = decode_name_from_index_or_literal(block, pos, 4);
                    if (name_res.is_err()) {
                        return dp::result::err(name_res.error());
                    }
                    auto value_res = decode_string(block, pos);
                    if (value_res.is_err()) {
                        return dp::result::err(value_res.error());
                    }

                    headers.push_back(http::HeaderField{name_res.value(), value_res.value()});
                    continue;
                }

                if ((first & 0xE0) == 0x20) {
                    auto size_res = decode_integer(block, pos, 5);
                    if (size_res.is_err()) {
                        return dp::result::err(size_res.error());
                    }
                    set_max_table_size(size_res.value());
                    continue;
                }

                return dp::result::err(dp::Error::invalid_argument("unsupported HPACK field representation"));
            }

            return dp::result::ok(std::move(headers));
        }

      private:
        static dp::usize entry_size(const http::HeaderField &header) {
            return header.name.size() + header.value.size() + 32;
        }

        void evict_to_limit() {
            while (dynamic_table_size_ > max_table_size_ && !dynamic_table_.empty()) {
                dynamic_table_size_ -= entry_size(dynamic_table_.back());
                dynamic_table_.pop_back();
            }
        }

        void add_dynamic(const http::HeaderField &header) {
            auto sz = entry_size(header);
            if (sz > max_table_size_) {
                dynamic_table_.clear();
                dynamic_table_size_ = 0;
                return;
            }

            dynamic_table_.insert(dynamic_table_.begin(), header);
            dynamic_table_size_ += sz;
            evict_to_limit();
        }

        static void encode_integer(dp::Vector<dp::u8> &out, dp::usize value, dp::u8 prefix_bits, dp::u8 prefix_mask) {
            dp::u8 max_prefix = static_cast<dp::u8>((1u << prefix_bits) - 1u);
            if (value < max_prefix) {
                out.push_back(static_cast<dp::u8>(prefix_mask | static_cast<dp::u8>(value)));
                return;
            }

            out.push_back(static_cast<dp::u8>(prefix_mask | max_prefix));
            value -= max_prefix;

            while (value >= 128) {
                out.push_back(static_cast<dp::u8>((value % 128) + 128));
                value /= 128;
            }
            out.push_back(static_cast<dp::u8>(value));
        }

        static dp::Result<dp::usize> decode_integer(const dp::Vector<dp::u8> &in, dp::usize &pos, dp::u8 prefix_bits) {
            if (pos >= in.size()) {
                return dp::result::err(dp::Error::invalid_argument("HPACK integer decode out of bounds"));
            }

            dp::u8 max_prefix = static_cast<dp::u8>((1u << prefix_bits) - 1u);
            dp::usize value = in[pos] & max_prefix;
            ++pos;

            if (value < max_prefix) {
                return dp::result::ok(value);
            }

            dp::usize m = 0;
            while (pos < in.size()) {
                dp::u8 b = in[pos++];
                value += static_cast<dp::usize>(b & 0x7F) << m;
                if ((b & 0x80) == 0) {
                    return dp::result::ok(value);
                }
                m += 7;
                if (m > 28) {
                    return dp::result::err(dp::Error::invalid_argument("HPACK integer overflow"));
                }
            }

            return dp::result::err(dp::Error::invalid_argument("truncated HPACK integer"));
        }

        static void encode_string(dp::Vector<dp::u8> &out, const dp::String &value) {
            encode_integer(out, value.size(), 7, 0x00);
            out.insert(out.end(), value.begin(), value.end());
        }

        static dp::Result<dp::String> decode_string(const dp::Vector<dp::u8> &in, dp::usize &pos) {
            if (pos >= in.size()) {
                return dp::result::err(dp::Error::invalid_argument("HPACK string out of bounds"));
            }

            bool huffman = (in[pos] & 0x80) != 0;
            auto len_res = decode_integer(in, pos, 7);
            if (len_res.is_err()) {
                return dp::result::err(len_res.error());
            }

            dp::usize len = len_res.value();
            if (pos + len > in.size()) {
                return dp::result::err(dp::Error::invalid_argument("truncated HPACK string"));
            }

            if (huffman) {
                auto decoded = http3::huffman_decoder().decode(in.data() + pos, len);
                if (decoded.is_err()) {
                    return dp::result::err(decoded.error());
                }
                pos += len;
                return dp::result::ok(std::move(decoded.value()));
            }

            dp::String out(reinterpret_cast<const char *>(in.data() + pos), len);
            pos += len;
            return dp::result::ok(std::move(out));
        }

        dp::Result<http::HeaderField> index_to_header(dp::usize index) const {
            if (index == 0) {
                return dp::result::err(dp::Error::invalid_argument("HPACK index 0 is invalid"));
            }

            if (index <= static_table().size()) {
                auto &entry = static_table()[index - 1];
                return dp::result::ok(http::HeaderField{entry.first, entry.second});
            }

            dp::usize dyn_index = index - static_table().size() - 1;
            if (dyn_index >= dynamic_table_.size()) {
                return dp::result::err(dp::Error::invalid_argument("HPACK dynamic index out of range"));
            }

            return dp::result::ok(dynamic_table_[dyn_index]);
        }

        dp::Result<dp::String> decode_name_from_index_or_literal(const dp::Vector<dp::u8> &block, dp::usize &pos,
                                                                 dp::u8 prefix_bits) const {
            auto name_index_res = decode_integer(block, pos, prefix_bits);
            if (name_index_res.is_err()) {
                return dp::result::err(name_index_res.error());
            }

            if (name_index_res.value() == 0) {
                return decode_string(block, pos);
            }

            auto indexed = index_to_header(name_index_res.value());
            if (indexed.is_err()) {
                return dp::result::err(indexed.error());
            }
            return dp::result::ok(indexed.value().name);
        }

        dp::Optional<dp::usize> find_name_index(const dp::String &name) const {
            for (dp::usize i = 0; i < static_table().size(); ++i) {
                if (static_table()[i].first == name) {
                    return i + 1;
                }
            }
            for (dp::usize i = 0; i < dynamic_table_.size(); ++i) {
                if (dynamic_table_[i].name == name) {
                    return static_table().size() + i + 1;
                }
            }
            return dp::nullopt;
        }

        dp::Optional<dp::usize> find_exact_index(const http::HeaderField &header) const {
            for (dp::usize i = 0; i < static_table().size(); ++i) {
                if (static_table()[i].first == header.name && static_table()[i].second == header.value) {
                    return i + 1;
                }
            }
            for (dp::usize i = 0; i < dynamic_table_.size(); ++i) {
                if (dynamic_table_[i].name == header.name && dynamic_table_[i].value == header.value) {
                    return static_table().size() + i + 1;
                }
            }
            return dp::nullopt;
        }

        static const dp::Vector<std::pair<dp::String, dp::String>> &static_table() {
            static const dp::Vector<std::pair<dp::String, dp::String>> table = {
                {":authority", ""},
                {":method", "GET"},
                {":method", "POST"},
                {":path", "/"},
                {":path", "/index.html"},
                {":scheme", "http"},
                {":scheme", "https"},
                {":status", "200"},
                {":status", "204"},
                {":status", "206"},
                {":status", "304"},
                {":status", "400"},
                {":status", "404"},
                {":status", "500"},
                {"accept-charset", ""},
                {"accept-encoding", "gzip, deflate"},
                {"accept-language", ""},
                {"accept-ranges", ""},
                {"accept", ""},
                {"access-control-allow-origin", ""},
                {"age", ""},
                {"allow", ""},
                {"authorization", ""},
                {"cache-control", ""},
                {"content-disposition", ""},
                {"content-encoding", ""},
                {"content-language", ""},
                {"content-length", ""},
                {"content-location", ""},
                {"content-range", ""},
                {"content-type", ""},
                {"cookie", ""},
                {"date", ""},
                {"etag", ""},
                {"expect", ""},
                {"expires", ""},
                {"from", ""},
                {"host", ""},
                {"if-match", ""},
                {"if-modified-since", ""},
                {"if-none-match", ""},
                {"if-range", ""},
                {"if-unmodified-since", ""},
                {"last-modified", ""},
                {"link", ""},
                {"location", ""},
                {"max-forwards", ""},
                {"proxy-authenticate", ""},
                {"proxy-authorization", ""},
                {"range", ""},
                {"referer", ""},
                {"refresh", ""},
                {"retry-after", ""},
                {"server", ""},
                {"set-cookie", ""},
                {"strict-transport-security", ""},
                {"transfer-encoding", ""},
                {"user-agent", ""},
                {"vary", ""},
                {"via", ""},
                {"www-authenticate", ""},
            };
            return table;
        }

        dp::Vector<http::HeaderField> dynamic_table_;
        dp::usize dynamic_table_size_ = 0;
        dp::usize max_table_size_ = 4096;
    };

} // namespace netpipe::http2

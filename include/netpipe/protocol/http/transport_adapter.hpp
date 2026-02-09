#pragma once

#include <netpipe/core/common.hpp>

namespace netpipe::http {

    enum class StreamMode { FramedMessages, RawBytes };

    class TransportAdapter {
      public:
        explicit TransportAdapter(StreamMode mode) : mode_(mode) {}

        StreamMode mode() const { return mode_; }

        void feed(const netpipe::Message &bytes) { buffer_.insert(buffer_.end(), bytes.begin(), bytes.end()); }

        void feed(netpipe::Message &&bytes) {
            buffer_.insert(buffer_.end(), std::make_move_iterator(bytes.begin()), std::make_move_iterator(bytes.end()));
        }

        bool has_complete_unit() const {
            if (mode_ == StreamMode::RawBytes) {
                return !buffer_.empty();
            }

            if (buffer_.size() < 4) {
                return false;
            }

            dp::u32 message_size = netpipe::decode_u32_be(buffer_.data());
            return buffer_.size() >= static_cast<dp::usize>(4 + message_size);
        }

        dp::Result<netpipe::Message> pop_unit() {
            if (mode_ == StreamMode::RawBytes) {
                if (buffer_.empty()) {
                    return dp::result::err(dp::Error::not_found("no bytes available"));
                }

                netpipe::Message out = std::move(buffer_);
                buffer_.clear();
                return dp::result::ok(std::move(out));
            }

            if (buffer_.size() < 4) {
                return dp::result::err(dp::Error::not_found("insufficient data for framed header"));
            }

            dp::u32 message_size = netpipe::decode_u32_be(buffer_.data());
            if (buffer_.size() < static_cast<dp::usize>(4 + message_size)) {
                return dp::result::err(dp::Error::not_found("incomplete framed payload"));
            }

            netpipe::Message out(buffer_.begin() + 4, buffer_.begin() + 4 + message_size);
            buffer_.erase(buffer_.begin(), buffer_.begin() + 4 + message_size);
            return dp::result::ok(std::move(out));
        }

        static netpipe::Message encode_unit(StreamMode mode, const netpipe::Message &payload) {
            if (mode == StreamMode::RawBytes) {
                return payload;
            }

            netpipe::Message frame;
            frame.reserve(payload.size() + 4);
            auto len = netpipe::encode_u32_be(static_cast<dp::u32>(payload.size()));
            frame.insert(frame.end(), len.begin(), len.end());
            frame.insert(frame.end(), payload.begin(), payload.end());
            return frame;
        }

      private:
        StreamMode mode_;
        netpipe::Message buffer_;
    };

} // namespace netpipe::http

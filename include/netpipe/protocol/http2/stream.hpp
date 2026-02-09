#pragma once

#include <netpipe/protocol/http2/frame.hpp>
#include <netpipe/protocol/http2/hpack.hpp>

namespace netpipe::http2 {

    enum class StreamState { Idle, Open, HalfClosedLocal, HalfClosedRemote, Closed };

    struct DecodedHeaders {
        dp::u32 stream_id = 0;
        http::HeaderList headers;
        bool end_stream = false;
    };

    class StreamManager {
      public:
        StreamManager() = default;

        StreamState state(dp::u32 stream_id) const {
            auto it = streams_.find(stream_id);
            if (it == streams_.end()) {
                return StreamState::Idle;
            }
            return it->second;
        }

        dp::Result<dp::Optional<DecodedHeaders>> process_incoming_frame(const Frame &frame) {
            if (frame.header.stream_id == 0) {
                return dp::result::err(dp::Error::invalid_argument("stream frame must have non-zero stream id"));
            }

            switch (frame.header.type) {
            case FrameType::Headers:
                return process_headers(frame);
            case FrameType::Continuation:
                return process_continuation(frame);
            case FrameType::Data:
                return process_data(frame);
            case FrameType::RstStream:
                streams_[frame.header.stream_id] = StreamState::Closed;
                return dp::result::ok(dp::Optional<DecodedHeaders>{});
            default:
                return dp::result::ok(dp::Optional<DecodedHeaders>{});
            }
        }

      private:
        struct PendingHeaderBlock {
            bool active = false;
            dp::u32 stream_id = 0;
            bool end_stream = false;
            dp::Vector<dp::u8> block;
        };

        dp::Result<dp::Optional<DecodedHeaders>> process_headers(const Frame &frame) {
            auto current = state(frame.header.stream_id);
            if (current == StreamState::Closed || current == StreamState::HalfClosedRemote) {
                return dp::result::err(dp::Error::invalid_argument("received HEADERS on closed remote stream"));
            }

            if (pending_.active) {
                return dp::result::err(
                    dp::Error::invalid_argument("received HEADERS while previous header block incomplete"));
            }

            if (current == StreamState::Idle) {
                streams_[frame.header.stream_id] = StreamState::Open;
            }

            pending_.active = true;
            pending_.stream_id = frame.header.stream_id;
            pending_.end_stream = (frame.header.flags & 0x1) != 0;
            pending_.block = frame.payload;

            if ((frame.header.flags & 0x4) != 0) {
                return finalize_header_block();
            }

            return dp::result::ok(dp::Optional<DecodedHeaders>{});
        }

        dp::Result<dp::Optional<DecodedHeaders>> process_continuation(const Frame &frame) {
            if (!pending_.active) {
                return dp::result::err(dp::Error::invalid_argument("unexpected CONTINUATION frame"));
            }
            if (pending_.stream_id != frame.header.stream_id) {
                return dp::result::err(dp::Error::invalid_argument("CONTINUATION stream id mismatch"));
            }

            pending_.block.insert(pending_.block.end(), frame.payload.begin(), frame.payload.end());
            if ((frame.header.flags & 0x4) != 0) {
                return finalize_header_block();
            }

            return dp::result::ok(dp::Optional<DecodedHeaders>{});
        }

        dp::Result<dp::Optional<DecodedHeaders>> process_data(const Frame &frame) {
            auto current = state(frame.header.stream_id);
            if (current == StreamState::Idle || current == StreamState::Closed ||
                current == StreamState::HalfClosedRemote) {
                return dp::result::err(dp::Error::invalid_argument("received DATA on invalid stream state"));
            }

            if ((frame.header.flags & 0x1) != 0) {
                transition_remote_end(frame.header.stream_id);
            }

            return dp::result::ok(dp::Optional<DecodedHeaders>{});
        }

        dp::Result<dp::Optional<DecodedHeaders>> finalize_header_block() {
            auto decoded = decoder_.decode(pending_.block);
            if (decoded.is_err()) {
                pending_ = PendingHeaderBlock{};
                return dp::result::err(decoded.error());
            }

            auto pseudo_ok = validate_pseudo_headers(decoded.value());
            if (pseudo_ok.is_err()) {
                pending_ = PendingHeaderBlock{};
                return dp::result::err(pseudo_ok.error());
            }

            DecodedHeaders out;
            out.stream_id = pending_.stream_id;
            out.headers = std::move(decoded.value());
            out.end_stream = pending_.end_stream;

            if (pending_.end_stream) {
                transition_remote_end(out.stream_id);
            }

            pending_ = PendingHeaderBlock{};
            return dp::result::ok(dp::Optional<DecodedHeaders>(std::move(out)));
        }

        dp::Result<void> validate_pseudo_headers(const http::HeaderList &headers) const {
            bool seen_regular = false;
            dp::Map<dp::String, bool> pseudo_seen;
            bool is_request = false;
            bool is_response = false;

            for (const auto &header : headers) {
                bool is_pseudo = !header.name.empty() && header.name[0] == ':';
                if (!is_pseudo) {
                    seen_regular = true;
                    continue;
                }

                if (seen_regular) {
                    return dp::result::err(
                        dp::Error::invalid_argument("pseudo-headers must appear before regular headers"));
                }
                if (pseudo_seen.find(header.name) != pseudo_seen.end()) {
                    return dp::result::err(dp::Error::invalid_argument("duplicate pseudo-header"));
                }
                pseudo_seen[header.name] = true;

                if (header.name == ":status") {
                    is_response = true;
                } else if (header.name == ":method" || header.name == ":scheme" || header.name == ":path" ||
                           header.name == ":authority") {
                    is_request = true;
                } else {
                    return dp::result::err(dp::Error::invalid_argument("unknown pseudo-header"));
                }
            }

            if (is_request && is_response) {
                return dp::result::err(dp::Error::invalid_argument("request and response pseudo-headers mixed"));
            }

            if (is_request) {
                if (pseudo_seen.find(":method") == pseudo_seen.end() ||
                    pseudo_seen.find(":scheme") == pseudo_seen.end() ||
                    pseudo_seen.find(":path") == pseudo_seen.end()) {
                    return dp::result::err(dp::Error::invalid_argument("missing required request pseudo-header"));
                }
            }

            if (is_response && pseudo_seen.find(":status") == pseudo_seen.end()) {
                return dp::result::err(dp::Error::invalid_argument("missing :status pseudo-header"));
            }

            return dp::result::ok();
        }

        void transition_remote_end(dp::u32 stream_id) {
            auto current = state(stream_id);
            switch (current) {
            case StreamState::Open:
                streams_[stream_id] = StreamState::HalfClosedRemote;
                break;
            case StreamState::HalfClosedLocal:
                streams_[stream_id] = StreamState::Closed;
                break;
            default:
                break;
            }
        }

        dp::Map<dp::u32, StreamState> streams_;
        PendingHeaderBlock pending_;
        HpackContext decoder_;
    };

} // namespace netpipe::http2

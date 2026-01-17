#pragma once

#include <datapod/datapod.hpp>
#include <netpipe/transport/stream/quic/types.hpp>
#include <netpipe/transport/stream/quic/varint.hpp>

namespace netpipe::quic {

    // QUIC Frame Types (RFC 9000 Section 12.4)
    enum class FrameType : dp::u64 {
        Padding = 0x00,
        Ping = 0x01,
        Ack = 0x02,
        AckEcn = 0x03,
        ResetStream = 0x04,
        StopSending = 0x05,
        Crypto = 0x06,
        NewToken = 0x07,
        // STREAM frames: 0x08-0x0f (bits encode OFF, LEN, FIN)
        StreamBase = 0x08,
        MaxData = 0x10,
        MaxStreamData = 0x11,
        MaxStreamsBidi = 0x12,
        MaxStreamsUni = 0x13,
        DataBlocked = 0x14,
        StreamDataBlocked = 0x15,
        StreamsBlockedBidi = 0x16,
        StreamsBlockedUni = 0x17,
        NewConnectionId = 0x18,
        RetireConnectionId = 0x19,
        PathChallenge = 0x1a,
        PathResponse = 0x1b,
        ConnectionClose = 0x1c,
        ConnectionCloseApp = 0x1d,
        HandshakeDone = 0x1e
    };

    // Check if a frame type is a STREAM frame (0x08-0x0f)
    inline bool is_stream_frame(dp::u64 frame_type) { return frame_type >= 0x08 && frame_type <= 0x0f; }

    // STREAM frame type bits
    constexpr dp::u8 STREAM_OFF_BIT = 0x04; // Offset field present
    constexpr dp::u8 STREAM_LEN_BIT = 0x02; // Length field present
    constexpr dp::u8 STREAM_FIN_BIT = 0x01; // Final data

    // Base Frame interface
    struct Frame {
        virtual ~Frame() = default;
        virtual FrameType type() const = 0;
        virtual dp::Vector<dp::u8> serialize() const = 0;
    };

    // PADDING Frame (0x00)
    // Used to increase packet size (e.g., to meet minimum Initial packet size)
    struct PaddingFrame : Frame {
        dp::usize count = 1;

        FrameType type() const override { return FrameType::Padding; }

        dp::Vector<dp::u8> serialize() const override { return dp::Vector<dp::u8>(count, 0x00); }

        static dp::Res<std::pair<PaddingFrame, dp::usize>> parse(const dp::u8 *data, dp::usize size) {
            PaddingFrame frame;
            frame.count = 0;

            // Count consecutive zero bytes
            while (frame.count < size && data[frame.count] == 0x00) {
                frame.count++;
            }

            if (frame.count == 0) {
                return dp::result::err(dp::Error::invalid_argument("expected padding frame"));
            }

            return dp::result::ok(std::make_pair(std::move(frame), frame.count));
        }
    };

    // PING Frame (0x01)
    // Used to keep connection alive or elicit ACK
    struct PingFrame : Frame {
        FrameType type() const override { return FrameType::Ping; }

        dp::Vector<dp::u8> serialize() const override { return {0x01}; }

        static dp::Res<std::pair<PingFrame, dp::usize>> parse(const dp::u8 *data, dp::usize size) {
            if (size < 1 || data[0] != 0x01) {
                return dp::result::err(dp::Error::invalid_argument("expected ping frame"));
            }
            return dp::result::ok(std::make_pair(PingFrame{}, static_cast<dp::usize>(1)));
        }
    };

    // ACK Range
    struct AckRange {
        dp::u64 gap;   // Number of unacked packets before this range - 1
        dp::u64 range; // Number of consecutive acked packets - 1
    };

    // ACK Frame (0x02, 0x03)
    struct AckFrame : Frame {
        dp::u64 largest_ack;
        dp::u64 ack_delay;
        dp::Vector<AckRange> ack_ranges; // First range is the largest, implicit

        // ECN counts (only for AckEcn)
        bool has_ecn = false;
        dp::u64 ect0_count = 0;
        dp::u64 ect1_count = 0;
        dp::u64 ecn_ce_count = 0;

        FrameType type() const override { return has_ecn ? FrameType::AckEcn : FrameType::Ack; }

        dp::Vector<dp::u8> serialize() const override {
            dp::Vector<dp::u8> result;

            // Type
            result.push_back(has_ecn ? 0x03 : 0x02);

            // Largest Acknowledged
            auto bytes = varint_encode(largest_ack);
            result.insert(result.end(), bytes.begin(), bytes.end());

            // ACK Delay
            bytes = varint_encode(ack_delay);
            result.insert(result.end(), bytes.begin(), bytes.end());

            // ACK Range Count (excluding first range which is implicit)
            dp::usize range_count = ack_ranges.empty() ? 0 : ack_ranges.size() - 1;
            bytes = varint_encode(range_count);
            result.insert(result.end(), bytes.begin(), bytes.end());

            // First ACK Range (number of consecutive acked packets - 1)
            dp::u64 first_range = ack_ranges.empty() ? 0 : ack_ranges[0].range;
            bytes = varint_encode(first_range);
            result.insert(result.end(), bytes.begin(), bytes.end());

            // Additional ACK Ranges
            for (dp::usize i = 1; i < ack_ranges.size(); i++) {
                bytes = varint_encode(ack_ranges[i].gap);
                result.insert(result.end(), bytes.begin(), bytes.end());

                bytes = varint_encode(ack_ranges[i].range);
                result.insert(result.end(), bytes.begin(), bytes.end());
            }

            // ECN counts
            if (has_ecn) {
                bytes = varint_encode(ect0_count);
                result.insert(result.end(), bytes.begin(), bytes.end());

                bytes = varint_encode(ect1_count);
                result.insert(result.end(), bytes.begin(), bytes.end());

                bytes = varint_encode(ecn_ce_count);
                result.insert(result.end(), bytes.begin(), bytes.end());
            }

            return result;
        }

        static dp::Res<std::pair<AckFrame, dp::usize>> parse(const dp::u8 *data, dp::usize size) {
            if (size < 1) {
                return dp::result::err(dp::Error::invalid_argument("ACK frame too short"));
            }

            AckFrame frame;
            frame.has_ecn = (data[0] == 0x03);
            dp::usize offset = 1;

            // Largest Acknowledged
            auto result = varint_decode(data + offset, size - offset);
            if (result.is_err())
                return dp::result::err(result.error());
            frame.largest_ack = result.value().first;
            offset += result.value().second;

            // ACK Delay
            result = varint_decode(data + offset, size - offset);
            if (result.is_err())
                return dp::result::err(result.error());
            frame.ack_delay = result.value().first;
            offset += result.value().second;

            // ACK Range Count
            result = varint_decode(data + offset, size - offset);
            if (result.is_err())
                return dp::result::err(result.error());
            dp::u64 range_count = result.value().first;
            offset += result.value().second;

            // First ACK Range
            result = varint_decode(data + offset, size - offset);
            if (result.is_err())
                return dp::result::err(result.error());
            frame.ack_ranges.push_back(AckRange{0, result.value().first});
            offset += result.value().second;

            // Additional ACK Ranges
            for (dp::u64 i = 0; i < range_count; i++) {
                AckRange range;

                result = varint_decode(data + offset, size - offset);
                if (result.is_err())
                    return dp::result::err(result.error());
                range.gap = result.value().first;
                offset += result.value().second;

                result = varint_decode(data + offset, size - offset);
                if (result.is_err())
                    return dp::result::err(result.error());
                range.range = result.value().first;
                offset += result.value().second;

                frame.ack_ranges.push_back(range);
            }

            // ECN counts
            if (frame.has_ecn) {
                result = varint_decode(data + offset, size - offset);
                if (result.is_err())
                    return dp::result::err(result.error());
                frame.ect0_count = result.value().first;
                offset += result.value().second;

                result = varint_decode(data + offset, size - offset);
                if (result.is_err())
                    return dp::result::err(result.error());
                frame.ect1_count = result.value().first;
                offset += result.value().second;

                result = varint_decode(data + offset, size - offset);
                if (result.is_err())
                    return dp::result::err(result.error());
                frame.ecn_ce_count = result.value().first;
                offset += result.value().second;
            }

            return dp::result::ok(std::make_pair(std::move(frame), offset));
        }
    };

    // RESET_STREAM Frame (0x04)
    struct ResetStreamFrame : Frame {
        dp::u64 stream_id;
        dp::u64 application_error_code;
        dp::u64 final_size;

        FrameType type() const override { return FrameType::ResetStream; }

        dp::Vector<dp::u8> serialize() const override {
            dp::Vector<dp::u8> result = {0x04};

            auto bytes = varint_encode(stream_id);
            result.insert(result.end(), bytes.begin(), bytes.end());

            bytes = varint_encode(application_error_code);
            result.insert(result.end(), bytes.begin(), bytes.end());

            bytes = varint_encode(final_size);
            result.insert(result.end(), bytes.begin(), bytes.end());

            return result;
        }

        static dp::Res<std::pair<ResetStreamFrame, dp::usize>> parse(const dp::u8 *data, dp::usize size) {
            if (size < 1 || data[0] != 0x04) {
                return dp::result::err(dp::Error::invalid_argument("expected RESET_STREAM frame"));
            }

            ResetStreamFrame frame;
            dp::usize offset = 1;

            auto result = varint_decode(data + offset, size - offset);
            if (result.is_err())
                return dp::result::err(result.error());
            frame.stream_id = result.value().first;
            offset += result.value().second;

            result = varint_decode(data + offset, size - offset);
            if (result.is_err())
                return dp::result::err(result.error());
            frame.application_error_code = result.value().first;
            offset += result.value().second;

            result = varint_decode(data + offset, size - offset);
            if (result.is_err())
                return dp::result::err(result.error());
            frame.final_size = result.value().first;
            offset += result.value().second;

            return dp::result::ok(std::make_pair(std::move(frame), offset));
        }
    };

    // STOP_SENDING Frame (0x05)
    struct StopSendingFrame : Frame {
        dp::u64 stream_id;
        dp::u64 application_error_code;

        FrameType type() const override { return FrameType::StopSending; }

        dp::Vector<dp::u8> serialize() const override {
            dp::Vector<dp::u8> result = {0x05};

            auto bytes = varint_encode(stream_id);
            result.insert(result.end(), bytes.begin(), bytes.end());

            bytes = varint_encode(application_error_code);
            result.insert(result.end(), bytes.begin(), bytes.end());

            return result;
        }

        static dp::Res<std::pair<StopSendingFrame, dp::usize>> parse(const dp::u8 *data, dp::usize size) {
            if (size < 1 || data[0] != 0x05) {
                return dp::result::err(dp::Error::invalid_argument("expected STOP_SENDING frame"));
            }

            StopSendingFrame frame;
            dp::usize offset = 1;

            auto result = varint_decode(data + offset, size - offset);
            if (result.is_err())
                return dp::result::err(result.error());
            frame.stream_id = result.value().first;
            offset += result.value().second;

            result = varint_decode(data + offset, size - offset);
            if (result.is_err())
                return dp::result::err(result.error());
            frame.application_error_code = result.value().first;
            offset += result.value().second;

            return dp::result::ok(std::make_pair(std::move(frame), offset));
        }
    };

    // CRYPTO Frame (0x06)
    // Used to transmit cryptographic handshake messages
    struct CryptoFrame : Frame {
        dp::u64 offset;
        dp::Vector<dp::u8> data;

        FrameType type() const override { return FrameType::Crypto; }

        dp::Vector<dp::u8> serialize() const override {
            dp::Vector<dp::u8> result = {0x06};

            auto bytes = varint_encode(offset);
            result.insert(result.end(), bytes.begin(), bytes.end());

            bytes = varint_encode(data.size());
            result.insert(result.end(), bytes.begin(), bytes.end());

            result.insert(result.end(), data.begin(), data.end());

            return result;
        }

        static dp::Res<std::pair<CryptoFrame, dp::usize>> parse(const dp::u8 *data, dp::usize size) {
            if (size < 1 || data[0] != 0x06) {
                return dp::result::err(dp::Error::invalid_argument("expected CRYPTO frame"));
            }

            CryptoFrame frame;
            dp::usize offset = 1;

            auto result = varint_decode(data + offset, size - offset);
            if (result.is_err())
                return dp::result::err(result.error());
            frame.offset = result.value().first;
            offset += result.value().second;

            result = varint_decode(data + offset, size - offset);
            if (result.is_err())
                return dp::result::err(result.error());
            dp::u64 length = result.value().first;
            offset += result.value().second;

            if (offset + length > size) {
                return dp::result::err(dp::Error::invalid_argument("CRYPTO frame data truncated"));
            }

            frame.data = dp::Vector<dp::u8>(data + offset, data + offset + length);
            offset += length;

            return dp::result::ok(std::make_pair(std::move(frame), offset));
        }
    };

    // NEW_TOKEN Frame (0x07)
    struct NewTokenFrame : Frame {
        dp::Vector<dp::u8> token;

        FrameType type() const override { return FrameType::NewToken; }

        dp::Vector<dp::u8> serialize() const override {
            dp::Vector<dp::u8> result = {0x07};

            auto bytes = varint_encode(token.size());
            result.insert(result.end(), bytes.begin(), bytes.end());

            result.insert(result.end(), token.begin(), token.end());

            return result;
        }

        static dp::Res<std::pair<NewTokenFrame, dp::usize>> parse(const dp::u8 *data, dp::usize size) {
            if (size < 1 || data[0] != 0x07) {
                return dp::result::err(dp::Error::invalid_argument("expected NEW_TOKEN frame"));
            }

            NewTokenFrame frame;
            dp::usize offset = 1;

            auto result = varint_decode(data + offset, size - offset);
            if (result.is_err())
                return dp::result::err(result.error());
            dp::u64 length = result.value().first;
            offset += result.value().second;

            if (offset + length > size) {
                return dp::result::err(dp::Error::invalid_argument("NEW_TOKEN frame data truncated"));
            }

            frame.token = dp::Vector<dp::u8>(data + offset, data + offset + length);
            offset += length;

            return dp::result::ok(std::make_pair(std::move(frame), offset));
        }
    };

    // STREAM Frame (0x08-0x0f)
    struct StreamFrame : Frame {
        dp::u64 stream_id;
        dp::u64 offset = 0;
        dp::Vector<dp::u8> data;
        bool fin = false;

        // Helper to determine the frame type byte
        dp::u8 frame_type_byte() const {
            dp::u8 type_byte = 0x08;
            if (offset > 0)
                type_byte |= STREAM_OFF_BIT;
            type_byte |= STREAM_LEN_BIT; // Always include length for safety
            if (fin)
                type_byte |= STREAM_FIN_BIT;
            return type_byte;
        }

        FrameType type() const override { return FrameType::StreamBase; }

        dp::Vector<dp::u8> serialize() const override {
            dp::Vector<dp::u8> result;

            result.push_back(frame_type_byte());

            auto bytes = varint_encode(stream_id);
            result.insert(result.end(), bytes.begin(), bytes.end());

            if (offset > 0) {
                bytes = varint_encode(offset);
                result.insert(result.end(), bytes.begin(), bytes.end());
            }

            // Always include length
            bytes = varint_encode(data.size());
            result.insert(result.end(), bytes.begin(), bytes.end());

            result.insert(result.end(), data.begin(), data.end());

            return result;
        }

        static dp::Res<std::pair<StreamFrame, dp::usize>> parse(const dp::u8 *data, dp::usize size) {
            if (size < 1 || !is_stream_frame(data[0])) {
                return dp::result::err(dp::Error::invalid_argument("expected STREAM frame"));
            }

            dp::u8 type_byte = data[0];
            bool has_offset = (type_byte & STREAM_OFF_BIT) != 0;
            bool has_length = (type_byte & STREAM_LEN_BIT) != 0;
            bool has_fin = (type_byte & STREAM_FIN_BIT) != 0;

            StreamFrame frame;
            frame.fin = has_fin;
            dp::usize offset = 1;

            auto result = varint_decode(data + offset, size - offset);
            if (result.is_err())
                return dp::result::err(result.error());
            frame.stream_id = result.value().first;
            offset += result.value().second;

            if (has_offset) {
                result = varint_decode(data + offset, size - offset);
                if (result.is_err())
                    return dp::result::err(result.error());
                frame.offset = result.value().first;
                offset += result.value().second;
            }

            dp::u64 length;
            if (has_length) {
                result = varint_decode(data + offset, size - offset);
                if (result.is_err())
                    return dp::result::err(result.error());
                length = result.value().first;
                offset += result.value().second;
            } else {
                // Data extends to end of packet
                length = size - offset;
            }

            if (offset + length > size) {
                return dp::result::err(dp::Error::invalid_argument("STREAM frame data truncated"));
            }

            frame.data = dp::Vector<dp::u8>(data + offset, data + offset + length);
            offset += length;

            return dp::result::ok(std::make_pair(std::move(frame), offset));
        }
    };

    // MAX_DATA Frame (0x10)
    struct MaxDataFrame : Frame {
        dp::u64 maximum_data;

        FrameType type() const override { return FrameType::MaxData; }

        dp::Vector<dp::u8> serialize() const override {
            dp::Vector<dp::u8> result = {0x10};

            auto bytes = varint_encode(maximum_data);
            result.insert(result.end(), bytes.begin(), bytes.end());

            return result;
        }

        static dp::Res<std::pair<MaxDataFrame, dp::usize>> parse(const dp::u8 *data, dp::usize size) {
            if (size < 1 || data[0] != 0x10) {
                return dp::result::err(dp::Error::invalid_argument("expected MAX_DATA frame"));
            }

            MaxDataFrame frame;
            dp::usize offset = 1;

            auto result = varint_decode(data + offset, size - offset);
            if (result.is_err())
                return dp::result::err(result.error());
            frame.maximum_data = result.value().first;
            offset += result.value().second;

            return dp::result::ok(std::make_pair(std::move(frame), offset));
        }
    };

    // MAX_STREAM_DATA Frame (0x11)
    struct MaxStreamDataFrame : Frame {
        dp::u64 stream_id;
        dp::u64 maximum_stream_data;

        FrameType type() const override { return FrameType::MaxStreamData; }

        dp::Vector<dp::u8> serialize() const override {
            dp::Vector<dp::u8> result = {0x11};

            auto bytes = varint_encode(stream_id);
            result.insert(result.end(), bytes.begin(), bytes.end());

            bytes = varint_encode(maximum_stream_data);
            result.insert(result.end(), bytes.begin(), bytes.end());

            return result;
        }

        static dp::Res<std::pair<MaxStreamDataFrame, dp::usize>> parse(const dp::u8 *data, dp::usize size) {
            if (size < 1 || data[0] != 0x11) {
                return dp::result::err(dp::Error::invalid_argument("expected MAX_STREAM_DATA frame"));
            }

            MaxStreamDataFrame frame;
            dp::usize offset = 1;

            auto result = varint_decode(data + offset, size - offset);
            if (result.is_err())
                return dp::result::err(result.error());
            frame.stream_id = result.value().first;
            offset += result.value().second;

            result = varint_decode(data + offset, size - offset);
            if (result.is_err())
                return dp::result::err(result.error());
            frame.maximum_stream_data = result.value().first;
            offset += result.value().second;

            return dp::result::ok(std::make_pair(std::move(frame), offset));
        }
    };

    // MAX_STREAMS Frame (0x12 for bidi, 0x13 for uni)
    struct MaxStreamsFrame : Frame {
        bool unidirectional = false;
        dp::u64 maximum_streams;

        FrameType type() const override {
            return unidirectional ? FrameType::MaxStreamsUni : FrameType::MaxStreamsBidi;
        }

        dp::Vector<dp::u8> serialize() const override {
            dp::Vector<dp::u8> result = {static_cast<dp::u8>(unidirectional ? 0x13 : 0x12)};

            auto bytes = varint_encode(maximum_streams);
            result.insert(result.end(), bytes.begin(), bytes.end());

            return result;
        }

        static dp::Res<std::pair<MaxStreamsFrame, dp::usize>> parse(const dp::u8 *data, dp::usize size) {
            if (size < 1 || (data[0] != 0x12 && data[0] != 0x13)) {
                return dp::result::err(dp::Error::invalid_argument("expected MAX_STREAMS frame"));
            }

            MaxStreamsFrame frame;
            frame.unidirectional = (data[0] == 0x13);
            dp::usize offset = 1;

            auto result = varint_decode(data + offset, size - offset);
            if (result.is_err())
                return dp::result::err(result.error());
            frame.maximum_streams = result.value().first;
            offset += result.value().second;

            return dp::result::ok(std::make_pair(std::move(frame), offset));
        }
    };

    // DATA_BLOCKED Frame (0x14)
    struct DataBlockedFrame : Frame {
        dp::u64 maximum_data;

        FrameType type() const override { return FrameType::DataBlocked; }

        dp::Vector<dp::u8> serialize() const override {
            dp::Vector<dp::u8> result = {0x14};

            auto bytes = varint_encode(maximum_data);
            result.insert(result.end(), bytes.begin(), bytes.end());

            return result;
        }

        static dp::Res<std::pair<DataBlockedFrame, dp::usize>> parse(const dp::u8 *data, dp::usize size) {
            if (size < 1 || data[0] != 0x14) {
                return dp::result::err(dp::Error::invalid_argument("expected DATA_BLOCKED frame"));
            }

            DataBlockedFrame frame;
            dp::usize offset = 1;

            auto result = varint_decode(data + offset, size - offset);
            if (result.is_err())
                return dp::result::err(result.error());
            frame.maximum_data = result.value().first;
            offset += result.value().second;

            return dp::result::ok(std::make_pair(std::move(frame), offset));
        }
    };

    // STREAM_DATA_BLOCKED Frame (0x15)
    struct StreamDataBlockedFrame : Frame {
        dp::u64 stream_id;
        dp::u64 maximum_stream_data;

        FrameType type() const override { return FrameType::StreamDataBlocked; }

        dp::Vector<dp::u8> serialize() const override {
            dp::Vector<dp::u8> result = {0x15};

            auto bytes = varint_encode(stream_id);
            result.insert(result.end(), bytes.begin(), bytes.end());

            bytes = varint_encode(maximum_stream_data);
            result.insert(result.end(), bytes.begin(), bytes.end());

            return result;
        }

        static dp::Res<std::pair<StreamDataBlockedFrame, dp::usize>> parse(const dp::u8 *data, dp::usize size) {
            if (size < 1 || data[0] != 0x15) {
                return dp::result::err(dp::Error::invalid_argument("expected STREAM_DATA_BLOCKED frame"));
            }

            StreamDataBlockedFrame frame;
            dp::usize offset = 1;

            auto result = varint_decode(data + offset, size - offset);
            if (result.is_err())
                return dp::result::err(result.error());
            frame.stream_id = result.value().first;
            offset += result.value().second;

            result = varint_decode(data + offset, size - offset);
            if (result.is_err())
                return dp::result::err(result.error());
            frame.maximum_stream_data = result.value().first;
            offset += result.value().second;

            return dp::result::ok(std::make_pair(std::move(frame), offset));
        }
    };

    // STREAMS_BLOCKED Frame (0x16 for bidi, 0x17 for uni)
    struct StreamsBlockedFrame : Frame {
        bool unidirectional = false;
        dp::u64 maximum_streams;

        FrameType type() const override {
            return unidirectional ? FrameType::StreamsBlockedUni : FrameType::StreamsBlockedBidi;
        }

        dp::Vector<dp::u8> serialize() const override {
            dp::Vector<dp::u8> result = {static_cast<dp::u8>(unidirectional ? 0x17 : 0x16)};

            auto bytes = varint_encode(maximum_streams);
            result.insert(result.end(), bytes.begin(), bytes.end());

            return result;
        }

        static dp::Res<std::pair<StreamsBlockedFrame, dp::usize>> parse(const dp::u8 *data, dp::usize size) {
            if (size < 1 || (data[0] != 0x16 && data[0] != 0x17)) {
                return dp::result::err(dp::Error::invalid_argument("expected STREAMS_BLOCKED frame"));
            }

            StreamsBlockedFrame frame;
            frame.unidirectional = (data[0] == 0x17);
            dp::usize offset = 1;

            auto result = varint_decode(data + offset, size - offset);
            if (result.is_err())
                return dp::result::err(result.error());
            frame.maximum_streams = result.value().first;
            offset += result.value().second;

            return dp::result::ok(std::make_pair(std::move(frame), offset));
        }
    };

    // NEW_CONNECTION_ID Frame (0x18)
    struct NewConnectionIdFrame : Frame {
        dp::u64 sequence_number;
        dp::u64 retire_prior_to;
        ConnectionId connection_id;
        dp::Vector<dp::u8> stateless_reset_token; // 16 bytes

        static constexpr dp::usize RESET_TOKEN_LENGTH = 16;

        FrameType type() const override { return FrameType::NewConnectionId; }

        dp::Vector<dp::u8> serialize() const override {
            dp::Vector<dp::u8> result = {0x18};

            auto bytes = varint_encode(sequence_number);
            result.insert(result.end(), bytes.begin(), bytes.end());

            bytes = varint_encode(retire_prior_to);
            result.insert(result.end(), bytes.begin(), bytes.end());

            result.push_back(static_cast<dp::u8>(connection_id.size()));
            result.insert(result.end(), connection_id.data.begin(), connection_id.data.end());

            result.insert(result.end(), stateless_reset_token.begin(), stateless_reset_token.end());

            return result;
        }

        static dp::Res<std::pair<NewConnectionIdFrame, dp::usize>> parse(const dp::u8 *data, dp::usize size) {
            if (size < 1 || data[0] != 0x18) {
                return dp::result::err(dp::Error::invalid_argument("expected NEW_CONNECTION_ID frame"));
            }

            NewConnectionIdFrame frame;
            dp::usize offset = 1;

            auto result = varint_decode(data + offset, size - offset);
            if (result.is_err())
                return dp::result::err(result.error());
            frame.sequence_number = result.value().first;
            offset += result.value().second;

            result = varint_decode(data + offset, size - offset);
            if (result.is_err())
                return dp::result::err(result.error());
            frame.retire_prior_to = result.value().first;
            offset += result.value().second;

            if (offset >= size) {
                return dp::result::err(dp::Error::invalid_argument("NEW_CONNECTION_ID truncated"));
            }
            dp::usize cid_len = data[offset++];
            if (cid_len > MAX_CID_LENGTH || offset + cid_len + RESET_TOKEN_LENGTH > size) {
                return dp::result::err(dp::Error::invalid_argument("invalid connection ID length"));
            }

            frame.connection_id = ConnectionId::from_bytes(data + offset, cid_len);
            offset += cid_len;

            frame.stateless_reset_token = dp::Vector<dp::u8>(data + offset, data + offset + RESET_TOKEN_LENGTH);
            offset += RESET_TOKEN_LENGTH;

            return dp::result::ok(std::make_pair(std::move(frame), offset));
        }
    };

    // RETIRE_CONNECTION_ID Frame (0x19)
    struct RetireConnectionIdFrame : Frame {
        dp::u64 sequence_number;

        FrameType type() const override { return FrameType::RetireConnectionId; }

        dp::Vector<dp::u8> serialize() const override {
            dp::Vector<dp::u8> result = {0x19};

            auto bytes = varint_encode(sequence_number);
            result.insert(result.end(), bytes.begin(), bytes.end());

            return result;
        }

        static dp::Res<std::pair<RetireConnectionIdFrame, dp::usize>> parse(const dp::u8 *data, dp::usize size) {
            if (size < 1 || data[0] != 0x19) {
                return dp::result::err(dp::Error::invalid_argument("expected RETIRE_CONNECTION_ID frame"));
            }

            RetireConnectionIdFrame frame;
            dp::usize offset = 1;

            auto result = varint_decode(data + offset, size - offset);
            if (result.is_err())
                return dp::result::err(result.error());
            frame.sequence_number = result.value().first;
            offset += result.value().second;

            return dp::result::ok(std::make_pair(std::move(frame), offset));
        }
    };

    // PATH_CHALLENGE Frame (0x1a)
    struct PathChallengeFrame : Frame {
        dp::Vector<dp::u8> data; // 8 bytes

        static constexpr dp::usize DATA_LENGTH = 8;

        FrameType type() const override { return FrameType::PathChallenge; }

        dp::Vector<dp::u8> serialize() const override {
            dp::Vector<dp::u8> result = {0x1a};
            result.insert(result.end(), data.begin(), data.end());
            return result;
        }

        static dp::Res<std::pair<PathChallengeFrame, dp::usize>> parse(const dp::u8 *data, dp::usize size) {
            if (size < 1 + DATA_LENGTH || data[0] != 0x1a) {
                return dp::result::err(dp::Error::invalid_argument("expected PATH_CHALLENGE frame"));
            }

            PathChallengeFrame frame;
            frame.data = dp::Vector<dp::u8>(data + 1, data + 1 + DATA_LENGTH);

            return dp::result::ok(std::make_pair(std::move(frame), static_cast<dp::usize>(1 + DATA_LENGTH)));
        }
    };

    // PATH_RESPONSE Frame (0x1b)
    struct PathResponseFrame : Frame {
        dp::Vector<dp::u8> data; // 8 bytes

        static constexpr dp::usize DATA_LENGTH = 8;

        FrameType type() const override { return FrameType::PathResponse; }

        dp::Vector<dp::u8> serialize() const override {
            dp::Vector<dp::u8> result = {0x1b};
            result.insert(result.end(), data.begin(), data.end());
            return result;
        }

        static dp::Res<std::pair<PathResponseFrame, dp::usize>> parse(const dp::u8 *data, dp::usize size) {
            if (size < 1 + DATA_LENGTH || data[0] != 0x1b) {
                return dp::result::err(dp::Error::invalid_argument("expected PATH_RESPONSE frame"));
            }

            PathResponseFrame frame;
            frame.data = dp::Vector<dp::u8>(data + 1, data + 1 + DATA_LENGTH);

            return dp::result::ok(std::make_pair(std::move(frame), static_cast<dp::usize>(1 + DATA_LENGTH)));
        }
    };

    // CONNECTION_CLOSE Frame (0x1c for QUIC errors, 0x1d for application errors)
    struct ConnectionCloseFrame : Frame {
        bool is_application_error = false;
        dp::u64 error_code;
        dp::u64 frame_type = 0; // Only for QUIC errors (0x1c)
        dp::String reason_phrase;

        FrameType type() const override {
            return is_application_error ? FrameType::ConnectionCloseApp : FrameType::ConnectionClose;
        }

        dp::Vector<dp::u8> serialize() const override {
            dp::Vector<dp::u8> result = {static_cast<dp::u8>(is_application_error ? 0x1d : 0x1c)};

            auto bytes = varint_encode(error_code);
            result.insert(result.end(), bytes.begin(), bytes.end());

            if (!is_application_error) {
                bytes = varint_encode(frame_type);
                result.insert(result.end(), bytes.begin(), bytes.end());
            }

            bytes = varint_encode(reason_phrase.size());
            result.insert(result.end(), bytes.begin(), bytes.end());

            result.insert(result.end(), reason_phrase.begin(), reason_phrase.end());

            return result;
        }

        static dp::Res<std::pair<ConnectionCloseFrame, dp::usize>> parse(const dp::u8 *data, dp::usize size) {
            if (size < 1 || (data[0] != 0x1c && data[0] != 0x1d)) {
                return dp::result::err(dp::Error::invalid_argument("expected CONNECTION_CLOSE frame"));
            }

            ConnectionCloseFrame frame;
            frame.is_application_error = (data[0] == 0x1d);
            dp::usize offset = 1;

            auto result = varint_decode(data + offset, size - offset);
            if (result.is_err())
                return dp::result::err(result.error());
            frame.error_code = result.value().first;
            offset += result.value().second;

            if (!frame.is_application_error) {
                result = varint_decode(data + offset, size - offset);
                if (result.is_err())
                    return dp::result::err(result.error());
                frame.frame_type = result.value().first;
                offset += result.value().second;
            }

            result = varint_decode(data + offset, size - offset);
            if (result.is_err())
                return dp::result::err(result.error());
            dp::u64 reason_len = result.value().first;
            offset += result.value().second;

            if (offset + reason_len > size) {
                return dp::result::err(dp::Error::invalid_argument("CONNECTION_CLOSE reason truncated"));
            }

            frame.reason_phrase = dp::String(reinterpret_cast<const char *>(data + offset), reason_len);
            offset += reason_len;

            return dp::result::ok(std::make_pair(std::move(frame), offset));
        }
    };

    // HANDSHAKE_DONE Frame (0x1e)
    // Sent by server to signal handshake confirmation
    struct HandshakeDoneFrame : Frame {
        FrameType type() const override { return FrameType::HandshakeDone; }

        dp::Vector<dp::u8> serialize() const override { return {0x1e}; }

        static dp::Res<std::pair<HandshakeDoneFrame, dp::usize>> parse(const dp::u8 *data, dp::usize size) {
            if (size < 1 || data[0] != 0x1e) {
                return dp::result::err(dp::Error::invalid_argument("expected HANDSHAKE_DONE frame"));
            }
            return dp::result::ok(std::make_pair(HandshakeDoneFrame{}, static_cast<dp::usize>(1)));
        }
    };

    // Get frame type from first byte (for dispatch)
    inline dp::u64 get_frame_type(dp::u8 first_byte) {
        // STREAM frames have type 0x08-0x0f
        if (is_stream_frame(first_byte)) {
            return static_cast<dp::u64>(FrameType::StreamBase);
        }
        return first_byte;
    }

} // namespace netpipe::quic

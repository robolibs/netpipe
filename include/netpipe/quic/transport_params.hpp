#pragma once

#include <datapod/datapod.hpp>
#include <netpipe/quic/types.hpp>
#include <netpipe/quic/varint.hpp>

namespace netpipe::quic {

    // Transport Parameter IDs (RFC 9000 Section 18.2)
    enum class TransportParameterId : dp::u64 {
        OriginalDestinationConnectionId = 0x00,
        MaxIdleTimeout = 0x01,
        StatelessResetToken = 0x02,
        MaxUdpPayloadSize = 0x03,
        InitialMaxData = 0x04,
        InitialMaxStreamDataBidiLocal = 0x05,
        InitialMaxStreamDataBidiRemote = 0x06,
        InitialMaxStreamDataUni = 0x07,
        InitialMaxStreamsBidi = 0x08,
        InitialMaxStreamsUni = 0x09,
        AckDelayExponent = 0x0a,
        MaxAckDelay = 0x0b,
        DisableActiveMigration = 0x0c,
        PreferredAddress = 0x0d,
        ActiveConnectionIdLimit = 0x0e,
        InitialSourceConnectionId = 0x0f,
        RetrySourceConnectionId = 0x10,
        // Greasing values in the range 27742..262143
        // 0x1c..0x1d: Reserved for version negotiation
    };

    // Default values for transport parameters
    constexpr dp::u64 DEFAULT_MAX_IDLE_TIMEOUT = 30000;         // 30 seconds in milliseconds
    constexpr dp::u64 DEFAULT_MAX_UDP_PAYLOAD_SIZE = 65527;     // Maximum UDP payload
    constexpr dp::u64 DEFAULT_INITIAL_MAX_DATA = 1048576;       // 1 MB
    constexpr dp::u64 DEFAULT_INITIAL_MAX_STREAM_DATA = 262144; // 256 KB
    constexpr dp::u64 DEFAULT_INITIAL_MAX_STREAMS_BIDI = 100;
    constexpr dp::u64 DEFAULT_INITIAL_MAX_STREAMS_UNI = 100;
    constexpr dp::u64 DEFAULT_ACK_DELAY_EXPONENT = 3; // 2^3 = 8 microseconds
    constexpr dp::u64 DEFAULT_MAX_ACK_DELAY = 25;     // 25 milliseconds
    constexpr dp::u64 DEFAULT_ACTIVE_CID_LIMIT = 2;

    // Minimum values
    constexpr dp::u64 MIN_MAX_UDP_PAYLOAD_SIZE = 1200;
    constexpr dp::u64 MAX_ACK_DELAY_EXPONENT = 20;
    constexpr dp::u64 MAX_MAX_ACK_DELAY = 16384; // 2^14 ms

    // Preferred Address structure
    struct PreferredAddress {
        dp::Vector<dp::u8> ipv4_address; // 4 bytes
        dp::u16 ipv4_port = 0;
        dp::Vector<dp::u8> ipv6_address; // 16 bytes
        dp::u16 ipv6_port = 0;
        ConnectionId connection_id;
        dp::Vector<dp::u8> stateless_reset_token; // 16 bytes

        dp::Vector<dp::u8> serialize() const {
            dp::Vector<dp::u8> result;

            // IPv4 address (4 bytes) + port (2 bytes)
            if (ipv4_address.size() == 4) {
                result.insert(result.end(), ipv4_address.begin(), ipv4_address.end());
            } else {
                result.insert(result.end(), 4, 0);
            }
            result.push_back(static_cast<dp::u8>((ipv4_port >> 8) & 0xFF));
            result.push_back(static_cast<dp::u8>(ipv4_port & 0xFF));

            // IPv6 address (16 bytes) + port (2 bytes)
            if (ipv6_address.size() == 16) {
                result.insert(result.end(), ipv6_address.begin(), ipv6_address.end());
            } else {
                result.insert(result.end(), 16, 0);
            }
            result.push_back(static_cast<dp::u8>((ipv6_port >> 8) & 0xFF));
            result.push_back(static_cast<dp::u8>(ipv6_port & 0xFF));

            // Connection ID
            result.push_back(static_cast<dp::u8>(connection_id.size()));
            result.insert(result.end(), connection_id.data.begin(), connection_id.data.end());

            // Stateless Reset Token (16 bytes)
            result.insert(result.end(), stateless_reset_token.begin(), stateless_reset_token.end());

            return result;
        }

        static dp::Res<PreferredAddress> parse(const dp::u8 *data, dp::usize size) {
            // Minimum size: 4 + 2 + 16 + 2 + 1 + 16 = 41 bytes (with empty CID)
            if (size < 41) {
                return dp::result::err(dp::Error::invalid_argument("preferred address too short"));
            }

            PreferredAddress addr;
            dp::usize offset = 0;

            // IPv4
            addr.ipv4_address = dp::Vector<dp::u8>(data + offset, data + offset + 4);
            offset += 4;
            addr.ipv4_port = (static_cast<dp::u16>(data[offset]) << 8) | data[offset + 1];
            offset += 2;

            // IPv6
            addr.ipv6_address = dp::Vector<dp::u8>(data + offset, data + offset + 16);
            offset += 16;
            addr.ipv6_port = (static_cast<dp::u16>(data[offset]) << 8) | data[offset + 1];
            offset += 2;

            // Connection ID
            dp::usize cid_len = data[offset++];
            if (offset + cid_len + 16 > size) {
                return dp::result::err(dp::Error::invalid_argument("preferred address CID truncated"));
            }
            addr.connection_id = ConnectionId::from_bytes(data + offset, cid_len);
            offset += cid_len;

            // Stateless Reset Token
            addr.stateless_reset_token = dp::Vector<dp::u8>(data + offset, data + offset + 16);

            return dp::result::ok(std::move(addr));
        }
    };

    // Transport Parameters
    struct TransportParameters {
        // Original destination connection ID (server only, from Initial packet)
        dp::Optional<ConnectionId> original_dest_cid;

        // Maximum idle timeout (milliseconds, 0 = disabled)
        dp::u64 max_idle_timeout = DEFAULT_MAX_IDLE_TIMEOUT;

        // Stateless reset token (server only)
        dp::Optional<dp::Vector<dp::u8>> stateless_reset_token;

        // Maximum UDP payload size
        dp::u64 max_udp_payload_size = DEFAULT_MAX_UDP_PAYLOAD_SIZE;

        // Initial flow control limits
        dp::u64 initial_max_data = DEFAULT_INITIAL_MAX_DATA;
        dp::u64 initial_max_stream_data_bidi_local = DEFAULT_INITIAL_MAX_STREAM_DATA;
        dp::u64 initial_max_stream_data_bidi_remote = DEFAULT_INITIAL_MAX_STREAM_DATA;
        dp::u64 initial_max_stream_data_uni = DEFAULT_INITIAL_MAX_STREAM_DATA;
        dp::u64 initial_max_streams_bidi = DEFAULT_INITIAL_MAX_STREAMS_BIDI;
        dp::u64 initial_max_streams_uni = DEFAULT_INITIAL_MAX_STREAMS_UNI;

        // ACK delay parameters
        dp::u64 ack_delay_exponent = DEFAULT_ACK_DELAY_EXPONENT;
        dp::u64 max_ack_delay = DEFAULT_MAX_ACK_DELAY;

        // Disable active connection migration
        bool disable_active_migration = false;

        // Preferred address (server only)
        dp::Optional<PreferredAddress> preferred_address;

        // Active connection ID limit
        dp::u64 active_connection_id_limit = DEFAULT_ACTIVE_CID_LIMIT;

        // Initial source connection ID
        dp::Optional<ConnectionId> initial_source_cid;

        // Retry source connection ID (server only, after Retry)
        dp::Optional<ConnectionId> retry_source_cid;

        // Serialize transport parameters for TLS extension
        dp::Vector<dp::u8> serialize() const {
            dp::Vector<dp::u8> result;

            auto append_param = [&result](TransportParameterId id, const dp::Vector<dp::u8> &value) {
                auto id_bytes = varint_encode(static_cast<dp::u64>(id));
                result.insert(result.end(), id_bytes.begin(), id_bytes.end());

                auto len_bytes = varint_encode(value.size());
                result.insert(result.end(), len_bytes.begin(), len_bytes.end());

                result.insert(result.end(), value.begin(), value.end());
            };

            auto append_varint_param = [&append_param](TransportParameterId id, dp::u64 value) {
                append_param(id, varint_encode(value));
            };

            auto append_empty_param = [&result](TransportParameterId id) {
                auto id_bytes = varint_encode(static_cast<dp::u64>(id));
                result.insert(result.end(), id_bytes.begin(), id_bytes.end());
                result.push_back(0); // Zero length
            };

            // Original destination connection ID
            if (original_dest_cid.has_value()) {
                append_param(TransportParameterId::OriginalDestinationConnectionId, original_dest_cid.value().data);
            }

            // Max idle timeout
            if (max_idle_timeout > 0) {
                append_varint_param(TransportParameterId::MaxIdleTimeout, max_idle_timeout);
            }

            // Stateless reset token
            if (stateless_reset_token.has_value()) {
                append_param(TransportParameterId::StatelessResetToken, stateless_reset_token.value());
            }

            // Max UDP payload size (only if different from default)
            if (max_udp_payload_size != DEFAULT_MAX_UDP_PAYLOAD_SIZE) {
                append_varint_param(TransportParameterId::MaxUdpPayloadSize, max_udp_payload_size);
            }

            // Flow control parameters
            append_varint_param(TransportParameterId::InitialMaxData, initial_max_data);
            append_varint_param(TransportParameterId::InitialMaxStreamDataBidiLocal,
                                initial_max_stream_data_bidi_local);
            append_varint_param(TransportParameterId::InitialMaxStreamDataBidiRemote,
                                initial_max_stream_data_bidi_remote);
            append_varint_param(TransportParameterId::InitialMaxStreamDataUni, initial_max_stream_data_uni);
            append_varint_param(TransportParameterId::InitialMaxStreamsBidi, initial_max_streams_bidi);
            append_varint_param(TransportParameterId::InitialMaxStreamsUni, initial_max_streams_uni);

            // ACK delay parameters (only if different from default)
            if (ack_delay_exponent != DEFAULT_ACK_DELAY_EXPONENT) {
                append_varint_param(TransportParameterId::AckDelayExponent, ack_delay_exponent);
            }
            if (max_ack_delay != DEFAULT_MAX_ACK_DELAY) {
                append_varint_param(TransportParameterId::MaxAckDelay, max_ack_delay);
            }

            // Disable active migration
            if (disable_active_migration) {
                append_empty_param(TransportParameterId::DisableActiveMigration);
            }

            // Preferred address
            if (preferred_address.has_value()) {
                append_param(TransportParameterId::PreferredAddress, preferred_address.value().serialize());
            }

            // Active connection ID limit
            append_varint_param(TransportParameterId::ActiveConnectionIdLimit, active_connection_id_limit);

            // Initial source connection ID
            if (initial_source_cid.has_value()) {
                append_param(TransportParameterId::InitialSourceConnectionId, initial_source_cid.value().data);
            }

            // Retry source connection ID
            if (retry_source_cid.has_value()) {
                append_param(TransportParameterId::RetrySourceConnectionId, retry_source_cid.value().data);
            }

            return result;
        }

        // Parse transport parameters from TLS extension data
        static dp::Res<TransportParameters> parse(const dp::u8 *data, dp::usize size) {
            TransportParameters params;
            dp::usize offset = 0;

            while (offset < size) {
                // Parameter ID (varint)
                auto id_result = varint_decode(data + offset, size - offset);
                if (id_result.is_err()) {
                    return dp::result::err(id_result.error());
                }
                auto [id, id_len] = id_result.value();
                offset += id_len;

                // Parameter length (varint)
                auto len_result = varint_decode(data + offset, size - offset);
                if (len_result.is_err()) {
                    return dp::result::err(len_result.error());
                }
                auto [param_len, len_len] = len_result.value();
                offset += len_len;

                if (offset + param_len > size) {
                    return dp::result::err(dp::Error::invalid_argument("transport parameter truncated"));
                }

                const dp::u8 *param_data = data + offset;

                // Parse based on ID
                switch (static_cast<TransportParameterId>(id)) {
                case TransportParameterId::OriginalDestinationConnectionId:
                    params.original_dest_cid = ConnectionId::from_bytes(param_data, param_len);
                    break;

                case TransportParameterId::MaxIdleTimeout: {
                    auto val_result = varint_decode(param_data, param_len);
                    if (val_result.is_ok()) {
                        params.max_idle_timeout = val_result.value().first;
                    }
                    break;
                }

                case TransportParameterId::StatelessResetToken:
                    if (param_len == 16) {
                        params.stateless_reset_token = dp::Vector<dp::u8>(param_data, param_data + param_len);
                    }
                    break;

                case TransportParameterId::MaxUdpPayloadSize: {
                    auto val_result = varint_decode(param_data, param_len);
                    if (val_result.is_ok()) {
                        params.max_udp_payload_size = std::max(val_result.value().first, MIN_MAX_UDP_PAYLOAD_SIZE);
                    }
                    break;
                }

                case TransportParameterId::InitialMaxData: {
                    auto val_result = varint_decode(param_data, param_len);
                    if (val_result.is_ok()) {
                        params.initial_max_data = val_result.value().first;
                    }
                    break;
                }

                case TransportParameterId::InitialMaxStreamDataBidiLocal: {
                    auto val_result = varint_decode(param_data, param_len);
                    if (val_result.is_ok()) {
                        params.initial_max_stream_data_bidi_local = val_result.value().first;
                    }
                    break;
                }

                case TransportParameterId::InitialMaxStreamDataBidiRemote: {
                    auto val_result = varint_decode(param_data, param_len);
                    if (val_result.is_ok()) {
                        params.initial_max_stream_data_bidi_remote = val_result.value().first;
                    }
                    break;
                }

                case TransportParameterId::InitialMaxStreamDataUni: {
                    auto val_result = varint_decode(param_data, param_len);
                    if (val_result.is_ok()) {
                        params.initial_max_stream_data_uni = val_result.value().first;
                    }
                    break;
                }

                case TransportParameterId::InitialMaxStreamsBidi: {
                    auto val_result = varint_decode(param_data, param_len);
                    if (val_result.is_ok()) {
                        params.initial_max_streams_bidi = val_result.value().first;
                    }
                    break;
                }

                case TransportParameterId::InitialMaxStreamsUni: {
                    auto val_result = varint_decode(param_data, param_len);
                    if (val_result.is_ok()) {
                        params.initial_max_streams_uni = val_result.value().first;
                    }
                    break;
                }

                case TransportParameterId::AckDelayExponent: {
                    auto val_result = varint_decode(param_data, param_len);
                    if (val_result.is_ok()) {
                        dp::u64 exp = val_result.value().first;
                        params.ack_delay_exponent = std::min(exp, MAX_ACK_DELAY_EXPONENT);
                    }
                    break;
                }

                case TransportParameterId::MaxAckDelay: {
                    auto val_result = varint_decode(param_data, param_len);
                    if (val_result.is_ok()) {
                        dp::u64 delay = val_result.value().first;
                        params.max_ack_delay = std::min(delay, MAX_MAX_ACK_DELAY);
                    }
                    break;
                }

                case TransportParameterId::DisableActiveMigration:
                    params.disable_active_migration = true;
                    break;

                case TransportParameterId::PreferredAddress: {
                    auto addr_result = PreferredAddress::parse(param_data, param_len);
                    if (addr_result.is_ok()) {
                        params.preferred_address = std::move(addr_result.value());
                    }
                    break;
                }

                case TransportParameterId::ActiveConnectionIdLimit: {
                    auto val_result = varint_decode(param_data, param_len);
                    if (val_result.is_ok()) {
                        params.active_connection_id_limit = std::max(val_result.value().first, 2ULL);
                    }
                    break;
                }

                case TransportParameterId::InitialSourceConnectionId:
                    params.initial_source_cid = ConnectionId::from_bytes(param_data, param_len);
                    break;

                case TransportParameterId::RetrySourceConnectionId:
                    params.retry_source_cid = ConnectionId::from_bytes(param_data, param_len);
                    break;

                default:
                    // Unknown parameter - ignore (for forward compatibility)
                    break;
                }

                offset += param_len;
            }

            return dp::result::ok(std::move(params));
        }

        // Validate parameters
        dp::Res<void> validate() const {
            if (max_udp_payload_size < MIN_MAX_UDP_PAYLOAD_SIZE) {
                return dp::result::err(dp::Error::invalid_argument("max_udp_payload_size below minimum"));
            }

            if (ack_delay_exponent > MAX_ACK_DELAY_EXPONENT) {
                return dp::result::err(dp::Error::invalid_argument("ack_delay_exponent too large"));
            }

            if (max_ack_delay > MAX_MAX_ACK_DELAY) {
                return dp::result::err(dp::Error::invalid_argument("max_ack_delay too large"));
            }

            if (active_connection_id_limit < 2) {
                return dp::result::err(dp::Error::invalid_argument("active_connection_id_limit must be at least 2"));
            }

            return dp::result::ok();
        }
    };

    // QUIC transport parameters TLS extension type
    constexpr dp::u16 QUIC_TRANSPORT_PARAMS_EXTENSION = 0x39;

} // namespace netpipe::quic

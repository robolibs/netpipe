#pragma once

#include <cstring>

#include <netpipe/core/common.hpp>
#include <netpipe/protocol/http2/frame.hpp>

namespace netpipe::http2 {

    enum class SettingsId : dp::u16 {
        HeaderTableSize = 0x1,
        EnablePush = 0x2,
        MaxConcurrentStreams = 0x3,
        InitialWindowSize = 0x4,
        MaxFrameSize = 0x5,
        MaxHeaderListSize = 0x6
    };

    struct Settings {
        dp::Map<dp::u16, dp::u32> values;

        void set(SettingsId id, dp::u32 value) { values[static_cast<dp::u16>(id)] = value; }

        dp::Optional<dp::u32> get(SettingsId id) const {
            auto it = values.find(static_cast<dp::u16>(id));
            if (it == values.end()) {
                return dp::nullopt;
            }
            return it->second;
        }
    };

    inline dp::Vector<dp::u8> serialize_settings_payload(const Settings &settings) {
        dp::Vector<dp::u8> out;
        out.reserve(settings.values.size() * 6);

        for (const auto &[id, value] : settings.values) {
            out.push_back(static_cast<dp::u8>((id >> 8) & 0xFF));
            out.push_back(static_cast<dp::u8>(id & 0xFF));
            out.push_back(static_cast<dp::u8>((value >> 24) & 0xFF));
            out.push_back(static_cast<dp::u8>((value >> 16) & 0xFF));
            out.push_back(static_cast<dp::u8>((value >> 8) & 0xFF));
            out.push_back(static_cast<dp::u8>(value & 0xFF));
        }

        return out;
    }

    inline dp::Result<Settings> parse_settings_payload(const dp::Vector<dp::u8> &payload) {
        if (payload.size() % 6 != 0) {
            return dp::result::err(dp::Error::invalid_argument("HTTP/2 SETTINGS payload must be multiple of 6 bytes"));
        }

        Settings settings;
        for (dp::usize i = 0; i < payload.size(); i += 6) {
            dp::u16 id = (static_cast<dp::u16>(payload[i]) << 8) | payload[i + 1];
            dp::u32 value = (static_cast<dp::u32>(payload[i + 2]) << 24) |
                            (static_cast<dp::u32>(payload[i + 3]) << 16) | (static_cast<dp::u32>(payload[i + 4]) << 8) |
                            static_cast<dp::u32>(payload[i + 5]);
            settings.values[id] = value;
        }

        return dp::result::ok(std::move(settings));
    }

    inline Frame make_settings_frame(const Settings &settings) {
        Frame frame;
        frame.header.type = FrameType::Settings;
        frame.header.stream_id = 0;
        frame.payload = serialize_settings_payload(settings);
        return frame;
    }

    inline Frame make_settings_ack_frame() {
        Frame frame;
        frame.header.type = FrameType::Settings;
        frame.header.flags = 0x1; // ACK
        frame.header.stream_id = 0;
        return frame;
    }

    class SettingsStateMachine {
      public:
        explicit SettingsStateMachine(bool is_client) : is_client_(is_client) {}

        dp::Result<netpipe::Message> start_client_preface(const Settings &local_settings) {
            if (!is_client_) {
                return dp::result::err(dp::Error::invalid_argument("only client can start with HTTP/2 preface"));
            }
            if (state_ != ConnectionState::Idle) {
                return dp::result::err(dp::Error::invalid_argument("invalid state for client preface"));
            }

            auto settings_frame = serialize_frame(make_settings_frame(local_settings));
            if (settings_frame.is_err()) {
                return dp::result::err(settings_frame.error());
            }

            netpipe::Message out;
            out.insert(out.end(), PREFACE, PREFACE + std::strlen(PREFACE));
            out.insert(out.end(), settings_frame.value().begin(), settings_frame.value().end());

            local_settings_sent_ = true;
            state_ = ConnectionState::PrefaceSent;
            return dp::result::ok(std::move(out));
        }

        dp::Result<void> process_client_preface(const netpipe::Message &preface) {
            if (is_client_) {
                return dp::result::err(dp::Error::invalid_argument("client must not process client preface"));
            }
            if (state_ != ConnectionState::Idle) {
                return dp::result::err(dp::Error::invalid_argument("invalid state for processing client preface"));
            }
            if (preface.size() != std::strlen(PREFACE) || std::memcmp(preface.data(), PREFACE, preface.size()) != 0) {
                return dp::result::err(dp::Error::invalid_argument("invalid HTTP/2 client preface"));
            }

            state_ = ConnectionState::PrefaceReceived;
            return dp::result::ok();
        }

        dp::Result<netpipe::Message> create_server_settings(const Settings &local_settings) {
            if (is_client_) {
                return dp::result::err(dp::Error::invalid_argument("client cannot create server SETTINGS frame"));
            }
            if (state_ != ConnectionState::PrefaceReceived) {
                return dp::result::err(
                    dp::Error::invalid_argument("server must receive preface before sending settings"));
            }

            auto settings_frame = serialize_frame(make_settings_frame(local_settings));
            if (settings_frame.is_err()) {
                return dp::result::err(settings_frame.error());
            }

            local_settings_sent_ = true;
            return dp::result::ok(std::move(settings_frame.value()));
        }

        dp::Result<dp::Optional<netpipe::Message>> process_incoming_frame(const Frame &frame) {
            if (frame.header.type != FrameType::Settings) {
                return dp::result::err(dp::Error::invalid_argument("expected SETTINGS frame during startup"));
            }
            if (frame.header.stream_id != 0) {
                return dp::result::err(dp::Error::invalid_argument("SETTINGS must use stream 0"));
            }

            const bool is_ack = (frame.header.flags & 0x1) != 0;

            if (is_ack) {
                if (!frame.payload.empty()) {
                    return dp::result::err(dp::Error::invalid_argument("SETTINGS ACK must have empty payload"));
                }
                if (!local_settings_sent_) {
                    return dp::result::err(
                        dp::Error::invalid_argument("received SETTINGS ACK before sending local SETTINGS"));
                }
                local_settings_acked_ = true;
                update_state();
                return dp::result::ok(dp::Optional<netpipe::Message>{});
            }

            auto parsed = parse_settings_payload(frame.payload);
            if (parsed.is_err()) {
                return dp::result::err(parsed.error());
            }
            peer_settings_ = parsed.value();
            peer_settings_received_ = true;

            auto ack = serialize_frame(make_settings_ack_frame());
            if (ack.is_err()) {
                return dp::result::err(ack.error());
            }

            update_state();
            return dp::result::ok(dp::Optional<netpipe::Message>(std::move(ack.value())));
        }

        ConnectionState state() const { return state_; }
        bool local_settings_acked() const { return local_settings_acked_; }
        bool peer_settings_received() const { return peer_settings_received_; }
        const Settings &peer_settings() const { return peer_settings_; }

      private:
        void update_state() {
            if (local_settings_acked_ && peer_settings_received_) {
                state_ = ConnectionState::SettingsExchanged;
            }
        }

        bool is_client_ = false;
        ConnectionState state_ = ConnectionState::Idle;
        bool local_settings_sent_ = false;
        bool local_settings_acked_ = false;
        bool peer_settings_received_ = false;
        Settings peer_settings_;
    };

} // namespace netpipe::http2

#pragma once

#include <cstring>

#include <netpipe/protocol/http2/flow_control.hpp>
#include <netpipe/protocol/http2/hpack.hpp>
#include <netpipe/protocol/http2/settings.hpp>
#include <netpipe/protocol/http2/shutdown.hpp>
#include <netpipe/protocol/http2/stream.hpp>

namespace netpipe::http2 {

    enum class InboundEventType { Settings, Headers, Data, RstStream, GoAway };

    struct InboundEvent {
        InboundEventType type = InboundEventType::Settings;
        dp::u32 stream_id = 0;
        bool end_stream = false;
        http::HeaderList headers;
        dp::Vector<dp::u8> data;
        dp::Optional<Settings> settings;
        dp::Optional<ErrorCode> rst_error;
        dp::Optional<GoAway> goaway;
    };

    class Connection {
      public:
        explicit Connection(bool is_client) : is_client_(is_client), settings_(is_client) {}

        bool is_client() const { return is_client_; }
        ConnectionState state() const { return settings_.state(); }

        dp::Result<void> start(const Settings &local_settings = {}) {
            if (!is_client_) {
                return dp::result::err(dp::Error::invalid_argument("server connection cannot call start"));
            }

            auto start_result = settings_.start_client_preface(local_settings);
            if (start_result.is_err()) {
                return dp::result::err(start_result.error());
            }

            outbound_.push_back(std::move(start_result.value()));
            return dp::result::ok();
        }

        dp::Result<void> accept_client_preface(const netpipe::Message &preface) {
            if (is_client_) {
                return dp::result::err(dp::Error::invalid_argument("client connection cannot accept client preface"));
            }

            auto result = settings_.process_client_preface(preface);
            if (result.is_err()) {
                return dp::result::err(result.error());
            }
            return dp::result::ok();
        }

        dp::Result<void> send_server_settings(const Settings &local_settings = {}) {
            if (is_client_) {
                return dp::result::err(dp::Error::invalid_argument("client connection cannot send server settings"));
            }

            auto settings_bytes = settings_.create_server_settings(local_settings);
            if (settings_bytes.is_err()) {
                return dp::result::err(settings_bytes.error());
            }

            outbound_.push_back(std::move(settings_bytes.value()));
            return dp::result::ok();
        }

        dp::Result<void> process_inbound_frame(const Frame &frame) {
            switch (frame.header.type) {
            case FrameType::Settings: {
                auto ack = settings_.process_incoming_frame(frame);
                if (ack.is_err()) {
                    return dp::result::err(ack.error());
                }

                if (ack.value().has_value()) {
                    outbound_.push_back(std::move(ack.value().value()));
                }

                InboundEvent event;
                event.type = InboundEventType::Settings;
                if ((frame.header.flags & 0x1) == 0) {
                    auto parsed = parse_settings_payload(frame.payload);
                    if (parsed.is_ok()) {
                        event.settings = parsed.value();
                    }
                }
                events_.push_back(std::move(event));
                return dp::result::ok();
            }

            case FrameType::Headers:
            case FrameType::Continuation: {
                auto headers_result = stream_manager_.process_incoming_frame(frame);
                if (headers_result.is_err()) {
                    return dp::result::err(headers_result.error());
                }

                if (headers_result.value().has_value()) {
                    InboundEvent event;
                    event.type = InboundEventType::Headers;
                    event.stream_id = headers_result.value().value().stream_id;
                    event.end_stream = headers_result.value().value().end_stream;
                    event.headers = std::move(headers_result.value().value().headers);

                    shutdown_manager_.on_stream_opened(event.stream_id);
                    if (event.end_stream) {
                        shutdown_manager_.on_stream_closed(event.stream_id);
                    }
                    events_.push_back(std::move(event));
                }
                return dp::result::ok();
            }

            case FrameType::Data: {
                auto stream_result = stream_manager_.process_incoming_frame(frame);
                if (stream_result.is_err()) {
                    return dp::result::err(stream_result.error());
                }

                InboundEvent event;
                event.type = InboundEventType::Data;
                event.stream_id = frame.header.stream_id;
                event.end_stream = (frame.header.flags & 0x1) != 0;
                event.data = frame.payload;
                events_.push_back(std::move(event));

                if ((frame.header.flags & 0x1) != 0) {
                    shutdown_manager_.on_stream_closed(frame.header.stream_id);
                }
                return dp::result::ok();
            }

            case FrameType::RstStream: {
                auto shut = shutdown_manager_.process_incoming(frame);
                if (shut.is_err()) {
                    return dp::result::err(shut.error());
                }

                auto parsed = parse_rst_stream(frame);
                if (parsed.is_err()) {
                    return dp::result::err(parsed.error());
                }

                InboundEvent event;
                event.type = InboundEventType::RstStream;
                event.stream_id = frame.header.stream_id;
                event.rst_error = parsed.value();
                events_.push_back(std::move(event));
                return dp::result::ok();
            }

            case FrameType::GoAway: {
                auto shut = shutdown_manager_.process_incoming(frame);
                if (shut.is_err()) {
                    return dp::result::err(shut.error());
                }

                auto parsed = parse_goaway(frame);
                if (parsed.is_err()) {
                    return dp::result::err(parsed.error());
                }

                InboundEvent event;
                event.type = InboundEventType::GoAway;
                event.goaway = parsed.value();
                events_.push_back(std::move(event));
                return dp::result::ok();
            }

            default:
                return dp::result::ok();
            }
        }

        dp::Result<void> process_inbound_bytes(const netpipe::Message &bytes) {
            dp::usize offset = 0;

            if (!is_client_ && settings_.state() == ConnectionState::Idle) {
                if (bytes.size() < std::strlen(PREFACE)) {
                    return dp::result::err(dp::Error::invalid_argument("inbound bytes shorter than HTTP/2 preface"));
                }

                netpipe::Message preface(bytes.begin(), bytes.begin() + std::strlen(PREFACE));
                auto preface_result = accept_client_preface(preface);
                if (preface_result.is_err()) {
                    return dp::result::err(preface_result.error());
                }
                offset = std::strlen(PREFACE);
            }

            while (offset < bytes.size()) {
                auto parsed = parse_frame(bytes.data() + offset, bytes.size() - offset);
                if (parsed.is_err()) {
                    return dp::result::err(parsed.error());
                }
                auto frame = std::move(parsed.value().first);
                auto consumed = parsed.value().second;
                if (consumed == 0) {
                    return dp::result::err(dp::Error::invalid_argument("HTTP/2 frame parse consumed zero bytes"));
                }

                auto process_result = process_inbound_frame(frame);
                if (process_result.is_err()) {
                    return dp::result::err(process_result.error());
                }

                offset += consumed;
            }

            return dp::result::ok();
        }

        bool has_outbound() const { return !outbound_.empty(); }

        bool has_event() const { return !events_.empty(); }

        dp::Result<netpipe::Message> pop_outbound() {
            if (outbound_.empty()) {
                return dp::result::err(dp::Error::not_found("no outbound bytes queued"));
            }

            netpipe::Message out = std::move(outbound_.front());
            outbound_.erase(outbound_.begin());
            return dp::result::ok(std::move(out));
        }

        dp::Result<InboundEvent> pop_event() {
            if (events_.empty()) {
                return dp::result::err(dp::Error::not_found("no inbound event available"));
            }

            InboundEvent event = std::move(events_.front());
            events_.erase(events_.begin());
            return dp::result::ok(std::move(event));
        }

        HpackContext &header_encoder() { return header_encoder_; }
        HpackContext &header_decoder() { return header_decoder_; }
        FlowController &flow_controller() { return flow_controller_; }
        ShutdownManager &shutdown_manager() { return shutdown_manager_; }
        StreamManager &stream_manager() { return stream_manager_; }

      private:
        bool is_client_ = false;
        SettingsStateMachine settings_;
        StreamManager stream_manager_;
        FlowController flow_controller_;
        ShutdownManager shutdown_manager_;
        HpackContext header_encoder_;
        HpackContext header_decoder_;
        dp::Vector<netpipe::Message> outbound_;
        dp::Vector<InboundEvent> events_;
    };

} // namespace netpipe::http2

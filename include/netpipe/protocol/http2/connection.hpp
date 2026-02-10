#pragma once

#include <cstring>

#include <netpipe/protocol/http2/settings.hpp>

namespace netpipe::http2 {

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
            auto ack = settings_.process_incoming_frame(frame);
            if (ack.is_err()) {
                return dp::result::err(ack.error());
            }

            if (ack.value().has_value()) {
                outbound_.push_back(std::move(ack.value().value()));
            }

            return dp::result::ok();
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

        dp::Result<netpipe::Message> pop_outbound() {
            if (outbound_.empty()) {
                return dp::result::err(dp::Error::not_found("no outbound bytes queued"));
            }

            netpipe::Message out = std::move(outbound_.front());
            outbound_.erase(outbound_.begin());
            return dp::result::ok(std::move(out));
        }

      private:
        bool is_client_ = false;
        SettingsStateMachine settings_;
        dp::Vector<netpipe::Message> outbound_;
    };

} // namespace netpipe::http2

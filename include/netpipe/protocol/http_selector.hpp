#pragma once

#include <netpipe/protocol/http11.hpp>
#include <netpipe/protocol/http2.hpp>
#include <netpipe/protocol/http3.hpp>

namespace netpipe::http {

    enum class Version { Auto, Http11, Http2, Http3 };

    struct SelectorConfig {
        Version preferred = Version::Auto;
        bool allow_http11 = true;
        bool allow_http2 = true;
        bool allow_http3 = true;
        bool require_tls_for_http2 = true;
        bool require_alpn_for_http2 = true;
    };

    struct NegotiatedCapabilities {
        bool tls_active = false;
        bool quic_transport = false;
        dp::Optional<dp::String> alpn_protocol;
    };

    inline dp::String version_name(Version version) {
        switch (version) {
        case Version::Auto:
            return "auto";
        case Version::Http11:
            return "http/1.1";
        case Version::Http2:
            return "h2";
        case Version::Http3:
            return "h3";
        }
        return "auto";
    }

    inline dp::Optional<Version> from_alpn(const dp::String &alpn) {
        if (alpn == "http/1.1") {
            return Version::Http11;
        }
        if (alpn == "h2") {
            return Version::Http2;
        }
        if (alpn == "h3") {
            return Version::Http3;
        }
        return dp::nullopt;
    }

    inline dp::Result<void> validate_version_allowed(Version version, const SelectorConfig &config) {
        if (version == Version::Http11 && !config.allow_http11) {
            return dp::result::err(dp::Error::invalid_argument("HTTP/1.1 is disabled in selector config"));
        }
        if (version == Version::Http2 && !config.allow_http2) {
            return dp::result::err(dp::Error::invalid_argument("HTTP/2 is disabled in selector config"));
        }
        if (version == Version::Http3 && !config.allow_http3) {
            return dp::result::err(dp::Error::invalid_argument("HTTP/3 is disabled in selector config"));
        }
        return dp::result::ok();
    }

    inline dp::Result<Version> select_version(const SelectorConfig &config, const NegotiatedCapabilities &caps) {
        auto choose = [&](Version version) -> dp::Result<Version> {
            auto allowed = validate_version_allowed(version, config);
            if (allowed.is_err()) {
                return dp::result::err(allowed.error());
            }
            if (version == Version::Http2 && config.require_tls_for_http2 && !caps.tls_active) {
                return dp::result::err(dp::Error::invalid_argument("HTTP/2 requires TLS in selector config"));
            }
            if (version == Version::Http3 && !caps.quic_transport) {
                return dp::result::err(dp::Error::invalid_argument("HTTP/3 requires QUIC transport"));
            }
            return dp::result::ok(version);
        };

        if (config.preferred != Version::Auto) {
            return choose(config.preferred);
        }

        if (caps.alpn_protocol.has_value()) {
            auto maybe_version = from_alpn(caps.alpn_protocol.value());
            if (!maybe_version.has_value()) {
                return dp::result::err(dp::Error::invalid_argument("unsupported ALPN protocol for HTTP selector"));
            }
            return choose(maybe_version.value());
        }

        if (caps.quic_transport && config.allow_http3) {
            return choose(Version::Http3);
        }

        if (config.allow_http2 && caps.tls_active && !config.require_alpn_for_http2) {
            return choose(Version::Http2);
        }

        if (config.allow_http11) {
            return choose(Version::Http11);
        }

        return dp::result::err(dp::Error::invalid_argument("no compatible HTTP version available"));
    }

    class ProtocolSelector {
      public:
        explicit ProtocolSelector(SelectorConfig config = {}) : config_(std::move(config)) {}

        void set_capabilities(const NegotiatedCapabilities &caps) { caps_ = caps; }
        void set_config(const SelectorConfig &config) { config_ = config; }

        dp::Result<Version> select() const { return select_version(config_, caps_); }

        dp::Result<http11::ClientConnection> create_http11_client() const {
            auto selected = select();
            if (selected.is_err()) {
                return dp::result::err(selected.error());
            }
            if (selected.value() != Version::Http11) {
                return dp::result::err(dp::Error::invalid_argument("selector did not choose HTTP/1.1"));
            }
            return dp::result::ok(http11::ClientConnection{});
        }

        dp::Result<http2::StreamManager> create_http2_stream_manager() const {
            auto selected = select();
            if (selected.is_err()) {
                return dp::result::err(selected.error());
            }
            if (selected.value() != Version::Http2) {
                return dp::result::err(dp::Error::invalid_argument("selector did not choose HTTP/2"));
            }
            return dp::result::ok(http2::StreamManager{});
        }

      private:
        SelectorConfig config_;
        NegotiatedCapabilities caps_;
    };

} // namespace netpipe::http

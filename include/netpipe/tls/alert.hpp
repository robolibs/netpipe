#pragma once

#include <datapod/datapod.hpp>
#include <echo/echo.hpp>

namespace netpipe::tls {

    // TLS Alert Level
    enum class AlertLevel : dp::u8 { Warning = 1, Fatal = 2 };

    // TLS Alert Description (RFC 8446 Section 6)
    enum class AlertDescription : dp::u8 {
        CloseNotify = 0,
        UnexpectedMessage = 10,
        BadRecordMac = 20,
        RecordOverflow = 22,
        HandshakeFailure = 40,
        BadCertificate = 42,
        UnsupportedCertificate = 43,
        CertificateRevoked = 44,
        CertificateExpired = 45,
        CertificateUnknown = 46,
        IllegalParameter = 47,
        UnknownCa = 48,
        AccessDenied = 49,
        DecodeError = 50,
        DecryptError = 51,
        ProtocolVersion = 70,
        InsufficientSecurity = 71,
        InternalError = 80,
        InappropriateFallback = 86,
        UserCanceled = 90,
        MissingExtension = 109,
        UnsupportedExtension = 110,
        UnrecognizedName = 112,
        BadCertificateStatusResponse = 113,
        UnknownPskIdentity = 115,
        CertificateRequired = 116,
        NoApplicationProtocol = 120
    };

    // Convert AlertDescription to human-readable string
    inline const char *alert_description_to_string(AlertDescription desc) {
        switch (desc) {
        case AlertDescription::CloseNotify:
            return "close_notify";
        case AlertDescription::UnexpectedMessage:
            return "unexpected_message";
        case AlertDescription::BadRecordMac:
            return "bad_record_mac";
        case AlertDescription::RecordOverflow:
            return "record_overflow";
        case AlertDescription::HandshakeFailure:
            return "handshake_failure";
        case AlertDescription::BadCertificate:
            return "bad_certificate";
        case AlertDescription::UnsupportedCertificate:
            return "unsupported_certificate";
        case AlertDescription::CertificateRevoked:
            return "certificate_revoked";
        case AlertDescription::CertificateExpired:
            return "certificate_expired";
        case AlertDescription::CertificateUnknown:
            return "certificate_unknown";
        case AlertDescription::IllegalParameter:
            return "illegal_parameter";
        case AlertDescription::UnknownCa:
            return "unknown_ca";
        case AlertDescription::AccessDenied:
            return "access_denied";
        case AlertDescription::DecodeError:
            return "decode_error";
        case AlertDescription::DecryptError:
            return "decrypt_error";
        case AlertDescription::ProtocolVersion:
            return "protocol_version";
        case AlertDescription::InsufficientSecurity:
            return "insufficient_security";
        case AlertDescription::InternalError:
            return "internal_error";
        case AlertDescription::InappropriateFallback:
            return "inappropriate_fallback";
        case AlertDescription::UserCanceled:
            return "user_canceled";
        case AlertDescription::MissingExtension:
            return "missing_extension";
        case AlertDescription::UnsupportedExtension:
            return "unsupported_extension";
        case AlertDescription::UnrecognizedName:
            return "unrecognized_name";
        case AlertDescription::BadCertificateStatusResponse:
            return "bad_certificate_status_response";
        case AlertDescription::UnknownPskIdentity:
            return "unknown_psk_identity";
        case AlertDescription::CertificateRequired:
            return "certificate_required";
        case AlertDescription::NoApplicationProtocol:
            return "no_application_protocol";
        default:
            return "unknown_alert";
        }
    }

    // TLS Alert message
    struct Alert {
        AlertLevel level;
        AlertDescription description;

        // Serialize to bytes (2 bytes)
        dp::Vector<dp::u8> serialize() const { return {static_cast<dp::u8>(level), static_cast<dp::u8>(description)}; }

        // Parse from bytes
        static dp::Res<Alert> parse(const dp::u8 *data, dp::usize size) {
            if (size < 2) {
                return dp::result::err(dp::Error::invalid_argument("alert too short"));
            }

            Alert alert;
            alert.level = static_cast<AlertLevel>(data[0]);
            alert.description = static_cast<AlertDescription>(data[1]);

            return dp::result::ok(alert);
        }

        static dp::Res<Alert> parse(const dp::Vector<dp::u8> &data) { return parse(data.data(), data.size()); }

        // Check if this is a fatal alert
        bool is_fatal() const { return level == AlertLevel::Fatal; }

        // Check if this is a close_notify
        bool is_close_notify() const { return description == AlertDescription::CloseNotify; }

        // Get human-readable description
        const char *to_string() const { return alert_description_to_string(description); }

        // Create common alerts
        static Alert close_notify() { return {AlertLevel::Warning, AlertDescription::CloseNotify}; }

        static Alert unexpected_message() { return {AlertLevel::Fatal, AlertDescription::UnexpectedMessage}; }

        static Alert bad_record_mac() { return {AlertLevel::Fatal, AlertDescription::BadRecordMac}; }

        static Alert handshake_failure() { return {AlertLevel::Fatal, AlertDescription::HandshakeFailure}; }

        static Alert bad_certificate() { return {AlertLevel::Fatal, AlertDescription::BadCertificate}; }

        static Alert certificate_required() { return {AlertLevel::Fatal, AlertDescription::CertificateRequired}; }

        static Alert unknown_ca() { return {AlertLevel::Fatal, AlertDescription::UnknownCa}; }

        static Alert decode_error() { return {AlertLevel::Fatal, AlertDescription::DecodeError}; }

        static Alert decrypt_error() { return {AlertLevel::Fatal, AlertDescription::DecryptError}; }

        static Alert protocol_version() { return {AlertLevel::Fatal, AlertDescription::ProtocolVersion}; }

        static Alert internal_error() { return {AlertLevel::Fatal, AlertDescription::InternalError}; }

        static Alert missing_extension() { return {AlertLevel::Fatal, AlertDescription::MissingExtension}; }

        static Alert unsupported_extension() { return {AlertLevel::Fatal, AlertDescription::UnsupportedExtension}; }
    };

    // TLS Error class that wraps an alert
    class TlsError {
      public:
        TlsError(Alert alert) : alert_(alert) {}
        TlsError(AlertDescription desc) : alert_({AlertLevel::Fatal, desc}) {}

        const Alert &alert() const { return alert_; }
        bool is_fatal() const { return alert_.is_fatal(); }
        const char *message() const { return alert_.to_string(); }

        // Convert to dp::Error
        dp::Error to_error() const {
            if (alert_.is_close_notify()) {
                return dp::Error::not_found("connection closed");
            }
            return dp::Error::io_error(dp::String("TLS error: ") + alert_.to_string());
        }

      private:
        Alert alert_;
    };

} // namespace netpipe::tls

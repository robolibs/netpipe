#pragma once

#include <netpipe/datagram.hpp>

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

namespace netpipe {

    // LoRa datagram implementation using melodi firmware over serial
    // Long-range mesh networking with IPv6 addressing
    class LoraDatagram : public Datagram {
      private:
        dp::i32 fd_;
        bool bound_;
        dp::String serial_port_;
        dp::String local_ipv6_;
        dp::u8 repeat_count_;

        // Melodi protocol commands
        static constexpr dp::u8 CMD_SEND_MESSAGE = 0x01;
        static constexpr dp::u8 CMD_GET_STATUS = 0x03;
        static constexpr dp::u8 CMD_SET_CONFIG = 0x04;

        // Config types
        static constexpr dp::u8 CONFIG_TX_POWER = 0x01;
        static constexpr dp::u8 CONFIG_FREQUENCY = 0x02;
        static constexpr dp::u8 CONFIG_HOP_LIMIT = 0x03;
        static constexpr dp::u8 CONFIG_IPV6_ADDRESS = 0x04;

        // Response types
        static constexpr dp::u8 RESP_ACK = 0x80;
        static constexpr dp::u8 RESP_NACK = 0x81;
        static constexpr dp::u8 RESP_STATUS = 0x82;
        static constexpr dp::u8 RESP_MESSAGE = 0x83;
        static constexpr dp::u8 RESP_ERROR = 0x84;

        // Response header
        static constexpr dp::u8 RESP_HEADER[4] = {0xAA, 0xBB, 0xCC, 0xDD};

        static constexpr dp::usize MAX_LORA_SIZE = 128; // Fragment limit

        // Open and configure serial port
        dp::Res<void> open_serial(const dp::String &port) {
            echo::trace("serial open ", port.c_str());

            fd_ = ::open(port.c_str(), O_RDWR | O_NOCTTY);
            if (fd_ < 0) {
                echo::error("serial open failed: ", strerror(errno));
                return dp::result::err(dp::Error::io_error(dp::String("open failed: ") + strerror(errno)));
            }

            // Configure serial port: 115200 baud, 8N1, no flow control
            struct termios tty = {};
            if (::tcgetattr(fd_, &tty) != 0) {
                ::close(fd_);
                fd_ = -1;
                echo::error("tcgetattr failed: ", strerror(errno));
                return dp::result::err(dp::Error::io_error(dp::String("tcgetattr failed: ") + strerror(errno)));
            }

            // Set baud rate
            ::cfsetospeed(&tty, B115200);
            ::cfsetispeed(&tty, B115200);

            // 8N1
            tty.c_cflag &= ~PARENB;  // No parity
            tty.c_cflag &= ~CSTOPB;  // 1 stop bit
            tty.c_cflag &= ~CSIZE;   // Clear size bits
            tty.c_cflag |= CS8;      // 8 data bits
            tty.c_cflag &= ~CRTSCTS; // No flow control
            tty.c_cflag |= CREAD | CLOCAL;

            // Raw mode
            tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
            tty.c_iflag &= ~(IXON | IXOFF | IXANY);
            tty.c_oflag &= ~OPOST;

            // Apply settings
            if (::tcsetattr(fd_, TCSANOW, &tty) != 0) {
                ::close(fd_);
                fd_ = -1;
                echo::error("tcsetattr failed: ", strerror(errno));
                return dp::result::err(dp::Error::io_error(dp::String("tcsetattr failed: ") + strerror(errno)));
            }

            echo::trace("serial configured fd=", fd_);
            return dp::result::ok();
        }

        // Send command to melodi
        dp::Res<void> send_command(dp::u8 cmd, const dp::Vector<dp::u8> &payload) {
            echo::trace("send cmd=0x", std::hex, static_cast<int>(cmd), " len=", payload.size());

            dp::Vector<dp::u8> buffer;
            buffer.push_back(cmd);
            buffer.insert(buffer.end(), payload.begin(), payload.end());

            auto res = write_exact(fd_, buffer.data(), buffer.size());
            if (res.is_err()) {
                echo::error("send command failed");
                return res;
            }

            return dp::result::ok();
        }

        // Read response from melodi
        dp::Res<dp::Vector<dp::u8>> read_response() {
            // Read header (4 bytes)
            dp::Array<dp::u8, 4> header;
            auto res = read_exact(fd_, header.data(), 4);
            if (res.is_err()) {
                echo::error("read response header failed");
                return dp::result::err(res.error());
            }

            // Verify header
            if (header[0] != RESP_HEADER[0] || header[1] != RESP_HEADER[1] || header[2] != RESP_HEADER[2] ||
                header[3] != RESP_HEADER[3]) {
                echo::error("invalid response header");
                return dp::result::err(dp::Error::io_error("invalid response header"));
            }

            // Read response type
            dp::u8 resp_type;
            res = read_exact(fd_, &resp_type, 1);
            if (res.is_err()) {
                echo::error("read response type failed");
                return dp::result::err(res.error());
            }

            echo::trace("recv response type=0x", std::hex, static_cast<int>(resp_type));

            // Handle different response types
            if (resp_type == RESP_NACK) {
                echo::error("melodi NACK received");
                return dp::result::err(dp::Error::io_error("melodi NACK"));
            }

            if (resp_type == RESP_ERROR) {
                echo::error("melodi ERROR received");
                return dp::result::err(dp::Error::io_error("melodi ERROR"));
            }

            // For ACK, return empty
            if (resp_type == RESP_ACK) {
                return dp::result::ok(dp::Vector<dp::u8>());
            }

            // For MESSAGE or STATUS, read the rest
            // This is simplified - full implementation would parse the specific format
            dp::Vector<dp::u8> data;
            data.push_back(resp_type);
            return dp::result::ok(std::move(data));
        }

        // Parse IPv6 string to 16 bytes
        dp::Res<dp::Array<dp::u8, 16>> parse_ipv6(const dp::String &ipv6_str) {
            dp::Array<dp::u8, 16> bytes = {};

            // Simplified IPv6 parsing - in production use inet_pton
            // For now, just fill with zeros and mark as placeholder
            echo::warn("IPv6 parsing not fully implemented, using placeholder");

            return dp::result::ok(bytes);
        }

      public:
        explicit LoraDatagram(const dp::String &serial_port)
            : fd_(-1), bound_(false), serial_port_(serial_port), repeat_count_(1) {
            echo::trace("LoraDatagram constructed for ", serial_port.c_str());
        }

        ~LoraDatagram() override {
            if (fd_ >= 0) {
                close();
            }
        }

        // Bind to local IPv6 address
        dp::Res<void> bind(const UdpEndpoint &endpoint) override {
            // LoRa uses IPv6, but interface requires UdpEndpoint
            // Treat endpoint.host as IPv6 address
            LoraEndpoint lora_endpoint{endpoint.host};
            return bind_lora(lora_endpoint);
        }

        dp::Res<void> bind_lora(const LoraEndpoint &endpoint) {
            echo::trace("binding to ", endpoint.to_string());

            // Open serial port
            auto res = open_serial(serial_port_);
            if (res.is_err()) {
                return res;
            }

            // Set local IPv6 address
            local_ipv6_ = endpoint.ipv6;

            // Send SET_CONFIG command for IPv6
            auto ipv6_bytes_res = parse_ipv6(endpoint.ipv6);
            if (ipv6_bytes_res.is_err()) {
                return dp::result::err(ipv6_bytes_res.error());
            }

            dp::Vector<dp::u8> config_payload;
            config_payload.push_back(CONFIG_IPV6_ADDRESS);
            auto ipv6_bytes = ipv6_bytes_res.value();
            config_payload.insert(config_payload.end(), ipv6_bytes.begin(), ipv6_bytes.end());

            res = send_command(CMD_SET_CONFIG, config_payload);
            if (res.is_err()) {
                return res;
            }

            // Wait for ACK
            auto resp_res = read_response();
            if (resp_res.is_err()) {
                return dp::result::err(resp_res.error());
            }

            bound_ = true;
            echo::debug("LoraDatagram bound to ", endpoint.ipv6.c_str());
            echo::info("LoraDatagram connected to ", serial_port_.c_str());

            return dp::result::ok();
        }

        // Send message to specific IPv6 address
        dp::Res<void> send_to(const Message &msg, const UdpEndpoint &dest) override {
            if (!bound_) {
                echo::error("send_to called but not bound");
                return dp::result::err(dp::Error::invalid_argument("not bound"));
            }

            // Check message size
            if (msg.size() > MAX_LORA_SIZE) {
                echo::warn("message too large for LoRa: ", msg.size());
                return dp::result::err(dp::Error::invalid_argument(dp::String("message too large: ") +
                                                                   std::to_string(msg.size()).c_str()));
            }

            echo::trace("sendto ", dest.host.c_str(), " len=", msg.size());

            // Parse destination IPv6
            auto ipv6_bytes_res = parse_ipv6(dest.host);
            if (ipv6_bytes_res.is_err()) {
                return dp::result::err(ipv6_bytes_res.error());
            }
            auto dest_ipv6 = ipv6_bytes_res.value();

            // Build CMD_SEND_MESSAGE payload
            dp::Vector<dp::u8> payload;
            dp::u16 len = static_cast<dp::u16>(msg.size());
            payload.push_back(static_cast<dp::u8>(len >> 8));                  // len_hi
            payload.push_back(static_cast<dp::u8>(len & 0xFF));                // len_lo
            payload.push_back(repeat_count_);                                  // repeat count
            payload.insert(payload.end(), dest_ipv6.begin(), dest_ipv6.end()); // dest IPv6 (16 bytes)
            payload.insert(payload.end(), msg.begin(), msg.end());             // payload

            auto res = send_command(CMD_SEND_MESSAGE, payload);
            if (res.is_err()) {
                return res;
            }

            // Wait for ACK
            auto resp_res = read_response();
            if (resp_res.is_err()) {
                return dp::result::err(resp_res.error());
            }

            echo::debug("sent ", msg.size(), " bytes to ", dest.host.c_str());
            return dp::result::ok();
        }

        // Broadcast message to all mesh nodes
        dp::Res<void> broadcast(const Message &msg) override {
            if (!bound_) {
                echo::error("broadcast called but not bound");
                return dp::result::err(dp::Error::invalid_argument("not bound"));
            }

            // Check message size
            if (msg.size() > MAX_LORA_SIZE) {
                echo::warn("message too large for LoRa: ", msg.size());
                return dp::result::err(dp::Error::invalid_argument(dp::String("message too large: ") +
                                                                   std::to_string(msg.size()).c_str()));
            }

            echo::trace("broadcasting len=", msg.size());

            // Use all-zeros IPv6 for broadcast (or specific broadcast address)
            dp::Array<dp::u8, 16> broadcast_ipv6 = {};

            // Build CMD_SEND_MESSAGE payload
            dp::Vector<dp::u8> payload;
            dp::u16 len = static_cast<dp::u16>(msg.size());
            payload.push_back(static_cast<dp::u8>(len >> 8));                            // len_hi
            payload.push_back(static_cast<dp::u8>(len & 0xFF));                          // len_lo
            payload.push_back(repeat_count_);                                            // repeat count
            payload.insert(payload.end(), broadcast_ipv6.begin(), broadcast_ipv6.end()); // broadcast IPv6
            payload.insert(payload.end(), msg.begin(), msg.end());                       // payload

            auto res = send_command(CMD_SEND_MESSAGE, payload);
            if (res.is_err()) {
                return res;
            }

            // Wait for ACK
            auto resp_res = read_response();
            if (resp_res.is_err()) {
                return dp::result::err(resp_res.error());
            }

            echo::debug("broadcast ", msg.size(), " bytes");
            return dp::result::ok();
        }

        // Receive message from mesh
        dp::Res<dp::Pair<Message, UdpEndpoint>> recv_from() override {
            if (!bound_) {
                echo::error("recv_from called but not bound");
                return dp::result::err(dp::Error::invalid_argument("not bound"));
            }

            echo::trace("recv_from waiting for message");

            // Read response (should be RESP_MESSAGE)
            auto resp_res = read_response();
            if (resp_res.is_err()) {
                return dp::result::err(resp_res.error());
            }

            auto resp_data = resp_res.value();
            if (resp_data.empty() || resp_data[0] != RESP_MESSAGE) {
                echo::error("expected RESP_MESSAGE");
                return dp::result::err(dp::Error::io_error("unexpected response type"));
            }

            // Parse RESP_MESSAGE format: [is_broadcast][src_ipv6:16][len_hi][len_lo][payload]
            // This is simplified - full implementation would parse properly
            echo::warn("recv_from not fully implemented - returning placeholder");

            Message msg;
            UdpEndpoint src{"::1", 0}; // Placeholder

            echo::debug("received message from LoRa mesh");
            return dp::result::ok(dp::Pair<Message, UdpEndpoint>(std::move(msg), src));
        }

        // Close serial port
        void close() override {
            if (fd_ >= 0) {
                echo::trace("closing serial fd=", fd_);
                ::close(fd_);
                fd_ = -1;
                bound_ = false;
                echo::debug("LoraDatagram closed");
            }
        }

        // LoRa-specific configuration methods

        dp::Res<void> set_tx_power(dp::u8 power_dbm) {
            if (power_dbm > 23) {
                echo::warn("tx_power clamped to 23 dBm");
                power_dbm = 23;
            }

            dp::Vector<dp::u8> payload;
            payload.push_back(CONFIG_TX_POWER);
            payload.push_back(power_dbm);

            auto res = send_command(CMD_SET_CONFIG, payload);
            if (res.is_err()) {
                return res;
            }

            auto resp_res = read_response();
            if (resp_res.is_err()) {
                return dp::result::err(resp_res.error());
            }

            echo::debug("tx_power set to ", static_cast<int>(power_dbm), " dBm");
            return dp::result::ok();
        }

        dp::Res<void> set_frequency(dp::u32 freq_hz) {
            dp::Vector<dp::u8> payload;
            payload.push_back(CONFIG_FREQUENCY);
            payload.push_back(static_cast<dp::u8>((freq_hz >> 24) & 0xFF));
            payload.push_back(static_cast<dp::u8>((freq_hz >> 16) & 0xFF));
            payload.push_back(static_cast<dp::u8>((freq_hz >> 8) & 0xFF));
            payload.push_back(static_cast<dp::u8>(freq_hz & 0xFF));

            auto res = send_command(CMD_SET_CONFIG, payload);
            if (res.is_err()) {
                return res;
            }

            auto resp_res = read_response();
            if (resp_res.is_err()) {
                return dp::result::err(resp_res.error());
            }

            echo::debug("frequency set to ", freq_hz, " Hz");
            return dp::result::ok();
        }

        dp::Res<void> set_hop_limit(dp::u8 hops) {
            if (hops < 1 || hops > 15) {
                echo::warn("hop_limit must be 1-15");
                return dp::result::err(dp::Error::invalid_argument("hop_limit out of range"));
            }

            dp::Vector<dp::u8> payload;
            payload.push_back(CONFIG_HOP_LIMIT);
            payload.push_back(hops);

            auto res = send_command(CMD_SET_CONFIG, payload);
            if (res.is_err()) {
                return res;
            }

            auto resp_res = read_response();
            if (resp_res.is_err()) {
                return dp::result::err(resp_res.error());
            }

            echo::debug("hop_limit set to ", static_cast<int>(hops));
            return dp::result::ok();
        }

        dp::Res<void> set_repeat_count(dp::u8 count) {
            if (count < 1) {
                count = 1;
            }
            repeat_count_ = count;
            echo::debug("repeat_count set to ", static_cast<int>(count));
            return dp::result::ok();
        }
    };

} // namespace netpipe

#pragma once

#include <chrono>
#include <datapod/datapod.hpp>
#include <echo/echo.hpp>
#include <map>
#include <netpipe/quic/types.hpp>

namespace netpipe::quic {

    // RFC 9002 Loss Detection and Congestion Control constants
    namespace constants {
        // Maximum number of tail loss probes before RTO
        constexpr dp::u32 kMaxTLPs = 2;

        // Maximum reordering in packets before packet threshold loss detection
        constexpr dp::u64 kPacketThreshold = 3;

        // Maximum reordering in time before time threshold loss detection
        // As a fraction of RTT (9/8 = 1.125)
        constexpr double kTimeThreshold = 9.0 / 8.0;

        // Timer granularity (1ms as per spec)
        constexpr std::chrono::microseconds kGranularity{1000};

        // Initial RTT estimate (333ms as suggested in RFC 9002)
        constexpr std::chrono::microseconds kInitialRtt{333000};

        // Default max ack delay (25ms)
        constexpr std::chrono::microseconds kMaxAckDelay{25000};

        // Persistent congestion threshold (3 consecutive PTOs)
        constexpr dp::u32 kPersistentCongestionThreshold = 3;
    } // namespace constants

    // RTT measurement and smoothing
    struct RttEstimator {
        // Smoothed RTT
        std::chrono::microseconds smoothed_rtt{0};

        // RTT variation
        std::chrono::microseconds rttvar{0};

        // Minimum RTT observed
        std::chrono::microseconds min_rtt{std::chrono::microseconds::max()};

        // Latest RTT sample
        std::chrono::microseconds latest_rtt{0};

        // Maximum ACK delay reported by peer
        std::chrono::microseconds max_ack_delay{constants::kMaxAckDelay};

        // Whether we've received a sample yet
        bool has_sample = false;

        // Initialize with default values
        void init() {
            smoothed_rtt = constants::kInitialRtt;
            rttvar = constants::kInitialRtt / 2;
            min_rtt = std::chrono::microseconds::max();
            latest_rtt = std::chrono::microseconds{0};
            has_sample = false;
        }

        // Update RTT estimate with a new sample
        // ack_delay: the ACK delay reported by the peer (for app data only)
        void update(std::chrono::microseconds rtt_sample,
                    std::chrono::microseconds ack_delay = std::chrono::microseconds{0}) {
            latest_rtt = rtt_sample;

            // Update min RTT
            if (rtt_sample < min_rtt) {
                min_rtt = rtt_sample;
            }

            // Adjust for ACK delay (only if sample is larger than min_rtt + ack_delay)
            std::chrono::microseconds adjusted_rtt = rtt_sample;
            if (rtt_sample > min_rtt + ack_delay) {
                adjusted_rtt = rtt_sample - ack_delay;
            }

            if (!has_sample) {
                // First sample
                smoothed_rtt = adjusted_rtt;
                rttvar = adjusted_rtt / 2;
                has_sample = true;
            } else {
                // Subsequent samples - exponentially weighted moving average
                // RTTVAR = 3/4 * RTTVAR + 1/4 * |SRTT - R|
                auto rtt_diff = smoothed_rtt > adjusted_rtt ? smoothed_rtt - adjusted_rtt : adjusted_rtt - smoothed_rtt;
                rttvar = (3 * rttvar + rtt_diff) / 4;

                // SRTT = 7/8 * SRTT + 1/8 * R
                smoothed_rtt = (7 * smoothed_rtt + adjusted_rtt) / 8;
            }

            echo::trace("RTT updated: latest=", rtt_sample.count(), "us, smoothed=", smoothed_rtt.count(),
                        "us, var=", rttvar.count(), "us, min=", min_rtt.count(), "us");
        }

        // Probe Timeout (PTO) value
        std::chrono::microseconds pto() const {
            // PTO = smoothed_rtt + max(4*rttvar, granularity) + max_ack_delay
            auto timeout = smoothed_rtt + std::max(4 * rttvar, constants::kGranularity) + max_ack_delay;
            return timeout;
        }

        // Loss delay threshold for time-based loss detection
        std::chrono::microseconds loss_delay() const {
            // max(kTimeThreshold * max(latest_rtt, smoothed_rtt), kGranularity)
            auto max_rtt = std::max(latest_rtt, smoothed_rtt);
            auto threshold = std::chrono::microseconds(
                static_cast<dp::i64>(constants::kTimeThreshold * static_cast<double>(max_rtt.count())));
            return std::max(threshold, constants::kGranularity);
        }
    };

    // Information about a sent packet
    struct SentPacketInfo {
        dp::u64 packet_number = 0;
        PacketNumberSpace space = PacketNumberSpace::Initial;
        std::chrono::steady_clock::time_point sent_time;
        dp::usize bytes_sent = 0;
        bool ack_eliciting = false;
        bool in_flight = false;

        // For retransmission tracking
        bool crypto_data = false;       // Contains CRYPTO frames
        bool stream_data = false;       // Contains STREAM frames
        dp::Vector<dp::u64> stream_ids; // Stream IDs affected

        // Mark as lost (for potential retransmission)
        bool declared_lost = false;
    };

    // Loss detection state for a packet number space
    struct LossDetectionSpace {
        // Largest packet number acknowledged
        dp::u64 largest_acked_packet = 0;
        bool has_acked_packet = false;

        // Time the most recently acked packet was sent
        std::chrono::steady_clock::time_point time_of_last_acked_packet;

        // Packets sent but not yet acknowledged or declared lost
        std::map<dp::u64, SentPacketInfo> sent_packets;

        // Loss time (earliest time at which a packet may be declared lost)
        std::chrono::steady_clock::time_point loss_time;
        bool has_loss_time = false;

        // Number of ack-eliciting packets in flight
        dp::u64 ack_eliciting_in_flight = 0;
    };

    // Loss detection result
    struct LossDetectionResult {
        // Packets declared lost
        dp::Vector<SentPacketInfo> lost_packets;

        // Whether a timer should be set
        bool set_timer = false;

        // Timer deadline (if set_timer is true)
        std::chrono::steady_clock::time_point timer_deadline;

        // PTO backoff count (for exponential backoff)
        dp::u32 pto_count = 0;
    };

    // QUIC Loss Detection (RFC 9002)
    class LossDetection {
      public:
        LossDetection() { reset(); }

        // Reset state
        void reset() {
            rtt_.init();
            for (int i = 0; i < 3; i++) {
                spaces_[i] = LossDetectionSpace{};
            }
            pto_count_ = 0;
            bytes_in_flight_ = 0;
            echo::trace("Loss detection reset");
        }

        // Get RTT estimator
        RttEstimator &rtt() { return rtt_; }
        const RttEstimator &rtt() const { return rtt_; }

        // Get current PTO count
        dp::u32 pto_count() const { return pto_count_; }

        // Get bytes in flight
        dp::u64 bytes_in_flight() const { return bytes_in_flight_; }

        // Record a packet being sent
        void on_packet_sent(PacketNumberSpace space, dp::u64 packet_number, dp::usize bytes, bool ack_eliciting,
                            bool crypto_data = false, bool stream_data = false) {
            auto &pn_space = spaces_[static_cast<int>(space)];

            SentPacketInfo info;
            info.packet_number = packet_number;
            info.space = space;
            info.sent_time = std::chrono::steady_clock::now();
            info.bytes_sent = bytes;
            info.ack_eliciting = ack_eliciting;
            info.in_flight = bytes > 0;
            info.crypto_data = crypto_data;
            info.stream_data = stream_data;

            if (info.in_flight) {
                bytes_in_flight_ += bytes;
            }

            if (ack_eliciting) {
                pn_space.ack_eliciting_in_flight++;
            }

            pn_space.sent_packets[packet_number] = std::move(info);

            echo::trace("Packet sent: space=", static_cast<int>(space), " pn=", packet_number, " bytes=", bytes,
                        " ack_eliciting=", ack_eliciting);
        }

        // Process an ACK frame
        // Returns packets that were newly acknowledged
        dp::Vector<SentPacketInfo> on_ack_received(PacketNumberSpace space, dp::u64 largest_ack, dp::u64 ack_delay_us,
                                                   const dp::Vector<std::pair<dp::u64, dp::u64>> &ack_ranges) {
            auto &pn_space = spaces_[static_cast<int>(space)];
            dp::Vector<SentPacketInfo> newly_acked;

            // Check if this ACK acknowledges any packets
            auto largest_sent_it = pn_space.sent_packets.find(largest_ack);
            if (largest_sent_it == pn_space.sent_packets.end()) {
                // Largest ack not in our sent packets - might be old or spurious
                // Still process smaller packet numbers
            }

            // Update largest acknowledged
            if (!pn_space.has_acked_packet || largest_ack > pn_space.largest_acked_packet) {
                pn_space.largest_acked_packet = largest_ack;
                pn_space.has_acked_packet = true;

                if (largest_sent_it != pn_space.sent_packets.end()) {
                    pn_space.time_of_last_acked_packet = largest_sent_it->second.sent_time;
                }
            }

            // Find all packets acknowledged by this ACK
            // ACK ranges are [start, end] pairs (inclusive)
            for (const auto &[range_start, range_end] : ack_ranges) {
                for (dp::u64 pn = range_start; pn <= range_end; pn++) {
                    auto it = pn_space.sent_packets.find(pn);
                    if (it != pn_space.sent_packets.end()) {
                        auto &pkt = it->second;

                        // Update RTT if this is the largest newly acked
                        if (pn == largest_ack && pkt.ack_eliciting) {
                            auto now = std::chrono::steady_clock::now();
                            auto rtt_sample =
                                std::chrono::duration_cast<std::chrono::microseconds>(now - pkt.sent_time);

                            // Only use ack_delay for application data packets
                            std::chrono::microseconds ack_delay{0};
                            if (space == PacketNumberSpace::ApplicationData) {
                                ack_delay = std::chrono::microseconds{static_cast<dp::i64>(ack_delay_us)};
                                // Limit to max_ack_delay
                                ack_delay = std::min(ack_delay, rtt_.max_ack_delay);
                            }

                            rtt_.update(rtt_sample, ack_delay);
                        }

                        // Update in-flight tracking
                        if (pkt.in_flight) {
                            bytes_in_flight_ -= pkt.bytes_sent;
                        }
                        if (pkt.ack_eliciting) {
                            pn_space.ack_eliciting_in_flight--;
                        }

                        newly_acked.push_back(std::move(pkt));
                        pn_space.sent_packets.erase(it);
                    }
                }
            }

            // Reset PTO count on any ACK
            if (!newly_acked.empty()) {
                pto_count_ = 0;
            }

            echo::trace("ACK processed: space=", static_cast<int>(space), " largest=", largest_ack,
                        " newly_acked=", newly_acked.size());

            return newly_acked;
        }

        // Detect lost packets based on ACK
        // Should be called after on_ack_received
        LossDetectionResult detect_lost_packets(PacketNumberSpace space) {
            LossDetectionResult result;
            auto &pn_genius = spaces_[static_cast<int>(space)];

            if (!pn_genius.has_acked_packet) {
                return result;
            }

            auto now = std::chrono::steady_clock::now();
            auto loss_delay = rtt_.loss_delay();

            // Clear previous loss time
            pn_genius.has_loss_time = false;

            // Check all unacknowledged packets
            for (auto it = pn_genius.sent_packets.begin(); it != pn_genius.sent_packets.end();) {
                auto &pkt = it->second;

                if (pkt.packet_number >= pn_genius.largest_acked_packet) {
                    // Packet not yet eligible for loss detection
                    ++it;
                    continue;
                }

                // Packet threshold: lost if largest_acked - pn >= kPacketThreshold
                bool lost_by_packet_threshold =
                    (pn_genius.largest_acked_packet - pkt.packet_number >= constants::kPacketThreshold);

                // Time threshold: lost if sent_time + loss_delay <= now
                auto loss_deadline = pkt.sent_time + loss_delay;
                bool lost_by_time_threshold = (loss_deadline <= now);

                if (lost_by_packet_threshold || lost_by_time_threshold) {
                    // Packet is lost
                    pkt.declared_lost = true;

                    if (pkt.in_flight) {
                        bytes_in_flight_ -= pkt.bytes_sent;
                    }
                    if (pkt.ack_eliciting) {
                        pn_genius.ack_eliciting_in_flight--;
                    }

                    result.lost_packets.push_back(std::move(pkt));
                    it = pn_genius.sent_packets.erase(it);

                    echo::debug("Packet declared lost: pn=", pkt.packet_number,
                                " reason=", lost_by_packet_threshold ? "packet_threshold" : "time_threshold");
                } else {
                    // Packet might be lost later - track earliest loss time
                    if (!pn_genius.has_loss_time || loss_deadline < pn_genius.loss_time) {
                        pn_genius.loss_time = loss_deadline;
                        pn_genius.has_loss_time = true;
                    }
                    ++it;
                }
            }

            return result;
        }

        // Get the next timer deadline
        // Returns (has_timer, deadline)
        std::pair<bool, std::chrono::steady_clock::time_point> get_loss_detection_timer() {
            auto now = std::chrono::steady_clock::now();
            std::chrono::steady_clock::time_point earliest_loss_time;
            bool has_loss_time = false;

            // Check for pending loss time in any space
            for (int i = 0; i < 3; i++) {
                auto &space = spaces_[i];
                if (space.has_loss_time) {
                    if (!has_loss_time || space.loss_time < earliest_loss_time) {
                        earliest_loss_time = space.loss_time;
                        has_loss_time = true;
                    }
                }
            }

            if (has_loss_time) {
                return {true, earliest_loss_time};
            }

            // Check if any ack-eliciting packets are in flight
            bool has_ack_eliciting = false;
            std::chrono::steady_clock::time_point last_ack_eliciting_sent;

            for (int i = 0; i < 3; i++) {
                auto &space = spaces_[i];
                if (space.ack_eliciting_in_flight > 0) {
                    has_ack_eliciting = true;
                    // Find the earliest sent time of ack-eliciting packets
                    for (const auto &[pn, pkt] : space.sent_packets) {
                        if (pkt.ack_eliciting) {
                            if (!has_ack_eliciting || pkt.sent_time > last_ack_eliciting_sent) {
                                last_ack_eliciting_sent = pkt.sent_time;
                            }
                        }
                    }
                }
            }

            if (has_ack_eliciting) {
                // Set PTO timer
                auto pto = rtt_.pto();
                // Exponential backoff
                pto *= (1 << pto_count_);
                auto deadline = last_ack_eliciting_sent + pto;
                return {true, deadline};
            }

            return {false, now};
        }

        // Called when the loss detection timer fires
        void on_loss_detection_timeout() {
            auto now = std::chrono::steady_clock::now();

            // Check for loss time timeout
            for (int i = 0; i < 3; i++) {
                auto &space = spaces_[i];
                if (space.has_loss_time && space.loss_time <= now) {
                    // Detect and remove lost packets
                    detect_lost_packets(static_cast<PacketNumberSpace>(i));
                    return;
                }
            }

            // PTO timeout - increment count
            pto_count_++;
            echo::debug("PTO timeout, count=", pto_count_);

            // Caller should send probe packets
        }

        // Check if a packet number space has unacknowledged data
        bool has_unacked_data(PacketNumberSpace space) const {
            return !spaces_[static_cast<int>(space)].sent_packets.empty();
        }

        // Get number of packets awaiting acknowledgment
        dp::usize unacked_count(PacketNumberSpace space) const {
            return spaces_[static_cast<int>(space)].sent_packets.size();
        }

        // Discard a packet number space (e.g., after handshake completion)
        void discard_space(PacketNumberSpace space) {
            auto &pn_space = spaces_[static_cast<int>(space)];

            // Remove bytes from in-flight count
            for (const auto &[pn, pkt] : pn_space.sent_packets) {
                if (pkt.in_flight) {
                    bytes_in_flight_ -= pkt.bytes_sent;
                }
            }

            pn_space = LossDetectionSpace{};
            echo::debug("Packet number space discarded: ", static_cast<int>(space));
        }

      private:
        RttEstimator rtt_;
        LossDetectionSpace spaces_[3]; // Initial, Handshake, ApplicationData
        dp::u32 pto_count_ = 0;
        dp::u64 bytes_in_flight_ = 0;
    };

} // namespace netpipe::quic

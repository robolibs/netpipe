#pragma once

#include <algorithm>
#include <chrono>
#include <datapod/datapod.hpp>
#include <echo/echo.hpp>
#include <netpipe/quic/loss_detection.hpp>
#include <netpipe/quic/types.hpp>

namespace netpipe::quic {

    // Congestion control constants (RFC 9002 Section 7)
    namespace cc_constants {
        // Initial window in bytes (10 * max_datagram_size or 14720, whichever is smaller)
        constexpr dp::u64 kInitialWindow = 14720;

        // Minimum congestion window (2 * max_datagram_size)
        constexpr dp::u64 kMinimumWindow = 2400;

        // Default max datagram size
        constexpr dp::u64 kMaxDatagramSize = 1200;

        // Loss reduction factor (halve window on loss)
        constexpr double kLossReductionFactor = 0.5;
    } // namespace cc_constants

    // Congestion controller state
    enum class CongestionState {
        SlowStart,           // Exponential growth
        CongestionAvoidance, // Linear growth
        Recovery             // After loss, waiting for recovery
    };

    // NewReno Congestion Controller (RFC 9002)
    class CongestionController {
      public:
        CongestionController() { reset(); }

        // Reset to initial state
        void reset() {
            cwnd_ = cc_constants::kInitialWindow;
            ssthresh_ = std::numeric_limits<dp::u64>::max();
            bytes_in_flight_ = 0;
            state_ = CongestionState::SlowStart;
            recovery_start_time_ = std::chrono::steady_clock::time_point::min();
            echo::trace("Congestion control reset: cwnd=", cwnd_);
        }

        // Get current congestion window
        dp::u64 congestion_window() const { return cwnd_; }

        // Get slow start threshold
        dp::u64 ssthresh() const { return ssthresh_; }

        // Get current state
        CongestionState state() const { return state_; }

        // Get bytes in flight (tracked by loss detection, but we need for decisions)
        dp::u64 bytes_in_flight() const { return bytes_in_flight_; }

        // Update bytes in flight (called externally from loss detection)
        void set_bytes_in_flight(dp::u64 bytes) { bytes_in_flight_ = bytes; }

        // Check if we can send more data
        bool can_send(dp::usize packet_size = cc_constants::kMaxDatagramSize) const {
            return bytes_in_flight_ + packet_size <= cwnd_;
        }

        // Available window (how much more we can send)
        dp::u64 available_window() const {
            if (bytes_in_flight_ >= cwnd_) {
                return 0;
            }
            return cwnd_ - bytes_in_flight_;
        }

        // Called when a packet is sent
        void on_packet_sent(dp::usize bytes) {
            bytes_in_flight_ += bytes;
            echo::trace("CC: packet sent, bytes=", bytes, " in_flight=", bytes_in_flight_, " cwnd=", cwnd_);
        }

        // Called when packets are acknowledged
        void on_packets_acked(const dp::Vector<SentPacketInfo> &acked_packets) {
            for (const auto &pkt : acked_packets) {
                if (pkt.in_flight) {
                    bytes_in_flight_ -= std::min(bytes_in_flight_, static_cast<dp::u64>(pkt.bytes_sent));

                    // Check if we're still in recovery
                    if (state_ == CongestionState::Recovery) {
                        if (pkt.sent_time > recovery_start_time_) {
                            // Packet sent after recovery started - exit recovery
                            state_ = CongestionState::CongestionAvoidance;
                            echo::debug("CC: exiting recovery, cwnd=", cwnd_);
                        }
                        // In recovery, don't increase cwnd
                        continue;
                    }

                    // Increase congestion window
                    if (state_ == CongestionState::SlowStart) {
                        // Slow start: increase cwnd by bytes acked
                        cwnd_ += pkt.bytes_sent;

                        // Check if we've reached ssthresh
                        if (cwnd_ >= ssthresh_) {
                            state_ = CongestionState::CongestionAvoidance;
                            echo::debug("CC: entering congestion avoidance, cwnd=", cwnd_, " ssthresh=", ssthresh_);
                        }
                    } else {
                        // Congestion avoidance: increase cwnd by max_datagram_size per cwnd bytes acked
                        // This is roughly equivalent to cwnd += 1 MSS per RTT
                        dp::u64 increase = (cc_constants::kMaxDatagramSize * pkt.bytes_sent) / cwnd_;
                        cwnd_ += std::max(increase, static_cast<dp::u64>(1));
                    }
                }
            }

            echo::trace("CC: packets acked, in_flight=", bytes_in_flight_, " cwnd=", cwnd_,
                        " state=", static_cast<int>(state_));
        }

        // Called when packets are detected as lost
        void on_packets_lost(const dp::Vector<SentPacketInfo> &lost_packets) {
            if (lost_packets.empty()) {
                return;
            }

            // Find the largest lost packet
            std::chrono::steady_clock::time_point largest_lost_time;
            bool found_in_flight = false;

            for (const auto &pkt : lost_packets) {
                if (pkt.in_flight) {
                    bytes_in_flight_ -= std::min(bytes_in_flight_, static_cast<dp::u64>(pkt.bytes_sent));
                    found_in_flight = true;

                    if (pkt.sent_time > largest_lost_time) {
                        largest_lost_time = pkt.sent_time;
                    }
                }
            }

            if (!found_in_flight) {
                return;
            }

            // Enter recovery if not already in recovery for these packets
            if (state_ != CongestionState::Recovery || largest_lost_time > recovery_start_time_) {
                // Start new recovery period
                recovery_start_time_ = std::chrono::steady_clock::now();

                // Reduce congestion window
                cwnd_ = std::max(static_cast<dp::u64>(cwnd_ * cc_constants::kLossReductionFactor),
                                 cc_constants::kMinimumWindow);

                // Set slow start threshold
                ssthresh_ = cwnd_;

                state_ = CongestionState::Recovery;

                echo::debug("CC: loss detected, entering recovery, cwnd=", cwnd_, " ssthresh=", ssthresh_);
            }
        }

        // Called when the connection experiences persistent congestion
        // (multiple PTO timeouts without acknowledgment)
        void on_persistent_congestion() {
            // Reset to minimum window
            cwnd_ = cc_constants::kMinimumWindow;
            ssthresh_ = cwnd_;
            state_ = CongestionState::SlowStart;

            echo::warn("CC: persistent congestion, reset cwnd=", cwnd_);
        }

        // Called when ECN-CE marks are received
        void on_ecn_ce_received() {
            // Treat ECN-CE like loss
            if (state_ != CongestionState::Recovery) {
                recovery_start_time_ = std::chrono::steady_clock::now();
                cwnd_ = std::max(static_cast<dp::u64>(cwnd_ * cc_constants::kLossReductionFactor),
                                 cc_constants::kMinimumWindow);
                ssthresh_ = cwnd_;
                state_ = CongestionState::Recovery;

                echo::debug("CC: ECN-CE received, entering recovery, cwnd=", cwnd_);
            }
        }

        // Check for persistent congestion
        // Returns true if persistent congestion is detected
        bool check_persistent_congestion(const dp::Vector<SentPacketInfo> &lost_packets,
                                         const RttEstimator &rtt) const {
            if (lost_packets.size() < 2) {
                return false;
            }

            // Find the earliest and latest lost packets
            std::chrono::steady_clock::time_point earliest;
            std::chrono::steady_clock::time_point latest;
            bool first = true;

            for (const auto &pkt : lost_packets) {
                if (first) {
                    earliest = latest = pkt.sent_time;
                    first = false;
                } else {
                    if (pkt.sent_time < earliest) {
                        earliest = pkt.sent_time;
                    }
                    if (pkt.sent_time > latest) {
                        latest = pkt.sent_time;
                    }
                }
            }

            // Persistent congestion period = (smoothed_rtt + max(4*rttvar, granularity) + max_ack_delay) * 3
            auto pto = rtt.pto();
            auto persistent_duration = pto * constants::kPersistentCongestionThreshold;

            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(latest - earliest);

            return duration >= persistent_duration;
        }

        // Pacing rate (bytes per second) - simple implementation
        // Full implementation would use RTT samples for more accurate pacing
        dp::u64 pacing_rate(const RttEstimator &rtt) const {
            if (rtt.smoothed_rtt.count() == 0) {
                return cwnd_ * 1000000 / constants::kInitialRtt.count(); // Use initial RTT
            }
            // Rate = cwnd / smoothed_rtt (in bytes per second)
            return cwnd_ * 1000000 / rtt.smoothed_rtt.count();
        }

        // Set max datagram size (affects minimum window calculations)
        void set_max_datagram_size(dp::u64 size) { max_datagram_size_ = size; }

      private:
        dp::u64 cwnd_;                                              // Congestion window
        dp::u64 ssthresh_;                                          // Slow start threshold
        dp::u64 bytes_in_flight_;                                   // Bytes currently in flight
        CongestionState state_;                                     // Current state
        std::chrono::steady_clock::time_point recovery_start_time_; // When recovery started
        dp::u64 max_datagram_size_ = cc_constants::kMaxDatagramSize;
    };

} // namespace netpipe::quic

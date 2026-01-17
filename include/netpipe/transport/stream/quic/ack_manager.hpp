#pragma once

#include <algorithm>
#include <chrono>
#include <datapod/datapod.hpp>
#include <echo/echo.hpp>
#include <netpipe/transport/stream/quic/frame.hpp>
#include <netpipe/transport/stream/quic/types.hpp>
#include <set>

namespace netpipe::quic {

    // ACK manager constants
    namespace ack_constants {
        // Maximum number of ACK ranges to track
        constexpr dp::usize kMaxAckRanges = 256;

        // Maximum ACK delay to report (in microseconds)
        constexpr dp::u64 kMaxAckDelay = 25000;

        // ACK delay exponent (default)
        constexpr dp::u32 kAckDelayExponent = 3;

        // Maximum number of packets to receive before sending ACK
        constexpr dp::u32 kMaxAckElicitingPackets = 2;
    } // namespace ack_constants

    // Range of packet numbers [start, end] inclusive
    struct PacketNumberRange {
        dp::u64 start;
        dp::u64 end;

        dp::u64 size() const { return end - start + 1; }

        bool contains(dp::u64 pn) const { return pn >= start && pn <= end; }

        // Check if this range is adjacent to or overlaps another
        bool adjacent_or_overlaps(const PacketNumberRange &other) const {
            return !(end + 1 < other.start || other.end + 1 < start);
        }

        // Merge with another range (assumes adjacent or overlapping)
        void merge(const PacketNumberRange &other) {
            start = std::min(start, other.start);
            end = std::max(end, other.end);
        }

        bool operator<(const PacketNumberRange &other) const {
            // Sort by start, descending (largest first)
            return start > other.start;
        }
    };

    // ACK state for a single packet number space
    class AckState {
      public:
        AckState() = default;

        // Record a packet as received
        void on_packet_received(dp::u64 packet_number, bool ack_eliciting) {
            auto now = std::chrono::steady_clock::now();

            // Update largest received
            if (!has_received_ || packet_number > largest_received_) {
                largest_received_ = packet_number;
                largest_received_time_ = now;
            }
            has_received_ = true;

            // Add to received ranges
            add_to_ranges(packet_number);

            // Track ACK-eliciting packets
            if (ack_eliciting) {
                ack_eliciting_received_++;
                if (first_ack_eliciting_time_ == std::chrono::steady_clock::time_point{}) {
                    first_ack_eliciting_time_ = now;
                }
            }

            echo::trace("Packet received: pn=", packet_number, " ack_eliciting=", ack_eliciting,
                        " total_ack_eliciting=", ack_eliciting_received_);
        }

        // Check if we should send an ACK now
        bool should_send_ack() const {
            // Send ACK immediately after receiving kMaxAckElicitingPackets ack-eliciting packets
            if (ack_eliciting_received_ >= ack_constants::kMaxAckElicitingPackets) {
                return true;
            }

            // Check if ACK delay timer has expired
            if (ack_eliciting_received_ > 0 && first_ack_eliciting_time_ != std::chrono::steady_clock::time_point{}) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - first_ack_eliciting_time_);
                if (elapsed.count() >= static_cast<dp::i64>(max_ack_delay_us_)) {
                    return true;
                }
            }

            return false;
        }

        // Generate an ACK frame for received packets
        // Returns empty optional if no ACK is needed
        dp::Res<AckFrame> generate_ack_frame() {
            if (!has_received_ || received_ranges_.empty()) {
                return dp::result::err(dp::Error::not_found("no packets to acknowledge"));
            }

            AckFrame frame;
            frame.largest_ack = largest_received_;

            // Calculate ACK delay (time since largest packet was received)
            auto now = std::chrono::steady_clock::now();
            auto delay = std::chrono::duration_cast<std::chrono::microseconds>(now - largest_received_time_);
            // Encode with ack_delay_exponent
            frame.ack_delay = static_cast<dp::u64>(delay.count()) >> ack_delay_exponent_;

            // Build ACK ranges (sorted largest first)
            // First range is implicit (from largest_ack down)
            auto it = received_ranges_.begin();
            if (it->end != largest_received_) {
                // This shouldn't happen if we track correctly
                echo::warn("ACK range inconsistency: largest=", largest_received_, " but range end=", it->end);
            }

            // First ACK Range
            AckRange first;
            first.gap = 0;
            first.range = it->end - it->start; // Number of packets - 1
            frame.ack_ranges.push_back(first);

            // Additional ranges
            dp::u64 prev_smallest = it->start;
            ++it;

            while (it != received_ranges_.end() && frame.ack_ranges.size() < ack_constants::kMaxAckRanges) {
                AckRange range;
                // Gap = number of unacked packets between ranges - 1
                // gap = prev_smallest - 1 - it->end - 1 = prev_smallest - it->end - 2
                range.gap = prev_smallest - it->end - 2;
                range.range = it->end - it->start;
                frame.ack_ranges.push_back(range);

                prev_smallest = it->start;
                ++it;
            }

            // Reset ACK-eliciting tracking
            ack_eliciting_received_ = 0;
            first_ack_eliciting_time_ = std::chrono::steady_clock::time_point{};

            echo::trace("ACK frame generated: largest=", frame.largest_ack, " ranges=", frame.ack_ranges.size());

            return dp::result::ok(std::move(frame));
        }

        // Get the packet number ranges acknowledged by an ACK frame
        // Returns vector of (start, end) pairs
        static dp::Vector<std::pair<dp::u64, dp::u64>> decode_ack_ranges(const AckFrame &frame) {
            dp::Vector<std::pair<dp::u64, dp::u64>> ranges;

            if (frame.ack_ranges.empty()) {
                return ranges;
            }

            // First range: [largest_ack - first_range, largest_ack]
            dp::u64 range_end = frame.largest_ack;
            dp::u64 range_start = range_end - frame.ack_ranges[0].range;
            ranges.push_back({range_start, range_end});

            dp::u64 prev_smallest = range_start;

            // Additional ranges
            for (dp::usize i = 1; i < frame.ack_ranges.size(); i++) {
                // Gap packets are unacknowledged
                // Next range ends at: prev_smallest - gap - 2
                range_end = prev_smallest - frame.ack_ranges[i].gap - 2;
                range_start = range_end - frame.ack_ranges[i].range;
                ranges.push_back({range_start, range_end});

                prev_smallest = range_start;
            }

            return ranges;
        }

        // Set ACK delay exponent (from transport parameters)
        void set_ack_delay_exponent(dp::u32 exponent) { ack_delay_exponent_ = exponent; }

        // Set max ACK delay (from transport parameters)
        void set_max_ack_delay(dp::u64 delay_us) { max_ack_delay_us_ = delay_us; }

        // Get largest received packet number
        dp::u64 largest_received() const { return largest_received_; }
        bool has_received() const { return has_received_; }

        // Clear all state (e.g., when discarding a packet number space)
        void clear() {
            received_ranges_.clear();
            has_received_ = false;
            largest_received_ = 0;
            largest_received_time_ = std::chrono::steady_clock::time_point{};
            ack_eliciting_received_ = 0;
            first_ack_eliciting_time_ = std::chrono::steady_clock::time_point{};
        }

      private:
        // Add a packet number to the received ranges
        void add_to_ranges(dp::u64 packet_number) {
            PacketNumberRange new_range{packet_number, packet_number};

            // Check if we can extend an existing range
            for (auto it = received_ranges_.begin(); it != received_ranges_.end();) {
                if (it->contains(packet_number)) {
                    // Already in a range
                    return;
                }

                if (new_range.adjacent_or_overlaps(*it)) {
                    // Merge with this range
                    new_range.merge(*it);
                    it = received_ranges_.erase(it);
                } else {
                    ++it;
                }
            }

            // Insert the (possibly merged) range
            received_ranges_.insert(new_range);

            // Limit number of ranges
            while (received_ranges_.size() > ack_constants::kMaxAckRanges) {
                // Remove the oldest (smallest) range
                auto it = received_ranges_.end();
                --it;
                received_ranges_.erase(it);
            }
        }

        // Received packet number ranges (sorted largest first)
        std::set<PacketNumberRange> received_ranges_;

        // Largest received packet number
        dp::u64 largest_received_ = 0;
        bool has_received_ = false;
        std::chrono::steady_clock::time_point largest_received_time_;

        // ACK-eliciting packet tracking
        dp::u32 ack_eliciting_received_ = 0;
        std::chrono::steady_clock::time_point first_ack_eliciting_time_{};

        // Configuration
        dp::u32 ack_delay_exponent_ = ack_constants::kAckDelayExponent;
        dp::u64 max_ack_delay_us_ = ack_constants::kMaxAckDelay;
    };

    // ACK manager for all packet number spaces
    class AckManager {
      public:
        AckManager() = default;

        // Get ACK state for a space
        AckState &state(PacketNumberSpace space) { return states_[static_cast<int>(space)]; }
        const AckState &state(PacketNumberSpace space) const { return states_[static_cast<int>(space)]; }

        // Record packet received in a space
        void on_packet_received(PacketNumberSpace space, dp::u64 packet_number, bool ack_eliciting) {
            states_[static_cast<int>(space)].on_packet_received(packet_number, ack_eliciting);
        }

        // Check if any space needs to send ACK
        bool any_space_needs_ack() const {
            for (int i = 0; i < 3; i++) {
                if (states_[i].should_send_ack()) {
                    return true;
                }
            }
            return false;
        }

        // Get spaces that need ACKs
        dp::Vector<PacketNumberSpace> spaces_needing_ack() const {
            dp::Vector<PacketNumberSpace> result;
            for (int i = 0; i < 3; i++) {
                if (states_[i].should_send_ack()) {
                    result.push_back(static_cast<PacketNumberSpace>(i));
                }
            }
            return result;
        }

        // Generate ACK frame for a space (if needed)
        dp::Res<AckFrame> generate_ack_frame(PacketNumberSpace space) {
            return states_[static_cast<int>(space)].generate_ack_frame();
        }

        // Discard a packet number space
        void discard_space(PacketNumberSpace space) {
            states_[static_cast<int>(space)].clear();
            echo::debug("ACK state discarded for space ", static_cast<int>(space));
        }

        // Set transport parameters for all spaces
        void set_ack_delay_exponent(dp::u32 exponent) {
            for (int i = 0; i < 3; i++) {
                states_[i].set_ack_delay_exponent(exponent);
            }
        }

        void set_max_ack_delay(dp::u64 delay_us) {
            for (int i = 0; i < 3; i++) {
                states_[i].set_max_ack_delay(delay_us);
            }
        }

      private:
        AckState states_[3]; // Initial, Handshake, ApplicationData
    };

} // namespace netpipe::quic

#pragma once
#include "common/model.hpp"
#include <deque>
#include <span>
namespace dm {
std::vector<std::uint8_t> information_query(std::uint8_t option);
std::vector<std::uint8_t> interval_command(int milliseconds);
struct Frame {
    std::uint8_t type=0;
    std::vector<std::uint8_t> payload;
};
class Framer {
    std::vector<std::uint8_t> buffer_;
    std::deque<std::string> diagnostic_;
public:
    std::uint64_t rejected=0, unknown=0;
    std::uint64_t received_bytes=0, validated_frames=0, status_frames=0, crc_failures=0, tail_failures=0;
    std::vector<Frame> feed(std::span<const std::uint8_t> bytes);
    void reset();
    Json diagnostics() const;
};
Sample parse_status(std::span<const std::uint8_t> payload, const Clock& received, std::uint64_t sequence);
Json parse_info(std::span<const std::uint8_t> payload);
std::string parse_serial_number(std::span<const std::uint8_t> payload);
} // namespace dm

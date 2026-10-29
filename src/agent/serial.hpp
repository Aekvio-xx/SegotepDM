#pragma once
#include "common/model.hpp"
#include <span>
namespace dm {
struct Port { std::string name, instance, description; };
std::vector<Port> enumerate_ports();
std::optional<Port> resolve_port(const Config& config);
class SerialPort {
    Handle port_, read_event_;
    OVERLAPPED read_{};
    std::array<std::uint8_t,1024> buffer_{};
    bool pending_=false;
public:
    SerialPort();
    ~SerialPort();
    bool is_open() const { return static_cast<bool>(port_); }
    HANDLE read_event() const { return read_event_.get(); }
    void open(const Port& port,const Config& config);
    void close();
    void begin_read();
    std::optional<std::vector<std::uint8_t>> complete_read();
    void send(std::span<const std::uint8_t> bytes,HANDLE interrupt);
};
} // namespace dm

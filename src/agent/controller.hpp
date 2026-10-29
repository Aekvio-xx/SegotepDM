#pragma once
#include "agent/protocol.hpp"
#include "agent/serial.hpp"
#include "agent/storage.hpp"
#include "common/shared_memory.hpp"
#include <atomic>
#include <mutex>
#include <thread>
namespace dm {
class Controller {
    fs::path root_;
    Handle wake_,cancel_;
    mutable std::mutex command_mutex_,view_mutex_;
    Config desired_;
    std::uint64_t epoch_=0;
    bool suspended_=false;
    struct IntervalRequest {int ms;std::uint64_t epoch;std::string session;};
    std::optional<IntervalRequest> interval_;
    std::uint64_t last_request_=0;
    std::atomic<bool> stopping_=false;
    Json view_=Json::object();
    Storage storage_;
    std::string instance_;
    std::thread worker_;
    void run();
public:
    Controller(fs::path root,Config config);
    ~Controller();
    void configure(const Config& config);
    bool request_interval(int milliseconds,std::string& error);
    void devices_changed();
    void suspend(bool suspended);
    void stop();
    Json snapshot() const;
};
} // namespace dm

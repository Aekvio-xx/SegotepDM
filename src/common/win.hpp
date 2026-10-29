#pragma once
#include <windows.h>
#include <nlohmann/json.hpp>
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dm {
using Json = nlohmann::json;
namespace fs = std::filesystem;
class Handle {
    HANDLE h_ = nullptr;
public:
    Handle() = default;
    explicit Handle(HANDLE h) : h_(h) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& o) noexcept : h_(o.release()) {}
    Handle& operator=(Handle&& o) noexcept { if (this != &o) reset(o.release()); return *this; }
    explicit operator bool() const { return h_ && h_ != INVALID_HANDLE_VALUE; }
    HANDLE get() const { return h_; }
    HANDLE release() { HANDLE h = h_; h_ = nullptr; return h; }
    void reset(HANDLE h = nullptr) { if (*this) CloseHandle(h_); h_ = h; }
};
std::string utf8(std::wstring_view s);
std::wstring wide(std::string_view s);
std::string error_text(DWORD e = GetLastError());
std::string guid();
std::string sha256(std::string_view s);
fs::path executable_dir();
std::wstring current_sid();
std::wstring object_name(std::wstring_view purpose);
class UserSecurity {
    PSECURITY_DESCRIPTOR descriptor_ = nullptr;
    SECURITY_ATTRIBUTES attributes_{};
public:
    UserSecurity();
    ~UserSecurity();
    SECURITY_ATTRIBUTES* get() { return &attributes_; }
};
struct Clock {
    std::int64_t wall_ms = 0;
    std::uint64_t tick_ms = 0;
    int offset_minutes = 0;
    std::string utc, local, date, minute_local;
    std::int64_t minute_start_wall_ms = 0;
    static Clock now();
    static Clock at(std::int64_t wall, std::uint64_t tick);
};
std::string read_file(const fs::path& path, std::size_t limit);
void atomic_write(const fs::path& path, std::string_view data);
bool set_autostart(bool enabled, std::string& error);
bool autostart_matches();
bool storage_space_ok(const fs::path& root);
inline constexpr wchar_t kWindowClass[] = L"SegotepDM.TelemetryAgent.v1";
} // namespace dm

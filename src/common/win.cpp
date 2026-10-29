#include "common/win.hpp"
#include <sddl.h>
#include <bcrypt.h>
#include <objbase.h>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <cstdio>
#include <cstdlib>

namespace dm {
std::string utf8(std::wstring_view s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string r(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), r.data(), n, nullptr, nullptr);
    return r;
}
std::wstring wide(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring r(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), r.data(), n);
    return r;
}
std::string error_text(DWORD e) {
    wchar_t* p = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, e, 0, reinterpret_cast<wchar_t*>(&p), 0, nullptr);
    std::string r = p ? utf8(p) : "Windows error";
    if (p) LocalFree(p);
    while (!r.empty() && (r.back() == '\r' || r.back() == '\n')) r.pop_back();
    return r + " (" + std::to_string(e) + ")";
}
std::string guid() {
    GUID id{};
    if (FAILED(CoCreateGuid(&id))) throw std::runtime_error("Cannot create instance ID");
    wchar_t text[40]{};
    StringFromGUID2(id, text, 40);
    return utf8(text);
}
std::string sha256(std::string_view s) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("SHA256 provider unavailable");
    std::array<unsigned char, 32> digest{};
    auto status = BCryptHash(alg, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(s.data())),
        static_cast<ULONG>(s.size()), digest.data(), static_cast<ULONG>(digest.size()));
    BCryptCloseAlgorithmProvider(alg, 0);
    if (status < 0) throw std::runtime_error("SHA256 failed");
    std::ostringstream out;
    for (auto b : digest) out << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    return out.str();
}
fs::path executable_dir() {
    std::wstring s(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, s.data(), static_cast<DWORD>(s.size()));
    if (!n || n >= s.size()) throw std::runtime_error("Executable path unavailable");
    s.resize(n);
    return fs::path(s).parent_path();
}
std::wstring current_sid() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) throw std::runtime_error(error_text());
    Handle token(raw);
    DWORD bytes = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &bytes);
    std::vector<unsigned char> buffer(bytes);
    if (!GetTokenInformation(token.get(), TokenUser, buffer.data(), bytes, &bytes)) throw std::runtime_error(error_text());
    wchar_t* text = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text)) throw std::runtime_error(error_text());
    std::wstring result(text);
    LocalFree(text);
    return result;
}
std::wstring object_name(std::wstring_view purpose) {
    return L"Local\\SegotepDM." + std::wstring(purpose) + L".v1." + current_sid();
}
UserSecurity::UserSecurity() {
    auto sddl = L"D:P(A;;GA;;;" + current_sid() + L")";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor_, nullptr))
        throw std::runtime_error(error_text());
    attributes_ = {sizeof(SECURITY_ATTRIBUTES), descriptor_, FALSE};
}
UserSecurity::~UserSecurity() { if (descriptor_) LocalFree(descriptor_); }
Clock Clock::now() {
    FILETIME f{};
    GetSystemTimeAsFileTime(&f);
    ULARGE_INTEGER t{}; t.LowPart = f.dwLowDateTime; t.HighPart = f.dwHighDateTime;
    return at(static_cast<std::int64_t>(t.QuadPart / 10000ULL) - 11644473600000LL, GetTickCount64());
}
Clock Clock::at(std::int64_t wall, std::uint64_t tick) {
    Clock r; r.wall_ms = wall; r.tick_ms = tick;
    ULARGE_INTEGER n{}; n.QuadPart = static_cast<ULONGLONG>(wall + 11644473600000LL) * 10000ULL;
    FILETIME f{n.LowPart, n.HighPart};
    SYSTEMTIME utc{}, local{};
    FileTimeToSystemTime(&f, &utc);
    DYNAMIC_TIME_ZONE_INFORMATION tz{};
    GetDynamicTimeZoneInformation(&tz);
    if (!SystemTimeToTzSpecificLocalTimeEx(&tz, &utc, &local)) local = utc;
    FILETIME lf{};
    SystemTimeToFileTime(&local, &lf);
    ULARGE_INTEGER l{}; l.LowPart = lf.dwLowDateTime; l.HighPart = lf.dwHighDateTime;
    r.offset_minutes = static_cast<int>((static_cast<std::int64_t>(l.QuadPart) - static_cast<std::int64_t>(n.QuadPart)) / 600000000LL);
    auto format = [](const SYSTEMTIME& t) {
        char b[40]{};
        sprintf_s(b, "%04u-%02u-%02uT%02u:%02u:%02u.%03u", t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds);
        return std::string(b);
    };
    char offset[10]{};
    const int m = std::abs(r.offset_minutes);
    sprintf_s(offset, "%c%02d:%02d", r.offset_minutes < 0 ? '-' : '+', m/60, m%60);
    r.utc = format(utc) + "Z";
    r.local = format(local) + offset;
    r.date = r.local.substr(0, 10);
    r.minute_local = r.local.substr(0,16) + ":00.000" + offset;
    r.minute_start_wall_ms = wall - local.wSecond*1000LL - local.wMilliseconds;
    return r;
}
std::string read_file(const fs::path& path, std::size_t limit) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Cannot read " + utf8(path.wstring()));
    auto n = file.tellg();
    if (n < 0 || static_cast<std::uint64_t>(n) > limit) throw std::runtime_error("File exceeds size limit");
    std::string data(static_cast<std::size_t>(n), '\0');
    file.seekg(0);
    if (!data.empty() && !file.read(data.data(), static_cast<std::streamsize>(data.size()))) throw std::runtime_error("Incomplete file read");
    return data;
}
void atomic_write(const fs::path& path, std::string_view data) {
    fs::create_directories(path.parent_path());
    auto tmp = path; tmp += L".tmp." + std::to_wstring(GetCurrentProcessId());
    Handle f(CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!f) throw std::runtime_error(error_text());
    DWORD done = 0;
    if (!WriteFile(f.get(), data.data(), static_cast<DWORD>(data.size()), &done, nullptr) || done != data.size() || !FlushFileBuffers(f.get()))
        throw std::runtime_error(error_text());
    f.reset();
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) throw std::runtime_error(error_text());
}
static std::wstring startup_command() {
    return L"\"" + (executable_dir()/L"DmTelemetryAgent.exe").wstring() + L"\" --autostart";
}
bool set_autostart(bool enabled, std::string& error) {
    HKEY key{};
    auto rc = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (rc == ERROR_SUCCESS) {
        auto command = startup_command();
        if(enabled && command.size()>260) {RegCloseKey(key);error="自启动命令超过 Windows Run 的长度限制，请缩短程序路径";return false;}
        rc = enabled ? RegSetValueExW(key,L"SegotepDMTelemetryAgent",0,REG_SZ,reinterpret_cast<const BYTE*>(command.c_str()),static_cast<DWORD>((command.size()+1)*sizeof(wchar_t)))
                     : RegDeleteValueW(key,L"SegotepDMTelemetryAgent");
        RegCloseKey(key);
        if (!enabled && rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
    }
    if (rc != ERROR_SUCCESS) { error = error_text(rc); return false; }
    return true;
}
bool autostart_matches() {
    wchar_t value[32768]{}; DWORD bytes = sizeof(value);
    auto rc = RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",L"SegotepDMTelemetryAgent",RRF_RT_REG_SZ,nullptr,value,&bytes);
    return rc == ERROR_SUCCESS && startup_command() == value;
}
bool storage_space_ok(const fs::path& root) {
    ULARGE_INTEGER free{};
    if (!GetDiskFreeSpaceExW(root.c_str(), &free, nullptr, nullptr)) throw std::runtime_error(error_text());
    return free.QuadPart >= 100ULL*1024*1024;
}
} // namespace dm

#pragma once
#include "common/win.hpp"
namespace dm {
struct MetricDef { const char* id; const char* source; const char* unit; const wchar_t* label; unsigned offset; double scale; };
inline constexpr std::array<MetricDef,11> kMetrics{{
    {"input_power_w","power","W",L"输入功率",12,1.0/20},
    {"fan_speed_rpm","fan_speed","RPM",L"风扇转速",24,30},
    {"temperature_low_c","low_voltage_temprature","degC",L"低压侧温度",18,1.0/256},
    {"input_voltage_v","input_voltage","V",L"输入电压",22,0.1},
    {"rail_3v3_v","voltage_3v3","V",L"+3.3V 电压",0,0.001},
    {"rail_5v_v","voltage_5v","V",L"+5V 电压",2,0.001},
    {"rail_12v_v","voltage_12v","V",L"+12V 电压",4,0.001},
    {"rail_5v_sb_v","voltage_5v_sb","V",L"+5V SB 电压",6,0.001},
    {"input_frequency_hz","frequency","Hz",L"输入频率",14,0.1},
    {"temperature_high_c","high_voltage_temprature","degC",L"高压区温度",16,1.0/256},
    {"temperature_environment_c","environment_temprature","degC",L"环境温度",20,1.0/256}
}};
struct Sample {
    Clock received;
    std::array<double,11> values{};
    std::array<std::uint16_t,11> raw{};
    std::uint16_t fault = 0;
    std::uint8_t mode = 0;
    std::array<std::uint8_t,3> reserved{};
    std::uint64_t sequence = 0;
};
struct Config {
    bool serial_enabled = false, autostart = false;
    bool connect_on_startup = true;
    std::string port, device_instance;
    int data_bits = 8, stop_bits = 1;
    std::string parity = "N";
    bool dtr = false, rts = false;
    int csv_flush_seconds = 300, stale_after_ms = 10000, data_retention_days = 365;
    int last_requested_interval_ms = 2000;
};
Json config_json(const Config& c);
Config load_config(const fs::path& root, bool& first, std::string& error);
bool save_config(const fs::path& root, const Config& c, std::string& error);
Json metric_json(const std::optional<Sample>& sample, const Clock& now, bool online, int stale_ms);
inline constexpr char kRxProfile[] = "himos-1.6.1-rx-header-crc";
inline constexpr char kRxValidation[] = "dm850g_capture_observed_other_firmware_pending";
} // namespace dm

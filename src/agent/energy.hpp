#pragma once
#include "common/model.hpp"
#include <map>
#include <set>
namespace dm {
struct EnergyInput {
    std::string schema="2",agent,session,scope,date,updated,reason;
    std::uint64_t sequence=0,valid_ms=0;
    double mean_w=0,delta_kwh=0;
    bool has_data=false;
};
class EnergyBook {
    Json data_;
    bool incomplete_=false;
    std::string error_,run_id_;
    void cap();
    void period_add(Json& period,double kwh,std::uint64_t ms,const std::string& updated,std::uint64_t revision);
    void apply_unchecked(const EnergyInput& input);
public:
    EnergyBook();
    void recover(const fs::path& root);
    void save(const fs::path& root) const;
    bool apply(const EnergyInput& input);
    void resolve(const std::string& agent,const std::string& session,const std::string& usb,const std::string& psu);
    std::string canonical(const EnergyInput& input) const;
    std::string display_scope(const std::string& requested,bool paused) const;
    Json view(const std::string& scope,const Clock& now) const;
    std::vector<std::string> scopes() const;
    std::map<std::string,std::string> verified() const;
    std::vector<std::vector<std::string>> summaries(const Clock& at);
    std::vector<std::string> columns(const EnergyInput& input) const;
    std::uint64_t revision() const;
    std::string quality() const {return incomplete_?"recovery_incomplete":"estimated_partial";}
    const std::string& error() const {return error_;}
    void warn(std::string error);
};
std::vector<std::string> energy_csv_fields(std::string_view record);
std::string energy_csv_record(const std::vector<std::string>& fields);
std::string energy_number(double value);
} // namespace dm

#include "common/model.hpp"
#include <stdexcept>
namespace dm {
Json config_json(const Config& c) {
    return {{"schema_version",1},{"serial_enabled",c.serial_enabled},{"autostart",c.autostart},
        {"connect_on_startup",c.connect_on_startup},
        {"port",c.port},{"device_instance",c.device_instance},{"baud_rate",115200},{"data_bits",c.data_bits},
        {"stop_bits",c.stop_bits},{"parity",c.parity},{"dtr",c.dtr},{"rts",c.rts},{"flow_control","none"},
        {"serial_parameters_validation","115200_code_confirmed_other_parameters_pending"},
        {"rx_profile",kRxProfile},{"rx_profile_validation",kRxValidation},
        {"csv_flush_seconds",c.csv_flush_seconds},{"stale_after_ms",c.stale_after_ms},
        {"data_retention_days",c.data_retention_days},{"last_requested_interval_ms",c.last_requested_interval_ms}};
}
Config load_config(const fs::path& root, bool& first, std::string& error) {
    Config c; auto path = root/L"config"/L"settings.json";
    std::error_code ec; first = !fs::exists(path,ec);
    if (first && !ec) return c;
    first = false;
    try {
        auto j = Json::parse(read_file(path,65536));
        if (!j.is_object() || j.value("schema_version",0) != 1) throw std::runtime_error("Unsupported settings version");
        c.serial_enabled=j.value("serial_enabled",false); c.autostart=j.value("autostart",false);
        c.connect_on_startup=j.value("connect_on_startup",true);
        c.port=j.value("port",std::string{}); c.device_instance=j.value("device_instance",std::string{});
        c.data_bits=j.value("data_bits",8); c.stop_bits=j.value("stop_bits",1); c.parity=j.value("parity",std::string("N"));
        c.dtr=j.value("dtr",false); c.rts=j.value("rts",false);
        c.csv_flush_seconds=j.value("csv_flush_seconds",300); c.stale_after_ms=j.value("stale_after_ms",10000);
        c.data_retention_days=j.value("data_retention_days",365);
        c.last_requested_interval_ms=j.value("last_requested_interval_ms",2000);
        if ((c.data_bits!=7 && c.data_bits!=8) || (c.stop_bits!=1 && c.stop_bits!=2) ||
            (c.parity!="N" && c.parity!="E" && c.parity!="O") || c.csv_flush_seconds<60 || c.csv_flush_seconds>3600 ||
            c.stale_after_ms<5000 || c.stale_after_ms>20000 || c.data_retention_days<0 || c.data_retention_days>36500 ||
            c.last_requested_interval_ms<1000 || c.last_requested_interval_ms>3000 || c.last_requested_interval_ms%500 ||
            c.port.size()>32 || c.device_instance.size()>4096 ||
            j.value("baud_rate",115200)!=115200 || j.value("flow_control",std::string("none"))!="none" ||
            (j.value("rx_profile",std::string(kRxProfile))!=kRxProfile &&
             j.value("rx_profile",std::string(kRxProfile))!="himos-1.6.1-candidate"))
            throw std::runtime_error("Invalid settings; serial stays disabled");
        if (c.serial_enabled && c.port.empty()) throw std::runtime_error("No selected serial port");
        return c;
    } catch (const std::exception& e) {
        error=std::string("配置读取失败，串口保持关闭：")+e.what();
        Config fallback;fallback.connect_on_startup=false;fallback.serial_enabled=false;
        return fallback;
    }
}
bool save_config(const fs::path& root,const Config& c,std::string& error) {
    try { atomic_write(root/L"config"/L"settings.json",config_json(c).dump(2)+"\n"); return true; }
    catch(const std::exception& e) { error=std::string("配置未保存：")+e.what(); return false; }
}
Json metric_json(const std::optional<Sample>& s,const Clock& now,bool online,int stale_ms) {
    Json result=Json::object();
    for(std::size_t i=0;i<kMetrics.size();++i) {
        auto& d=kMetrics[i];
        Json m={{"unit",d.unit},{"source_field",d.source},{"evidence","application_code"},{"device_sample_time",nullptr},
            {"validity",s?"parseable":"missing"},{"freshness",!online?"offline":(!s || now.tick_ms-s->received.tick_ms>=static_cast<unsigned>(stale_ms)?"stale":"fresh")}};
        m["value"]=s?Json(s->values[i]):Json(nullptr);
        m["raw_value"]=s?Json(s->raw[i]):Json(nullptr);
        m["received_at_utc"]=s?Json(s->received.utc):Json(nullptr);
        m["received_tick_ms"]=s?Json(s->received.tick_ms):Json(nullptr);
        m["age_ms"]=s?Json(now.tick_ms-s->received.tick_ms):Json(nullptr);
        result[d.id]=std::move(m);
    }
    return result;
}
} // namespace dm

#include "agent/energy.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <locale>
#include <regex>
#include <sstream>
#include <stdexcept>
namespace dm {
namespace {
constexpr std::size_t kMaxScopes=16,kMaxStreams=8192,kMaxAliases=4096,kMaxPeriods=8192;
constexpr std::size_t kMaxLedger=8*1024*1024,kMaxCheckpoint=1024*1024;
std::string session_key(std::string_view agent,std::string_view session) {return sha256(std::string(agent)+"|"+std::string(session));}
std::string stream_key(const EnergyInput& in) {return sha256(in.schema+"|"+in.agent+"|"+in.session);}
bool date_ok(const std::string& s) {
    static const std::regex date_pattern("^[0-9]{4}-[0-9]{2}-[0-9]{2}$");
    if(!std::regex_match(s,date_pattern))return false;
    int y=std::stoi(s.substr(0,4)),m=std::stoi(s.substr(5,2)),d=std::stoi(s.substr(8,2));
    static constexpr int days[]{31,28,31,30,31,30,31,31,30,31,30,31};
    return y>=1970 && m>=1 && m<=12 && d>=1 && d<=days[m-1]+(m==2 && y%4==0 && (y%100!=0||y%400==0)?1:0);
}
bool month_ok(const std::string& s) {return s.size()==7 && date_ok(s+"-01");}
std::string previous_month(const std::string& month) {
    int year=std::stoi(month.substr(0,4)),m=std::stoi(month.substr(5,2));if(--m==0){m=12;--year;}char b[16]{};sprintf_s(b,"%04d-%02d",year,m);return b;
}
bool scope_ok(const std::string& s) {return s.size()<=96 && (s.starts_with("psu:")||s.starts_with("usb:"));}
std::uint64_t counter(const Json& v) {
    if(v.is_number_unsigned())return v.get<std::uint64_t>();
    if(v.is_number_integer() && v.get<std::int64_t>()>=0)return static_cast<std::uint64_t>(v.get<std::int64_t>());
    if(v.is_string()) {
        auto s=v.get<std::string>();if(s.empty()||s.size()>20||!std::all_of(s.begin(),s.end(),[](char c){return c>='0'&&c<='9';}))throw std::runtime_error("无效电量计数");
        std::size_t n=0;auto out=std::stoull(s,&n);if(n!=s.size())throw std::runtime_error("无效电量计数");return out;
    }
    throw std::runtime_error("无效电量计数类型");
}
double numeric(std::string_view s) {
    if(s.empty())throw std::runtime_error("空数值");
    std::istringstream input{std::string(s)};input.imbue(std::locale::classic());double n=0;input>>n;
    if(!input||!std::isfinite(n)||(input>>std::ws&&!input.eof()))throw std::runtime_error("无效电量数值");
    return n;
}
Json scope_data() {return {{"days",Json::object()},{"months",Json::object()},{"marks",Json::object()},{"as_of_utc",""},{"revision",0ULL}};}
Json empty_period() {return {{"kwh",0.0},{"valid_ms",0ULL},{"updated_utc",""},{"revision",0ULL}};}
std::uint64_t next_revision(Json& data) {
    auto n=counter(data.at("revision"));if(n==std::numeric_limits<std::uint64_t>::max())throw std::runtime_error("电量版本计数已满");
    data["revision"]=++n;return n;
}
void validate_period(const Json& p) {
    auto kwh=p.at("kwh").get<double>();if(!std::isfinite(kwh)||kwh<0||kwh>1e9)throw std::runtime_error("无效账本电量");
    if(counter(p.at("valid_ms"))>10000000000000ULL)throw std::runtime_error("账本覆盖时长超限");counter(p.at("revision"));
    if(!p.at("updated_utc").is_string()||p.at("updated_utc").get_ref<const std::string&>().size()>48)throw std::runtime_error("无效账本时间");
}
template<class F> void read_csv(const fs::path& path,F callback,std::function<void(const std::string&)> issue={}) {
    std::ifstream input(path,std::ios::binary);if(!input)throw std::runtime_error("无法读取历史 CSV");
    std::vector<std::string> headers;std::string line,record;bool quoted=false;std::size_t rows=0;
    while(std::getline(input,line)) {
        if(input.eof()){if(issue)issue("历史CSV末条未以换行结束，未计入恢复");break;}
        if(!line.empty()&&line.back()=='\r')line.pop_back();
        for(std::size_t i=0;i<line.size();++i)if(line[i]=='"') {if(quoted&&i+1<line.size()&&line[i+1]=='"')++i;else quoted=!quoted;}
        if(!record.empty())record+='\n';record+=line;
        if(record.size()>2*1024*1024)throw std::runtime_error("历史 CSV 单行超过 2 MiB");
        if(quoted)continue;
        if(record.empty())continue;
        auto fields=energy_csv_fields(record);record.clear();
        if(headers.empty()) {
            headers=std::move(fields);if(headers[0].starts_with("\xef\xbb\xbf"))headers[0].erase(0,3);
            if(headers.size()<76 || headers[0]!="schema_version")throw std::runtime_error("历史 CSV 表头无效");
        } else {
            if(++rows>250000)throw std::runtime_error("历史 CSV 单文件超过 250000 行");
            if(fields.size()!=headers.size()){if(issue)issue("历史CSV列数不符，已跳过该记录");continue;}
            callback(headers,fields);
        }
    }
    if(!input.eof())throw std::runtime_error("历史 CSV 读取失败");
    if(!record.empty()&&issue)issue("历史CSV引号或尾条目不完整，已排除");
}
std::string field(const std::vector<std::string>& h,const std::vector<std::string>& f,std::string_view name) {
    auto it=std::find(h.begin(),h.end(),name);return it==h.end()?std::string{}:f[static_cast<std::size_t>(it-h.begin())];
}
EnergyInput input_from(const std::vector<std::string>& h,const std::vector<std::string>& f) {
    EnergyInput in;in.schema=f[0];in.agent=f[1];in.sequence=counter(Json(f[2]));in.session=f[3];in.scope=field(h,f,"energy_scope_id");
    if(in.scope.empty())in.scope=f[4];
    if(in.agent.empty()||in.agent.size()>160||in.session.size()>160)throw std::runtime_error("历史 CSV 实例标识无效");
    in.date=f[6].size()>=10?f[6].substr(0,10):std::string{};in.updated=f[8];in.reason=f[15];if(!f[75].empty()&&counter(Json(f[75]))>0)in.reason="csv_queue_drop";
    if(!date_ok(in.date)||!scope_ok(in.scope))throw std::runtime_error("历史 CSV 日期或设备标识无效");
    if(in.schema!="1"&&in.schema!="2")throw std::runtime_error("不支持的历史 CSV 版本");
    if(!f[16].empty()&&!f[20].empty()) {
        in.mean_w=numeric(f[16]);in.valid_ms=counter(Json(f[20]));in.has_data=in.valid_ms>0;
        if(in.mean_w<0||in.valid_ms>60000)throw std::runtime_error("历史 CSV 功率或覆盖时长超限");
    }
    in.delta_kwh=in.has_data?in.mean_w*static_cast<double>(in.valid_ms)/3600000000.0:0;
    if(in.schema=="2"&&in.has_data){auto delta=field(h,f,"energy_kwh");if(!delta.empty()){in.delta_kwh=numeric(delta);if(in.delta_kwh<0)throw std::runtime_error("负电量增量");}}
    return in;
}
}
std::string energy_number(double n) {
    if(!std::isfinite(n))return {};
    std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(17)<<n;return out.str();
}
std::vector<std::string> energy_csv_fields(std::string_view record) {
    std::vector<std::string> result;std::string value;bool quoted=false,ended=false;
    for(std::size_t i=0;i<record.size();++i) {
        char c=record[i];
        if(quoted) {if(c=='"'){if(i+1<record.size()&&record[i+1]=='"'){value+='"';++i;}else{quoted=false;ended=true;}}else value+=c;}
        else if(c==',') {result.push_back(std::move(value));value.clear();ended=false;}
        else if(c=='"'&&value.empty()&&!ended)quoted=true;
        else if(ended) {if(c!=' '&&c!='\t')throw std::runtime_error("CSV 引号尾部无效");}
        else if(c=='"')throw std::runtime_error("CSV 引号未转义");
        else value+=c;
        if(result.size()>128)throw std::runtime_error("CSV 列数超限");
    }
    if(quoted)throw std::runtime_error("CSV 引号未结束");
    result.push_back(std::move(value));return result;
}
std::string energy_csv_record(const std::vector<std::string>& fields) {
    std::string result;
    for(std::size_t i=0;i<fields.size();++i) {
        if(i)result+=',';const auto& s=fields[i];
        if(s.find_first_of(",\"\r\n")==std::string::npos)result+=s;
        else {result+='"';for(char c:s){if(c=='"')result+='"';result+=c;}result+='"';}
    }
    return result+"\r\n";
}
EnergyBook::EnergyBook():data_({{"version",1},{"revision",0ULL},{"scopes",Json::object()},{"resolved",Json::object()},{"verified",Json::object()},{"unresolved",Json::object()},{"incomplete",false},{"error",""},{"active_month",Clock::now().date.substr(0,7)}}),run_id_(guid()) {}
void EnergyBook::warn(std::string error) {
    incomplete_=true;error.resize(std::min<std::size_t>(error.size(),768));if(error_.empty())error_=std::move(error);
    data_["incomplete"]=true;data_["error"]=error_;
}
std::uint64_t EnergyBook::revision() const {return counter(data_.at("revision"));}
void EnergyBook::cap() {
    if(data_["scopes"].size()>kMaxScopes||data_["resolved"].size()>kMaxAliases||data_["unresolved"].size()>256||data_["verified"].size()>64)throw std::runtime_error("电量账本设备或会话数量达到上限");
    std::size_t periods=0,streams=0;
    for(auto& s:data_["scopes"]) {periods+=s["days"].size()+s["months"].size();streams+=s["marks"].size();}
    for(auto& item:data_["unresolved"])periods+=item.at("days").size();
    if(periods>kMaxPeriods||streams>kMaxStreams)throw std::runtime_error("电量账本历史数量达到上限");
}
void EnergyBook::period_add(Json& p,double kwh,std::uint64_t ms,const std::string& updated,std::uint64_t revision) {
    if(!p.is_object())p=empty_period();
    auto old=counter(p["valid_ms"]);if(old>std::numeric_limits<std::uint64_t>::max()-ms)throw std::runtime_error("电量覆盖时长溢出");
    auto total=p["kwh"].get<double>()+kwh;if(!std::isfinite(total)||total<0)throw std::runtime_error("电量累计溢出");
    p["kwh"]=total;p["valid_ms"]=old+ms;p["updated_utc"]=updated;p["revision"]=revision;
}
std::string EnergyBook::canonical(const EnergyInput& in) const {
    auto key=session_key(in.agent,in.session);
    auto it=data_["resolved"].find(key);
    if(in.scope.starts_with("usb:")&&it!=data_["resolved"].end())return it->value("psu",in.scope);
    return in.scope;
}
void EnergyBook::apply_unchecked(const EnergyInput& original) {
    EnergyInput in=original;in.scope=canonical(in);
    if(!scope_ok(in.scope)||!date_ok(in.date))return;
    auto& scopes=data_["scopes"];
    if(!scopes.contains(in.scope)){if(scopes.size()>=kMaxScopes)throw std::runtime_error("电量设备数达到16个上限");scopes[in.scope]=scope_data();}
    auto& s=scopes[in.scope];auto stream=stream_key(in);
    std::size_t streams=0;for(auto& item:scopes)streams+=item["marks"].size();
    if(!s["marks"].contains(stream)&&streams>=kMaxStreams)throw std::runtime_error("电量检查点数量达到上限");
    std::size_t periods=0;for(auto& item:scopes)periods+=item["days"].size()+item["months"].size();for(auto& item:data_["unresolved"])periods+=item.at("days").size();
    if(in.has_data&&periods+3>kMaxPeriods)throw std::runtime_error("电量历史期间数量达到上限");
    s["marks"][stream]=std::max(counter(s["marks"].value(stream,Json(0ULL))),in.sequence);
    auto rev=next_revision(data_);s["as_of_utc"]=in.updated;s["revision"]=rev;
    if(in.has_data) {
        const double delta=in.delta_kwh;
        if(!std::isfinite(delta)||delta<0||in.valid_ms>60000)throw std::runtime_error("电量分钟增量无效");
        period_add(s["days"][in.date],delta,in.valid_ms,in.updated,rev);
        period_add(s["months"][in.date.substr(0,7)],delta,in.valid_ms,in.updated,rev);
        if(in.scope.starts_with("usb:")&&!in.session.empty()) {
            auto key=session_key(in.agent,in.session);auto& unresolved=data_["unresolved"];
            if(!unresolved.contains(key)&&unresolved.size()>=256) {warn("未归属设备会话超过256，部分历史身份不能迁移");}
            else {
                auto& item=unresolved[key];if(!item.is_object())item={{"usb",in.scope},{"stream",stream},{"days",Json::object()}};
                period_add(item["days"][in.date],delta,in.valid_ms,in.updated,rev);
            }
        }
    }
    if(in.reason=="clock_changed"||in.reason=="execution_gap"||in.reason=="gap_before_segment"||in.reason=="csv_queue_drop")warn("存在时间调整或执行缺口，电量仅包含有效记录");
    // Older day details roll into the persistent monthly totals; checkpoint marks prevent replay.
    for(auto& scope:scopes)for(auto it=scope["days"].begin();it!=scope["days"].end();) {
        if(it.key().substr(0,7)<previous_month(data_.value("active_month",in.date.substr(0,7))))it=scope["days"].erase(it);else ++it;
    }
    cap();
}
bool EnergyBook::apply(const EnergyInput& in) {
    if(!scope_ok(in.scope)||!date_ok(in.date))return false;
    auto scope=canonical(in),stream=stream_key(in);
    auto it=data_["scopes"].find(scope);
    if(it!=data_["scopes"].end()&&counter((*it)["marks"].value(stream,Json(0ULL)))>=in.sequence)return false;
    apply_unchecked(in);return true;
}
void EnergyBook::resolve(const std::string& agent,const std::string& session,const std::string& usb,const std::string& psu) {
    if(session.empty()||!usb.starts_with("usb:")||!psu.starts_with("psu:")||!scope_ok(usb)||!scope_ok(psu))return;
    const auto key=session_key(agent,session);
    const bool known=data_["resolved"].contains(key);
    if(known&&data_["resolved"][key].value("psu",std::string{})!=psu){warn("同一设备会话出现矛盾序列号，未合并电量");return;}
    if(known&&!data_["unresolved"].contains(key)&&data_["verified"].value(usb,std::string{})==psu)return;
    if(!known&&data_["resolved"].size()>=kMaxAliases)throw std::runtime_error("电量身份检查点达到上限");
    auto& scopes=data_["scopes"];if(!scopes.contains(psu)){if(scopes.size()>=kMaxScopes)throw std::runtime_error("电量设备数达到上限");scopes[psu]=scope_data();}
    auto revision=next_revision(data_);
    auto item=data_["unresolved"].find(key);
    if(item!=data_["unresolved"].end()&&scopes.contains(usb)) {
        auto& from=scopes[usb];auto& to=scopes[psu];
        auto stream=item->value("stream",std::string{});
        const bool already=from["marks"].contains(stream)&&counter(to["marks"].value(stream,Json(0ULL)))>=counter(from["marks"][stream]);
        for(auto p=item->at("days").begin();p!=item->at("days").end();++p) {
            auto period=p.key();const auto amount=p.value()["kwh"].get<double>();const auto ms=counter(p.value()["valid_ms"]);
            auto subtract=[&](Json& source,const std::string& name) {
                auto f=source.find(name);if(f==source.end())return;
                auto old=counter((*f)["valid_ms"]);(*f)["valid_ms"]=old>=ms?old-ms:0;
                (*f)["kwh"]=std::max(0.0,(*f)["kwh"].get<double>()-amount);
                if(counter((*f)["valid_ms"])==0)source.erase(f);
            };
            if(from["days"].contains(period)) {subtract(from["days"],period);if(!already)period_add(to["days"][period],amount,ms,p.value()["updated_utc"],revision);}
            subtract(from["months"],period.substr(0,7));if(!already)period_add(to["months"][period.substr(0,7)],amount,ms,p.value()["updated_utc"],revision);
        }
        if(from["marks"].contains(stream)){to["marks"][stream]=std::max(counter(to["marks"].value(stream,Json(0ULL))),counter(from["marks"][stream]));from["marks"].erase(stream);}
        if(to.value("as_of_utc",std::string{})<from.value("as_of_utc",std::string{}))to["as_of_utc"]=from["as_of_utc"];
        from["revision"]=revision;to["revision"]=revision;data_["unresolved"].erase(item);
    }
    data_["resolved"][key]={{"usb",usb},{"psu",psu}};
    data_["verified"][usb]=psu;cap();
}
std::string EnergyBook::display_scope(const std::string& requested,bool paused) const {
    if(paused) {auto it=data_["verified"].find(requested);if(it!=data_["verified"].end()&&it->is_string())return it->get<std::string>();}
    return requested;
}
std::map<std::string,std::string> EnergyBook::verified() const {
    std::map<std::string,std::string> result;for(auto it=data_["verified"].begin();it!=data_["verified"].end();++it)if(it.value().is_string())result[it.key()]=it.value().get<std::string>();return result;
}
std::vector<std::string> EnergyBook::scopes() const {
    std::vector<std::string> result;for(auto it=data_["scopes"].begin();it!=data_["scopes"].end();++it)result.push_back(it.key());return result;
}
Json EnergyBook::view(const std::string& scope,const Clock& now) const {
    Json result={{"version",1},{"basis","minute_mean"},{"scope_id",scope},{"date_local",now.date},{"as_of_utc",""},{"revision",revision()},
        {"today_kwh",nullptr},{"month_kwh",nullptr},{"year_kwh",nullptr},{"today_valid_ms",0ULL},{"status","missing"},
        {"history_quality",quality()},{"recovery_error",error_},{"days",Json::array()},{"months",Json::array()}};
    auto found=data_["scopes"].find(scope);if(found==data_["scopes"].end())return result;
    const auto& s=*found;result["as_of_utc"]=s.value("as_of_utc",std::string{});
    auto make=[&](const std::string& period,bool month) {
        const auto& items=s[month?"months":"days"];auto it=items.find(period);const bool has=it!=items.end()&&counter((*it)["valid_ms"])>0;
        return Json{{"period",period},{"kwh",has?(*it)["kwh"]:Json(nullptr)},{"valid_ms",has?(*it)["valid_ms"]:Json(0ULL)},
            {"has_data",has},{"finalized",period<(month?now.date.substr(0,7):now.date)},{"quality",quality()},
            {"updated_utc",it!=items.end()?(*it)["updated_utc"]:Json("")},{"revision",it!=items.end()?(*it)["revision"]:Json(0ULL)}};
    };
    auto day=make(now.date,false),month=make(now.date.substr(0,7),true);
    result["today_kwh"]=day["kwh"];result["today_valid_ms"]=day["valid_ms"];result["month_kwh"]=month["kwh"];
    double year=0;bool any=false;
    for(auto it=s["months"].begin();it!=s["months"].end();++it)if(it.key().substr(0,4)==now.date.substr(0,4)&&it.key()<=now.date.substr(0,7)&&counter(it.value()["valid_ms"])>0){year+=it.value()["kwh"].get<double>();any=true;}
    if(any&&std::isfinite(year))result["year_kwh"]=year;
    for(int d=1;d<=std::stoi(now.date.substr(8,2));++d){char b[4]{};sprintf_s(b,"%02d",d);result["days"].push_back(make(now.date.substr(0,8)+b,false));}
    for(int m=1;m<=std::stoi(now.date.substr(5,2));++m){char b[4]{};sprintf_s(b,"%02d",m);result["months"].push_back(make(now.date.substr(0,5)+b,true));}
    return result;
}
std::vector<std::string> EnergyBook::columns(const EnergyInput& in) const {
    const auto scope=canonical(in);Clock at{};at.date=in.date;auto totals=view(scope,at);
    const auto amount=in.has_data?energy_number(in.delta_kwh):std::string{};
    auto number=[](const Json& n){return n.is_number()?energy_number(n.get<double>()):std::string{};};
    return {"telemetry",scope,in.date,amount,std::to_string(in.has_data?in.valid_ms:0),in.has_data?"true":"false","false",quality(),in.updated,std::to_string(revision()),
        number(totals["today_kwh"]),number(totals["month_kwh"]),number(totals["year_kwh"]),"","",""};
}
std::vector<std::vector<std::string>> EnergyBook::summaries(const Clock& at) {
    std::vector<std::vector<std::string>> rows;data_["active_month"]=at.date.substr(0,7);
    for(const auto& scope:scopes()) {
        auto rev=next_revision(data_);auto snapshot=sha256(run_id_+"|"+at.date+"|"+scope+"|"+std::to_string(rev));
        data_["scopes"][scope]["revision"]=rev;
        const auto& s=data_["scopes"][scope];auto current=view(scope,at);std::uint64_t sequence=0;
        auto row=[&](std::string kind,const Json& period) {
            std::vector<std::string> f(92);f[0]="2";f[1]="energy-summary:"+snapshot;f[2]=std::to_string(++sequence);f[4]=scope;
            f[76]=std::move(kind);f[77]=scope;f[78]=period["period"].get<std::string>();
            const bool has=period.value("has_data",false);f[79]=has?energy_number(period["kwh"].get<double>()):"";
            f[80]=std::to_string(counter(period["valid_ms"]));f[81]=has?"true":"false";f[82]=period.value("finalized",false)?"true":"false";
            f[83]=quality();f[84]=at.utc;f[85]=std::to_string(rev);f[89]=snapshot;return f;
        };
        std::size_t count=0;
        for(const auto& p:current["days"]){rows.push_back(row("energy_day",p));++count;}
        auto months=s["months"];if(!months.contains(at.date.substr(0,7)))months[at.date.substr(0,7)]=empty_period();
        for(auto it=months.begin();it!=months.end();++it) {
            const bool has=counter(it.value()["valid_ms"])>0;
            Json p={{"period",it.key()},{"kwh",it.value()["kwh"]},{"valid_ms",it.value()["valid_ms"]},{"has_data",has},{"finalized",it.key()<at.date.substr(0,7)}};
            rows.push_back(row("energy_month",p));++count;
        }
        if(count>4096)throw std::runtime_error("电量汇总超过4096行");
        Json checkpoint={{"streams",s["marks"]},{"resolved",Json::object()},{"verified",Json::object()},{"unresolved",Json::object()}};
        for(auto it=data_["unresolved"].begin();it!=data_["unresolved"].end();++it)if(it.value().value("usb",std::string{})==scope)checkpoint["unresolved"][it.key()]=it.value();
        for(auto it=data_["resolved"].begin();it!=data_["resolved"].end();++it)if(it.value().value("psu",std::string{})==scope)checkpoint["resolved"][it.key()]=it.value();
        for(auto it=data_["verified"].begin();it!=data_["verified"].end();++it)if(it.value()==scope)checkpoint["verified"][it.key()]=it.value();
        auto encoded=checkpoint.dump();if(encoded.size()>kMaxCheckpoint)throw std::runtime_error("电量检查点超过1 MiB");
        Json p={{"period",at.date},{"valid_ms",0ULL},{"has_data",false},{"finalized",false}};
        auto seal=row("energy_snapshot",p);seal[90]=std::to_string(count);seal[91]=std::move(encoded);rows.push_back(std::move(seal));
    }
    return rows;
}
void EnergyBook::save(const fs::path& root) const {
    auto text=data_.dump(-1,' ',false,Json::error_handler_t::replace);if(text.size()>kMaxLedger)throw std::runtime_error("电量账本超过8 MiB");
    atomic_write(root/L"config"/L"energy_history.json",text+"\n");
}
void EnergyBook::recover(const fs::path& root) {
    const auto path=root/L"config"/L"energy_history.json";
    if(fs::exists(path))try {
        auto loaded=Json::parse(read_file(path,kMaxLedger));
        if(loaded.value("version",0)!=1||!loaded["scopes"].is_object()||!loaded["resolved"].is_object()||!loaded["verified"].is_object()||!loaded["unresolved"].is_object())throw std::runtime_error("电量账本结构无效");
        counter(loaded.at("revision"));
        for(auto s=loaded["scopes"].begin();s!=loaded["scopes"].end();++s) {
            if(!scope_ok(s.key())||!s.value()["days"].is_object()||!s.value()["months"].is_object()||!s.value()["marks"].is_object())throw std::runtime_error("电量设备账本无效");
            for(auto p=s.value()["days"].begin();p!=s.value()["days"].end();++p){if(!date_ok(p.key()))throw std::runtime_error("账本日期无效");validate_period(p.value());}
            for(auto p=s.value()["months"].begin();p!=s.value()["months"].end();++p){if(!month_ok(p.key()))throw std::runtime_error("账本月份无效");validate_period(p.value());}
            counter(s.value().value("revision",Json(0ULL)));
            for(auto m=s.value()["marks"].begin();m!=s.value()["marks"].end();++m){if(m.key().size()!=64)throw std::runtime_error("账本stream标识无效");counter(m.value());}
        }
        for(auto it=loaded["resolved"].begin();it!=loaded["resolved"].end();++it)
            if(it.key().size()!=64||!it.value().is_object()||!scope_ok(it.value().value("psu",std::string{}))||(!it.value().value("usb",std::string{}).empty()&&!scope_ok(it.value().value("usb",std::string{}))))throw std::runtime_error("账本会话映射无效");
        for(auto it=loaded["verified"].begin();it!=loaded["verified"].end();++it)if(!scope_ok(it.key())||!it.value().is_string()||!scope_ok(it.value().get<std::string>()))throw std::runtime_error("账本设备映射无效");
        for(auto it=loaded["unresolved"].begin();it!=loaded["unresolved"].end();++it) {
            if(it.key().size()!=64||!it.value().is_object()||!scope_ok(it.value().value("usb",std::string{}))||it.value().value("stream",std::string{}).size()!=64||!it.value()["days"].is_object())throw std::runtime_error("账本待定会话无效");
            for(auto p=it.value()["days"].begin();p!=it.value()["days"].end();++p){if(!date_ok(p.key()))throw std::runtime_error("待定会话日期无效");validate_period(p.value());}
        }
        data_=std::move(loaded);incomplete_=data_.value("incomplete",false);error_=data_.value("error",std::string{});error_.resize(std::min<std::size_t>(error_.size(),768));cap();
    } catch(const std::exception& e) {data_=EnergyBook().data_;incomplete_=false;error_.clear();warn(std::string("账本恢复失败，尝试CSV：")+e.what());}
    data_["active_month"]=Clock::now().date.substr(0,7);
    std::vector<fs::path> files;std::uintmax_t bytes=0;
    const std::regex history_file_pattern("^telemetry_[0-9]{4}-[0-9]{2}-[0-9]{2}(?:_v2)?\\.csv$");
    if(fs::exists(root/L"data"))for(auto& file:fs::directory_iterator(root/L"data")) {
        if(!file.is_regular_file()||file.is_symlink()||!std::regex_match(utf8(file.path().filename().wstring()),history_file_pattern))continue;
        if(files.size()>=1024||file.file_size()>32ULL*1024*1024||bytes+file.file_size()>512ULL*1024*1024){warn("历史恢复达到文件数量或512 MiB读取上限，部分历史未核对");continue;}
        bytes+=file.file_size();files.push_back(file.path());
    }
    std::sort(files.begin(),files.end());
    struct Snapshot {std::string scope,date,updated;std::uint64_t revision=0;std::size_t expected=0;Json checkpoint;std::map<std::string,Json> days,months;bool sealed=false,bad=false,incomplete=false;};
    std::map<std::string,Snapshot> groups;std::map<std::string,Snapshot> latest;
    struct Proof {std::string agent,session,usb,psu;bool conflict=false;};std::map<std::string,Proof> proofs;
    auto accept_snapshot=[&](const Snapshot& group) {
        if(group.bad||!group.sealed||group.expected!=group.days.size()+group.months.size()||group.expected>4096)return;
        auto it=latest.find(group.scope);if(it==latest.end()||group.revision>it->second.revision)latest[group.scope]=group;
    };
    // First pass finds complete carried snapshots and proven same-session identities.
    for(const auto& file:files)try {
        groups.clear();
        read_csv(file,[&](const auto& h,const auto& f) {
            auto type=field(h,f,"record_type");
            if(type.empty()||type=="telemetry") {
                if(scope_ok(f[4])&&!f[3].empty()&&f[1].size()<=160&&f[3].size()<=160) {
                    auto key=session_key(f[1],f[3]);if(proofs.size()>=kMaxAliases&&!proofs.contains(key)){warn("历史设备身份数量超限");return;}
                    auto& p=proofs[key];p.agent=f[1];p.session=f[3];
                    if(f[4].starts_with("usb:")){if(p.usb.empty())p.usb=f[4];else if(p.usb!=f[4]){p.conflict=true;warn("同会话USB身份冲突，未自动归并");}}
                    if(f[4].starts_with("psu:")){if(p.psu.empty())p.psu=f[4];else if(p.psu!=f[4]){p.conflict=true;warn("同会话PSU身份冲突，未自动归并");}}
                }
                return;
            }
            if(type!="energy_day"&&type!="energy_month"&&type!="energy_snapshot")return;
            auto id=field(h,f,"energy_snapshot_id"),scope=field(h,f,"energy_scope_id"),period=field(h,f,"energy_period");
            if(id.empty()||id.size()>128||!scope_ok(scope))return;
            if(groups.size()>=256&&!groups.contains(id)){warn("历史汇总快照数量超限");return;}
            auto& g=groups[id];const auto rev=counter(Json(field(h,f,"energy_revision")));
            if(!g.scope.empty()&&(g.scope!=scope||g.revision!=rev)){g.bad=true;return;}
            g.scope=scope;g.revision=rev;g.updated=field(h,f,"energy_updated_utc");
            if(field(h,f,"energy_quality")=="recovery_incomplete")g.incomplete=true;
            if(type=="energy_snapshot") {
                g.date=period;g.expected=static_cast<std::size_t>(counter(Json(field(h,f,"energy_snapshot_rows"))));g.sealed=date_ok(period);
                auto cp=field(h,f,"energy_checkpoint");if(cp.empty()||cp.size()>kMaxCheckpoint){g.bad=true;return;}
                g.checkpoint=Json::parse(cp);if(!g.checkpoint["streams"].is_object()||g.checkpoint["streams"].size()>kMaxStreams){g.bad=true;return;}
                accept_snapshot(g);return;
            }
            if((type=="energy_day"&&!date_ok(period))||(type=="energy_month"&&!month_ok(period))){g.bad=true;return;}
            const bool has=field(h,f,"energy_has_data")=="true";auto ms=counter(Json(field(h,f,"energy_valid_ms")));
            double kwh=has?numeric(field(h,f,"energy_kwh")):0;
            if(kwh<0||kwh>1e9||ms>10000000000000ULL||(has&&ms==0)||(!has&&ms)){g.bad=true;return;}
            Json value={{"kwh",kwh},{"valid_ms",has?ms:0},{"updated_utc",g.updated},{"revision",rev}};
            auto& values=type=="energy_day"?g.days:g.months;
            if(values.contains(period)&&values[period]!=value)g.bad=true;
            values[period]=std::move(value);
        },[&](const std::string& message){warn(message);});
        for(const auto& [id,g]:groups)if(g.bad||!g.sealed||g.expected!=g.days.size()+g.months.size())warn("历史CSV存在未完成或矛盾的电量快照，已忽略");
    } catch(const std::exception& e){warn(std::string("历史汇总读取不完整：")+e.what());}
    for(auto& [scope,g]:latest) {auto before=data_;try {
        auto current=data_["scopes"].find(scope);std::uint64_t maximum=current!=data_["scopes"].end()?counter(current->value("revision",Json(0ULL))):0;
        if(current!=data_["scopes"].end())for(auto& p:(*current)["months"])maximum=std::max(maximum,counter(p["revision"]));
        if(g.revision<=maximum)continue;
        for(auto it=g.checkpoint["streams"].begin();it!=g.checkpoint["streams"].end();++it)if(it.key().size()!=64)throw std::runtime_error("汇总stream标识无效");
        for(const auto& name:{"resolved","verified","unresolved"})if(g.checkpoint.contains(name)&&!g.checkpoint[name].is_object())throw std::runtime_error("汇总身份检查点结构无效");
        if(g.checkpoint.value("resolved",Json::object()).size()>kMaxAliases||g.checkpoint.value("unresolved",Json::object()).size()>256||g.checkpoint.value("verified",Json::object()).size()>64)throw std::runtime_error("汇总身份检查点超过上限");
        if(g.checkpoint.contains("resolved"))for(auto it=g.checkpoint["resolved"].begin();it!=g.checkpoint["resolved"].end();++it)
            if(it.key().size()!=64||!it.value().is_object()||it.value().value("psu",std::string{})!=scope||(!it.value().value("usb",std::string{}).empty()&&!scope_ok(it.value().value("usb",std::string{}))))throw std::runtime_error("汇总已确认会话无效");
        if(g.checkpoint.contains("verified"))for(auto it=g.checkpoint["verified"].begin();it!=g.checkpoint["verified"].end();++it)
            if(!scope_ok(it.key())||!it.value().is_string()||it.value().get<std::string>()!=scope)throw std::runtime_error("汇总已确认设备无效");
        if(g.checkpoint.contains("unresolved"))for(auto it=g.checkpoint["unresolved"].begin();it!=g.checkpoint["unresolved"].end();++it) {
            if(it.key().size()!=64||!it.value().is_object()||it.value().value("usb",std::string{})!=scope||it.value().value("stream",std::string{}).size()!=64||!it.value()["days"].is_object())throw std::runtime_error("汇总待定会话无效");
            for(auto p=it.value()["days"].begin();p!=it.value()["days"].end();++p){if(!date_ok(p.key()))throw std::runtime_error("汇总待定日期无效");validate_period(p.value());}
        }
        Json s=scope_data();for(auto& [key,value]:g.days)s["days"][key]=value;for(auto& [key,value]:g.months)s["months"][key]=value;
        for(auto it=g.checkpoint["streams"].begin();it!=g.checkpoint["streams"].end();++it)s["marks"][it.key()]=counter(it.value());
        s["as_of_utc"]=g.updated;s["revision"]=g.revision;data_["scopes"][scope]=std::move(s);
        if(g.incomplete)warn("已恢复的CSV汇总标记历史不完整");
        for(const auto& name:{"resolved","verified","unresolved"})if(g.checkpoint.contains(name)&&g.checkpoint[name].is_object())
            for(auto it=g.checkpoint[name].begin();it!=g.checkpoint[name].end();++it)data_[name][it.key()]=it.value();
        data_["revision"]=std::max(revision(),g.revision);cap();
    }catch(const std::exception& e){data_=std::move(before);warn(std::string("汇总检查点恢复不完整：")+e.what());}}
    for(const auto& [key,p]:proofs) {
        if(p.conflict)continue;
        try {
            if(!p.psu.empty()&&!p.usb.empty())resolve(p.agent,p.session,p.usb,p.psu);
            else if(!p.psu.empty()&&!data_["resolved"].contains(key)&&data_["resolved"].size()<kMaxAliases)data_["resolved"][key]={{"usb",""},{"psu",p.psu}};
        }catch(const std::exception& e){warn(std::string("历史设备归并不完整：")+e.what());}
    }
    // A newer PSU snapshot may already contain an older provisional USB session.
    // resolve() removes the old contribution, adding it only if target checkpoint lacks it.
    std::vector<std::pair<std::string,Json>> existing;for(auto it=data_["resolved"].begin();it!=data_["resolved"].end();++it)existing.emplace_back(it.key(),it.value());
    for(const auto& [key,value]:existing)if(data_["unresolved"].contains(key)) {
        auto known=proofs.find(key);
        if(known!=proofs.end()&&!known->second.conflict&&!known->second.usb.empty())try {resolve(known->second.agent,known->second.session,known->second.usb,value.value("psu",std::string{}));}catch(const std::exception& e){warn(e.what());}
    }
    // Compare against the STARTUP checkpoint, not the growing replay watermark.
    // An exact per-stream sequence set permits clock-backward files in any order.
    std::map<std::string,std::uint64_t> baseline;std::map<std::string,std::set<std::uint64_t>> seen;std::size_t replayed=0;
    for(auto s=data_["scopes"].begin();s!=data_["scopes"].end();++s)for(auto m=s.value()["marks"].begin();m!=s.value()["marks"].end();++m)baseline[s.key()+"|"+m.key()]=counter(m.value());
    for(const auto& file:files)try {
        read_csv(file,[&](const auto& h,const auto& f) {
            auto type=field(h,f,"record_type");if(!type.empty()&&type!="telemetry")return;
            if(f[4]=="unselected")return;
            auto in=input_from(h,f);in.scope=canonical(in);auto key=in.scope+"|"+stream_key(in);
            if(in.sequence<=baseline[key])return;
            if(replayed>=200000)throw std::runtime_error("本次恢复新增记录超过200000条");
            if(!seen[key].insert(in.sequence).second)return;
            ++replayed;apply_unchecked(in);
        },[&](const std::string& message){warn(message);});
    }catch(const std::exception& e){warn(std::string("历史采样恢复不完整：")+e.what());}
    cap();
}
} // namespace dm


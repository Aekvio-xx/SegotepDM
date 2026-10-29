#include "agent/storage.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <cstdlib>
#include <cstdio>
namespace dm {
static std::string csv(std::string_view value) {
    if(value.find_first_of(",\"\r\n")==std::string_view::npos) return std::string(value);
    std::string out="\"";
    for(char c:value) { if(c=='"') out+='"'; out+=c; }
    return out+'"';
}
static std::string number(double value) {
    std::ostringstream out; out.imbue(std::locale::classic());
    out<<std::fixed<<std::setprecision(8)<<value;
    auto s=out.str();
    while(!s.empty() && s.back()=='0') s.pop_back();
    if(!s.empty() && s.back()=='.') s.pop_back();
    return s;
}
std::string csv_header() {
    std::string h="schema_version,agent_instance_id,record_sequence,device_session_id,device_id,metadata_id,minute_start_local,period_start_utc,period_end_utc,last_received_utc,connection_state,rx_profile_validation,sample_count,rejected_candidates,partial,gap_reason";
    for(auto& d:kMetrics) for(auto suffix:{"_mean","_min","_max","_last","_valid_ms"}) h+=","+std::string(d.id)+suffix;
    return h+",fault_code_last_u16,fault_nonzero_seen,invalid_fields,stale_fields,dropped_rows_total,record_type,energy_scope_id,energy_period,energy_kwh,energy_valid_ms,energy_has_data,energy_finalized,energy_quality,energy_updated_utc,energy_revision,energy_today_kwh,energy_month_kwh,energy_year_kwh,energy_snapshot_id,energy_snapshot_rows,energy_checkpoint\r\n";
}
static void seek(HANDLE file,std::int64_t offset,DWORD origin) {
    LARGE_INTEGER p{};p.QuadPart=offset;if(!SetFilePointerEx(file,p,nullptr,origin))throw std::runtime_error(error_text());
}
static std::string read_part(HANDLE file,std::int64_t offset,std::size_t size) {
    seek(file,offset,FILE_BEGIN);std::string s(size,'\0');DWORD done=0;
    if(size&&(!ReadFile(file,s.data(),static_cast<DWORD>(size),&done,nullptr)||done!=size))throw std::runtime_error(error_text());return s;
}
static void append_csv(const fs::path& root,const std::string& date,const std::vector<std::vector<std::string>>& records) {
    if(records.empty())return;
    fs::create_directories(root/L"data");auto path=root/L"data"/wide("telemetry_"+date+"_v2.csv");
    Handle file(CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(!file)throw std::runtime_error(error_text());
    LARGE_INTEGER size{};if(!GetFileSizeEx(file.get(),&size))throw std::runtime_error(error_text());
    const std::string header="\xef\xbb\xbf"+csv_header();
    if(size.QuadPart&&size.QuadPart<static_cast<std::int64_t>(header.size())) {
        if(!header.starts_with(read_part(file.get(),0,static_cast<std::size_t>(size.QuadPart))))throw std::runtime_error("CSV 表头或版本不一致");
        seek(file.get(),0,FILE_BEGIN);if(!SetEndOfFile(file.get()))throw std::runtime_error(error_text());size.QuadPart=0;
    }
    if(size.QuadPart&&read_part(file.get(),0,header.size())!=header)throw std::runtime_error("CSV 表头或版本不一致，停止追加");
    constexpr std::int64_t tail_limit=8*1024*1024;
    auto tail=read_part(file.get(),std::max<std::int64_t>(0,size.QuadPart-tail_limit),static_cast<std::size_t>(std::min(size.QuadPart,tail_limit)));
    if(!tail.empty()&&tail.back()!='\n') {
        auto newline=tail.find_last_of('\n');if(newline==std::string::npos)throw std::runtime_error("CSV 尾部无法安全恢复");
        size.QuadPart-=static_cast<std::int64_t>(tail.size()-newline-1);seek(file.get(),size.QuadPart,FILE_BEGIN);
        if(!SetEndOfFile(file.get()))throw std::runtime_error(error_text());tail.resize(newline+1);
    }
    std::string payload=size.QuadPart?std::string{}:header;std::size_t incoming=0;
    for(const auto& f:records) {
        if(f.size()!=92)throw std::runtime_error("内部 CSV v2 列数错误");
        auto line=energy_csv_record(f);incoming+=line.size();
        if(incoming>4*1024*1024)throw std::runtime_error("CSV 单批超过4 MiB，停止追加");
        const auto id=f[0]+","+f[1]+","+f[2]+",";
        if(!tail.starts_with(id)&&tail.find("\n"+id)==std::string::npos)payload+=line;
    }
    seek(file.get(),0,FILE_END);DWORD written=0;
    if(!payload.empty()&&(!WriteFile(file.get(),payload.data(),static_cast<DWORD>(payload.size()),&written,nullptr)||written!=payload.size()))throw std::runtime_error(error_text());
    if(!FlushFileBuffers(file.get()))throw std::runtime_error(error_text());
}
static void trim_log_capacity(const fs::path& root,const fs::path& current,std::uintmax_t incoming) {
    const std::regex pattern("^agent_[0-9]{4}-[0-9]{2}-[0-9]{2}(?:_[0-9]{3})?\\.log$");
    std::vector<std::pair<fs::path,std::uintmax_t>> files;
    std::uintmax_t total=incoming;
    for(auto& e:fs::directory_iterator(root/L"logs")) {
        if(!e.is_regular_file() || e.is_symlink())continue;
        if(!std::regex_match(utf8(e.path().filename().wstring()),pattern))continue;
        auto bytes=e.file_size();total+=bytes;files.push_back({e.path(),bytes});
    }
    std::sort(files.begin(),files.end(),[](auto& a,auto& b){return a.first.filename()<b.first.filename();});
    for(auto& [path,bytes]:files) {
        if(total<=100ULL*1024*1024)break;
        if(path==current)continue;
        fs::remove(path);total-=bytes;
    }
    if(total>100ULL*1024*1024)throw std::runtime_error("日志总量达到上限，无法清理已关闭分卷");
}

static void append_logs(const fs::path& root,const std::string& date,const std::deque<std::string>& lines) {
    if(lines.empty()) return;
    fs::create_directories(root/L"logs");
    const std::regex pattern("^agent_"+date+"(?:_([0-9]{3}))?\\.log$");
    int part=0;
    for(auto& entry:fs::directory_iterator(root/L"logs")) {
        std::smatch match; auto name=utf8(entry.path().filename().wstring());
        if(std::regex_match(name,match,pattern) && match[1].matched) part=std::max(part,std::stoi(match[1].str()));
    }
    auto path_for=[&](int n) {
        char suffix[16]{}; if(n) sprintf_s(suffix,"_%03d",n);
        return root/L"logs"/wide("agent_"+date+suffix+".log");
    };
    auto path=path_for(part);
    std::string text; for(auto& line:lines) text+=line;
    if(fs::exists(path) && fs::file_size(path)+text.size()>5*1024*1024) {
        if(part>=999) throw std::runtime_error("当天日志分卷已达上限");
        path=path_for(++part);
    }
    trim_log_capacity(root,path,text.size());
    Handle file(CreateFileW(path.c_str(),FILE_APPEND_DATA,FILE_SHARE_READ,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(!file) throw std::runtime_error(error_text());
    DWORD written=0;
    if(!WriteFile(file.get(),text.data(),static_cast<DWORD>(text.size()),&written,nullptr) || written!=text.size() || !FlushFileBuffers(file.get()))
        throw std::runtime_error(error_text());
}
static void retention(const fs::path& root,int data_days,const std::set<std::string>& pending_dates,const Clock& now) {
    const auto data_cutoff=Clock::at(now.wall_ms-static_cast<std::int64_t>(data_days)*86400000,now.tick_ms).date;
    const auto log_cutoff=Clock::at(now.wall_ms-30LL*86400000,now.tick_ms).date;
    std::vector<std::pair<fs::path,std::uintmax_t>> logs;
    std::uintmax_t total=0;
    for(auto kind:{L"data",L"logs"}) {
        auto folder=root/kind; if(!fs::exists(folder)) continue;
        bool is_data=std::wstring_view(kind)==L"data";
        std::regex pattern(is_data?"^telemetry_([0-9]{4}-[0-9]{2}-[0-9]{2})(?:_v2)?\\.csv$":"^agent_([0-9]{4}-[0-9]{2}-[0-9]{2})(?:_[0-9]{3})?\\.log$");
        for(auto& e:fs::directory_iterator(folder)) {
            if(!e.is_regular_file() || e.is_symlink()) continue;
            auto name=utf8(e.path().filename().wstring()); std::smatch m;
            if(!std::regex_match(name,m,pattern)) continue;
            auto date=m[1].str();
            bool expired=is_data?(data_days && date<data_cutoff):date<log_cutoff;
            if(date==now.date || pending_dates.contains(date)) expired=false;
            if(expired) { fs::remove(e.path()); continue; }
            if(!is_data) { auto bytes=e.file_size(); logs.push_back({e.path(),bytes}); total+=bytes; }
        }
    }
    std::sort(logs.begin(),logs.end(),[](auto& a,auto& b){return a.first.filename()<b.first.filename();});
    for(auto& [path,bytes]:logs) {
        if(total<=100ULL*1024*1024) break;
        if(path==logs.back().first) continue;
        fs::remove(path); total-=bytes;
    }
}
Storage::Storage(fs::path root):root_(std::move(root)) {thread_=std::thread([this]{run();});}
Storage::~Storage() {
    {std::lock_guard lock(mutex_); stopping_=true; force_=true;}
    changed_.notify_one(); if(thread_.joinable()) thread_.join();
}
void Storage::configure(const Config& c) { std::lock_guard lock(mutex_); config_=c;energy_selected_usb_=c.device_instance.empty()?std::string{}:"usb:"+sha256(c.device_instance); }
void Storage::enqueue(CsvRow row) {
    std::lock_guard lock(mutex_);++energy_queue_revision_;
    if(row.text.size()>256*1024) {status_["dropped_rows"]=status_["dropped_rows"].get<std::uint64_t>()+1;return;}
    while(rows_.size()+inflight_count_>=30 || queued_bytes_+inflight_bytes_+row.text.size()>256*1024) {
        status_["dropped_rows"]=status_["dropped_rows"].get<std::uint64_t>()+1;
        // A failed append may already have written complete prefix rows. Keep every
        // retry row until the entire batch is confirmed, so no later watermark can
        // skip a durable prefix whose energy was never committed.
        auto discard=std::find_if(rows_.begin(),rows_.end(),[](const CsvRow& pending){return !pending.retry_protected;});
        if(discard==rows_.end())return;
        queued_bytes_-=discard->text.size();rows_.erase(discard);
    }
    queued_bytes_+=row.text.size(); rows_.push_back(std::move(row));
}
void Storage::metadata(std::string id,Json value) {
    std::lock_guard lock(mutex_);
    if(metadata_.size()>=64 && !metadata_.contains(id)) {status_["metadata_error"]="设备信息待写队列已满";return;}
    metadata_[std::move(id)]=std::move(value);
}
void Storage::log(std::string key,std::string message) {
    const auto now=Clock::now();
    message.resize(std::min<std::size_t>(message.size(),1024));
    std::lock_guard lock(mutex_);
    if(!limits_.contains(key) && limits_.size()>=32) limits_.erase(limits_.begin());
    auto& limit=limits_[key];
    if(limit.tick && limit.message==message && now.tick_ms-limit.tick<300000) {++limit.suppressed;return;}
    const auto suppressed=limit.suppressed;
    limit={now.tick_ms,0,message};
    if(log_minute_!=now.tick_ms/60000) {log_minute_=now.tick_ms/60000;log_count_=0;}
    if(log_count_++>=20) {status_["dropped_logs"]=status_["dropped_logs"].get<std::uint64_t>()+1;return;}
    auto line=now.local+" DM-Agent ["+key+"] "+message;
    if(suppressed) line+=" (合并重复 "+std::to_string(suppressed)+" 次)";
    line+="\r\n";
    while(log_bytes_+line.size()>65536 && !logs_.empty()) {
        log_bytes_-=logs_.front().size(); logs_.pop_front();
        status_["dropped_logs"]=status_["dropped_logs"].get<std::uint64_t>()+1;
    }
    log_bytes_+=line.size(); logs_.push_back(std::move(line));
}
void Storage::flush() { {std::lock_guard lock(mutex_);force_=true;} changed_.notify_one(); }
Json Storage::status() {
    std::lock_guard lock(mutex_); auto j=status_;
    j["pending_rows"]=rows_.size()+inflight_count_; j["pending_bytes"]=queued_bytes_+inflight_bytes_; return j;
}

void Storage::resolve_energy_session(std::string agent,std::string session,std::string usb,std::string psu) {
    std::lock_guard lock(mutex_);
    if(energy_resolutions_.size()>=64){status_["energy_error"]="电量设备身份队列已满";return;}
    energy_resolutions_.push_back({std::move(agent),std::move(session),std::move(usb),std::move(psu)});
}
Json Storage::energy_snapshot(std::string scope,const Clock& now,const std::string& state) {
    std::lock_guard lock(mutex_);
    if(state!="connected"&&state!="stale")scope=energy_selected_usb_;
    else if(scope.empty()||scope=="unselected")scope=energy_selected_usb_;
    if(state!="connected"){auto known=energy_verified_.find(scope);if(known!=energy_verified_.end())scope=known->second;}
    auto it=energy_views_.find(scope);
    Json result=it==energy_views_.end()?Json{{"version",1},{"basis","minute_mean"},{"scope_id",scope},{"date_local",now.date},{"as_of_utc",""},{"revision",0ULL},{"today_kwh",nullptr},{"month_kwh",nullptr},{"year_kwh",nullptr},{"today_valid_ms",0ULL},{"history_quality","estimated_partial"},{"recovery_error",""},{"days",Json::array()},{"months",Json::array()}}:it->second;
    if(result.value("date_local",std::string{})!=now.date){result["date_local"]=now.date;result["today_kwh"]=nullptr;result["month_kwh"]=nullptr;result["year_kwh"]=nullptr;result["today_valid_ms"]=0;result["days"]=Json::array();result["months"]=Json::array();}
    auto error=status_.value("energy_error",std::string{});
    if(!error.empty()){result["recovery_error"]=error;result["history_quality"]="recovery_incomplete";}
    result["status"]=!energy_ready_?"recovering":!error.empty()?"error":!result["today_kwh"].is_number()?"missing":state=="connected"?"accumulating":"paused";
    return result;
}

void Storage::run() {
    std::uint64_t next_csv=0,next_log=0,next_meta=0,next_retention=0,next_energy_seed=0;
    Json cache;bool loaded=false;
    EnergyBook committed;bool recovered=false,needs_seed=true,ledger_saved=false;
    std::uint64_t view_queue=~0ULL,view_revision=~0ULL,view_dropped=~0ULL,known_dropped=0,past_seen=~0ULL;
    std::string view_date,view_error;
    std::string seeded_date;std::map<std::string,Clock> seed_times;
    auto encode=[&](const CsvRow& row,const EnergyBook& book) {
        auto fields=row.fields;if(fields.size()!=75)throw std::runtime_error("内部 CSV 基础列数错误");
        fields.push_back(std::to_string(row.dropped_total));
        auto extra=book.columns(row.energy);fields.insert(fields.end(),extra.begin(),extra.end());return fields;
    };
    for(;;) {
        bool stop,force;Config c;std::deque<std::array<std::string,4>> resolutions;
        {
            std::unique_lock lock(mutex_);
            changed_.wait_for(lock,std::chrono::seconds(1),[&]{return stopping_||force_;});
            stop=stopping_;force=force_;force_=false;c=config_;resolutions.swap(energy_resolutions_);
        }
        const auto now=Clock::now();
        auto set_error=[&](const char* key,std::string text) {
            text.resize(std::min<std::size_t>(text.size(),768));bool changed=false;
            {std::lock_guard lock(mutex_);changed=status_.value(key,std::string{})!=text;status_[key]=text;}
            if(changed&&std::string_view(key)!="log_error")try {log(std::string("storage_")+key,text.empty()?"记录状态已恢复":text);}catch(...){}
        };
        if(!recovered) {
            try {committed.recover(root_);set_error("energy_error",committed.error());}
            catch(const std::exception& e){committed.warn(e.what());set_error("energy_error",e.what());}
            recovered=true;
        }
        for(const auto& r:resolutions)try {
            auto before=committed.revision();committed.resolve(r[0],r[1],r[2],r[3]);if(before!=committed.revision()){needs_seed=true;next_energy_seed=0;}
        }catch(const std::exception& e){committed.warn(e.what());set_error("energy_error",e.what());}
        if(force || now.tick_ms>=next_meta) {
            std::map<std::string,Json> pending;
            {std::lock_guard lock(mutex_);pending.swap(metadata_);}
            try {
                if(!pending.empty()) {
                    if(!storage_space_ok(root_)) throw std::runtime_error("可用空间不足 100 MiB");
                    if(!loaded) {
                        auto path=root_/L"config"/L"devices.json";
                        cache=fs::exists(path)?Json::parse(read_file(path,8*1024*1024)):Json{{"schema_version",1},{"versions",Json::object()}};
                        if(cache.value("schema_version",0)!=1 || !cache["versions"].is_object()) throw std::runtime_error("设备信息缓存格式错误");
                        loaded=true;
                    }
                    bool modified=false;
                    for(auto& [id,value]:pending) if(!cache["versions"].contains(id)) {cache["versions"][id]=value;modified=true;}
                    if(modified) {
                        auto serialized=cache.dump(2,' ',false,Json::error_handler_t::replace);
                        if(serialized.size()>8*1024*1024) {loaded=false;throw std::runtime_error("设备信息缓存超过 8 MiB 上限");}
                        atomic_write(root_/L"config"/L"devices.json",serialized+"\n");
                    }
                    set_error("metadata_error","");
                }
                next_meta=now.tick_ms+1000;
            } catch(const std::exception& e) {
                loaded=false; set_error("metadata_error",e.what()); next_meta=now.tick_ms+60000;
                std::lock_guard lock(mutex_);
                for(auto& [id,value]:pending) if(metadata_.size()<64) metadata_.try_emplace(id,std::move(value));
            }
        }

        {std::lock_guard lock(mutex_);auto dropped=status_["dropped_rows"].get<std::uint64_t>();if(dropped!=known_dropped){known_dropped=dropped;committed.warn("部分CSV记录因有界队列丢弃，电量记录不完整");needs_seed=true;next_energy_seed=0;}}
        bool past_pending=false;{std::lock_guard lock(mutex_);if(energy_queue_revision_!=past_seen)for(const auto& row:rows_)if(row.date!=now.date){past_pending=true;past_seen=energy_queue_revision_;break;}}
        if(force||past_pending||now.tick_ms>=next_csv) {
            std::deque<CsvRow> batch;
            {std::lock_guard lock(mutex_);++energy_queue_revision_;batch.swap(rows_);inflight_count_=batch.size();inflight_bytes_=queued_bytes_;queued_bytes_=0;
                for(auto& row:batch)row.dropped_total=status_["dropped_rows"].get<std::uint64_t>();}
            try {
                if(!batch.empty()) {
                    if(!storage_space_ok(root_))throw std::runtime_error("可用空间不足 100 MiB");
                    if(!loaded)throw std::runtime_error("设备信息尚未保存");
                    for(const auto& row:batch)if(!row.metadata_id.empty()&&!cache["versions"].contains(row.metadata_id))throw std::runtime_error("CSV 引用的设备信息尚未保存");
                    while(!batch.empty()) {
                        auto candidate=committed;const auto date=batch.front().date;std::vector<std::vector<std::string>> group;std::size_t count=0;
                        for(const auto& row:batch){if(row.date!=date)break;candidate.apply(row.energy);group.push_back(encode(row,candidate));++count;}
                        append_csv(root_,date,group);
                        if(candidate.scopes()!=committed.scopes()){needs_seed=true;next_energy_seed=0;}
                        committed=std::move(candidate);
                        // Commit per successful date group; a failed later group cannot replay this one twice.
                        for(std::size_t n=0;n<count;++n)batch.pop_front();
                        if(date!=now.date){needs_seed=true;next_energy_seed=0;}
                    }
                    set_error("csv_error","");
                }
                next_csv=now.tick_ms+static_cast<std::uint64_t>(c.csv_flush_seconds)*1000;
            }catch(const std::exception& e) {
                set_error("csv_error",e.what());next_csv=now.tick_ms+60000;
                std::lock_guard lock(mutex_);++energy_queue_revision_;past_seen=energy_queue_revision_;
                while(!batch.empty()){batch.back().retry_protected=true;queued_bytes_+=batch.back().text.size();rows_.push_front(std::move(batch.back()));batch.pop_back();}
            }
            {std::lock_guard lock(mutex_);inflight_count_=inflight_bytes_=0;}
            try {if(!storage_space_ok(root_))throw std::runtime_error("可用空间不足100 MiB");if(!needs_seed){committed.save(root_);ledger_saved=true;set_error("energy_error","");}}
            catch(const std::exception& e){ledger_saved=false;set_error("energy_error",e.what());next_csv=std::min(next_csv,now.tick_ms+60000ULL);}
        }
        // Close old-date pending records before seeding the new file. If the controller
        // closes one just after this pass, its later successful commit requests a revision.
        if((needs_seed||seeded_date!=now.date)&&now.tick_ms>=next_energy_seed)try {
            if(!storage_space_ok(root_))throw std::runtime_error("可用空间不足100 MiB");
            auto candidate=committed;auto key=now.date+"|"+std::to_string(candidate.revision());
            if(seed_times.size()>64)seed_times.clear();
            auto [stamp,_]=seed_times.try_emplace(key,now);
            auto rows=candidate.summaries(stamp->second);
            if(!rows.empty())append_csv(root_,now.date,rows);
            committed=std::move(candidate);seeded_date=now.date;needs_seed=false;next_energy_seed=now.tick_ms+60000;
            committed.save(root_);ledger_saved=true;set_error("energy_error","");
        }catch(const std::exception& e){ledger_saved=false;set_error("energy_error",e.what());next_energy_seed=now.tick_ms+60000;next_csv=std::min(next_csv,now.tick_ms+60000ULL);}
        // Rebuild only when a closed segment, committed checkpoint, date, or quality
        // changes. Heartbeat reads use the cached projection without energy arithmetic.
        try {
            std::uint64_t generation=0,dropped=0;bool dirty=false;std::deque<CsvRow> pending;
            {std::lock_guard lock(mutex_);generation=energy_queue_revision_;dropped=status_["dropped_rows"].get<std::uint64_t>();
                dirty=generation!=view_queue||committed.revision()!=view_revision||now.date!=view_date||dropped!=view_dropped||committed.error()!=view_error;
                if(dirty)pending=rows_;}
            if(dirty) {
                auto live=committed;for(const auto& row:pending)live.apply(row.energy);
                std::map<std::string,Json> views;auto verified=live.verified();
                for(const auto& scope:live.scopes())views[scope]=live.view(scope,now);
                {std::lock_guard lock(mutex_);energy_views_=std::move(views);energy_verified_=std::move(verified);energy_ready_=true;}
                view_queue=generation;view_revision=committed.revision();view_date=now.date;view_dropped=dropped;view_error=committed.error();
            }
        }catch(const std::exception& e){set_error("energy_error",e.what());}
        if(force || now.tick_ms>=next_log) {
            std::deque<std::string> lines;
            {std::lock_guard lock(mutex_);lines.swap(logs_);log_bytes_=0;}
            try {
                if(!lines.empty()) {
                    if(!storage_space_ok(root_)) throw std::runtime_error("可用空间不足 100 MiB");
                    append_logs(root_,now.date,lines);set_error("log_error","");
                }
            } catch(const std::exception& e) {
                set_error("log_error",e.what());
                std::lock_guard lock(mutex_);
                for(auto it=lines.rbegin();it!=lines.rend();++it) if(log_bytes_+it->size()<=65536) {log_bytes_+=it->size();logs_.push_front(*it);}
            }
            next_log=now.tick_ms+60000;
        }
        if(now.tick_ms>=next_retention) {
            try {
                std::set<std::string> pending;
                {std::lock_guard lock(mutex_);for(auto& row:rows_) pending.insert(row.date);}
                if(!ledger_saved)throw std::runtime_error("电量账本尚未持久保存，暂缓历史清理");
                retention(root_,c.data_retention_days,pending,now);set_error("retention_error","");
            } catch(const std::exception& e) {set_error("retention_error",e.what());}
            next_retention=now.tick_ms+6*60*60*1000ULL;
        }

        if(stop)break;
    }
}
MinuteAccumulator::MinuteAccumulator(Storage& storage,RecordContext context,const Clock& now)
    :storage_(storage),context_(std::move(context)) {reset_bucket(now);}
void MinuteAccumulator::reset_bucket(const Clock& now) {
    start_=last_=now;
    end_tick_=now.tick_ms+static_cast<std::uint64_t>(now.minute_start_wall_ms+60000-now.wall_ms);
    stats_={};samples_=invalid_=0;fault_.reset();nonzero_=false;reason_.clear();
}
void MinuteAccumulator::integrate(std::uint64_t from,std::uint64_t until) {
    if(!active_) return;
    const auto end=std::min(until,active_->received.tick_ms+static_cast<std::uint64_t>(stale_ms_));
    from=std::max(from,active_->received.tick_ms);
    if(end<=from) return;
    for(std::size_t i=0;i<stats_.size();++i) {
        auto& stat=stats_[i]; const auto value=active_->values[i];
        if(!stat.seen) {stat.minimum=stat.maximum=value;stat.seen=true;}
        stat.minimum=std::min(stat.minimum,value);stat.maximum=std::max(stat.maximum,value);
        stat.last=value;stat.integral+=value*static_cast<double>(end-from);stat.valid_ms+=end-from;
    }
    fault_=active_->fault;nonzero_=nonzero_||active_->fault!=0;
}
void MinuteAccumulator::emit(const Clock& end,std::string reason) {
    if(end.tick_ms<=start_.tick_ms && !samples_) return;
    const auto id="2,"+context_.agent_id+","+std::to_string(++sequence_)+",";
    std::vector<std::string> fields={"2",context_.agent_id,std::to_string(sequence_),context_.session_id,context_.device_id,context_.metadata_id,
        start_.minute_local,start_.utc,end.utc,active_?active_->received.utc:"",context_.connection_state,kRxValidation,
        std::to_string(samples_),std::to_string(invalid_)};
    bool partial=start_.wall_ms!=start_.minute_start_wall_ms || end.wall_ms!=start_.minute_start_wall_ms+60000;
    for(auto& stat:stats_) partial=partial||stat.valid_ms<60000;
    fields.push_back(partial?"true":"false");fields.push_back(reason.empty()?reason_:reason);
    std::string invalid,stale;
    for(std::size_t i=0;i<stats_.size();++i) {
        auto& stat=stats_[i];
        fields.push_back(stat.valid_ms?number(stat.integral/stat.valid_ms):"");
        fields.push_back(stat.seen?number(stat.minimum):"");fields.push_back(stat.seen?number(stat.maximum):"");
        fields.push_back(stat.seen?number(stat.last):"");fields.push_back(std::to_string(stat.valid_ms));
        if(!stat.seen) {if(!invalid.empty())invalid+=';';invalid+=kMetrics[i].id;}
        if(!active_ || end.tick_ms-active_->received.tick_ms>=static_cast<unsigned>(stale_ms_)) {if(!stale.empty())stale+=';';stale+=kMetrics[i].id;}
    }
    fields.push_back(fault_?std::to_string(*fault_):"");fields.push_back(fault_?(nonzero_?"true":"false"):"");
    fields.push_back(invalid);fields.push_back(stale);
    std::string line;
    for(auto& field:fields) {if(!line.empty())line+=',';line+=csv(field);}
    CsvRow row;row.date=start_.date;row.identity=id;row.text=line+"\r\n";row.metadata_id=context_.metadata_id;row.fields=std::move(fields);
    row.energy.agent=context_.agent_id;row.energy.session=context_.session_id;row.energy.scope=context_.device_id;row.energy.date=start_.date;row.energy.updated=end.utc;
    row.energy.sequence=sequence_;row.energy.valid_ms=stats_[0].valid_ms;row.energy.has_data=stats_[0].valid_ms>0;
    row.energy.mean_w=row.energy.has_data?stats_[0].integral/static_cast<double>(stats_[0].valid_ms):0;
    row.energy.delta_kwh=row.energy.has_data?row.energy.mean_w*static_cast<double>(row.energy.valid_ms)/3600000000.0:0;row.energy.reason=reason.empty()?reason_:reason;
    storage_.enqueue(std::move(row));
}
void MinuteAccumulator::advance(const Clock& now) {
    const auto elapsed=now.tick_ms-last_.tick_ms;
    const auto wall_delta=now.wall_ms-last_.wall_ms;
    if(elapsed>60000 || std::llabs(wall_delta-static_cast<std::int64_t>(elapsed))>2000 || now.offset_minutes!=last_.offset_minutes) {
        emit(last_,elapsed>60000?"execution_gap":"clock_changed");active_.reset();reset_bucket(now);reason_="gap_before_segment";return;
    }
    while(last_.tick_ms<now.tick_ms) {
        auto end=std::min(now.tick_ms,end_tick_);
        integrate(last_.tick_ms,end);
        auto point=Clock::at(last_.wall_ms+static_cast<std::int64_t>(end-last_.tick_ms),end);
        last_=point;
        if(end==end_tick_) {auto previous_date=start_.date;emit(point,"");reset_bucket(point);if(previous_date!=point.date)storage_.flush();}
    }
}
void MinuteAccumulator::sample(const Sample& s) {
    advance(s.received);active_=s;++samples_;
    for(std::size_t i=0;i<stats_.size();++i) {
        auto& stat=stats_[i];if(!stat.seen) {stat.minimum=stat.maximum=s.values[i];stat.seen=true;}
        stat.minimum=std::min(stat.minimum,s.values[i]);stat.maximum=std::max(stat.maximum,s.values[i]);stat.last=s.values[i];
    }
    fault_=s.fault;nonzero_=nonzero_||s.fault!=0;
}
void MinuteAccumulator::change_context(const Clock& now,RecordContext context,std::string reason,bool carry) {
    advance(now);emit(now,reason);auto previous=active_;reset_bucket(now);
    active_=carry?previous:std::nullopt;context_=std::move(context);reason_=std::move(reason);
}
void MinuteAccumulator::finish(const Clock& now,std::string reason) {advance(now);emit(now,std::move(reason));active_.reset();reset_bucket(now);}
} // namespace dm


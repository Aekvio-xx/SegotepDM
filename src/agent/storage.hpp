#pragma once
#include "common/model.hpp"
#include "agent/energy.hpp"
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
namespace dm {
struct RecordContext {
    std::string agent_id, session_id, device_id, metadata_id, connection_state="disabled";
};
struct CsvRow { std::string date, identity, text, metadata_id; std::uint64_t dropped_total=0; std::vector<std::string> fields; EnergyInput energy; bool retry_protected=false; };
std::string csv_header();
class Storage {
    fs::path root_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::thread thread_;
    bool stopping_=false, force_=false;
    Config config_;
    std::deque<CsvRow> rows_;
    std::size_t queued_bytes_=0,inflight_count_=0,inflight_bytes_=0;
    std::map<std::string,Json> metadata_;
    std::deque<std::string> logs_;
    std::size_t log_bytes_=0;
    struct Limit { std::uint64_t tick=0, suppressed=0; std::string message; };
    std::map<std::string,Limit> limits_;
    std::uint64_t log_minute_=0;
    unsigned log_count_=0;
    Json status_={{"csv_error",""},{"metadata_error",""},{"log_error",""},{"retention_error",""},{"dropped_rows",0},{"dropped_logs",0}};
    std::deque<std::array<std::string,4>> energy_resolutions_;
    std::map<std::string,Json> energy_views_;
    std::map<std::string,std::string> energy_verified_;
    std::string energy_selected_usb_;
    bool energy_ready_=false;
    std::uint64_t energy_queue_revision_=0;
    void run();
public:
    explicit Storage(fs::path root);
    ~Storage();
    void configure(const Config& c);
    void enqueue(CsvRow row);
    void metadata(std::string id,Json value);
    void log(std::string key,std::string message);
    void flush();
    Json status();
    void resolve_energy_session(std::string agent,std::string session,std::string usb,std::string psu);
    Json energy_snapshot(std::string scope,const Clock& now,const std::string& state);
};
class MinuteAccumulator {
    struct Stat {
        double integral=0,minimum=0,maximum=0,last=0;
        std::uint64_t valid_ms=0;
        bool seen=false;
    };
    Storage& storage_;
    RecordContext context_;
    Clock start_, last_;
    std::uint64_t end_tick_=0, sequence_=0, samples_=0, invalid_=0;
    std::array<Stat,11> stats_{};
    std::optional<Sample> active_;
    std::optional<std::uint16_t> fault_;
    bool nonzero_=false;
    int stale_ms_=10000;
    std::string reason_;
    void reset_bucket(const Clock& at);
    void integrate(std::uint64_t from,std::uint64_t until);
    void emit(const Clock& end,std::string reason);
public:
    MinuteAccumulator(Storage& storage,RecordContext context,const Clock& now);
    void advance(const Clock& now);
    void sample(const Sample& sample);
    void change_context(const Clock& now,RecordContext context,std::string reason,bool carry=false);
    void state(std::string state) { context_.connection_state=std::move(state); }
    void stale_after(int ms) { stale_ms_=ms; }
    void bad_frames(std::uint64_t count) { invalid_+=count; }
    void finish(const Clock& now,std::string reason);
};
} // namespace dm

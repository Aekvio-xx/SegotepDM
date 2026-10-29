#include "agent/controller.hpp"
#include <algorithm>
#include <limits>
namespace dm {
Controller::Controller(fs::path root,Config config)
    :root_(std::move(root)),wake_(CreateEventW(nullptr,TRUE,FALSE,nullptr)),cancel_(CreateEventW(nullptr,TRUE,FALSE,nullptr)),desired_(config),storage_(root_),instance_(guid()) {
    if(!wake_ || !cancel_) throw std::runtime_error(error_text());
    storage_.configure(config);
    worker_=std::thread([this]{run();});
}
Controller::~Controller() {stop();}
void Controller::configure(const Config& c) {
    {
        std::lock_guard lock(command_mutex_);
        if(c.serial_enabled!=desired_.serial_enabled || c.port!=desired_.port || c.device_instance!=desired_.device_instance ||
            c.data_bits!=desired_.data_bits || c.stop_bits!=desired_.stop_bits || c.parity!=desired_.parity || c.dtr!=desired_.dtr || c.rts!=desired_.rts) {
            ++epoch_;interval_.reset();SetEvent(cancel_.get());
        }
        desired_=c;
    }
    storage_.configure(c);SetEvent(wake_.get());
}
bool Controller::request_interval(int ms,std::string& error) {
    auto view=snapshot();
    if(ms<1000 || ms>3000 || ms%500) {error="回传间隔无效";return false;}
    std::lock_guard lock(command_mutex_);
    if(!desired_.serial_enabled || suspended_ || view.value("connection_state",std::string{})!="connected") {
        error="设备当前没有新鲜数据，未发送设置";return false;
    }
    auto now=GetTickCount64();
    if(interval_ || (last_request_ && now-last_request_<1000)) {error="设置请求过于频繁";return false;}
    last_request_=now;
    interval_=IntervalRequest{ms,epoch_,view.value("device_session_id",std::string{})};
    SetEvent(wake_.get());return true;
}
void Controller::devices_changed() {SetEvent(wake_.get());}
void Controller::suspend(bool value) {
    {std::lock_guard lock(command_mutex_);if(suspended_==value)return;suspended_=value;++epoch_;interval_.reset();SetEvent(cancel_.get());}
    SetEvent(wake_.get());
}
void Controller::stop() {stopping_=true;SetEvent(cancel_.get());SetEvent(wake_.get());if(worker_.joinable())worker_.join();}
Json Controller::snapshot() const {std::lock_guard lock(view_mutex_);return view_;}
void Controller::run() {
    try {
        SnapshotWriter publisher;
        SerialPort serial;
        Framer framer;
        RecordContext context{instance_,"","unselected","","disabled"};
        auto now=Clock::now();
        MinuteAccumulator minutes(storage_,context,now);
        std::optional<Sample> sample;
        Json info=Json::object();
        std::string energy_usb,sn,state="disabled",last_error,last_action,actual_port,info_received,sn_received;
        std::uint64_t local_epoch=std::numeric_limits<std::uint64_t>::max(),next_retry=0,opened=0,query_due=0,published=0,snapshot_sequence=0,status_sequence=0;
        unsigned retry=0;
        std::vector<std::uint64_t> intervals;
        Config config;
        auto metadata=[&](const Clock& at,bool carry) {
            Json data={{"device_id",context.device_id},{"serial_number",sn},{"specification",info},
                {"profile",kRxProfile},{"rx_profile_validation",kRxValidation}};
            auto id=sha256(data.dump(-1,' ',false,Json::error_handler_t::replace));
            if(id==context.metadata_id)return;
            data["first_seen_utc"]=at.utc;data["metadata_id"]=id;
            storage_.metadata(id,data);
            context.metadata_id=id;context.connection_state=state;
            minutes.change_context(at,context,"metadata_changed",carry);
        };
        auto permitted=[&] {
            std::lock_guard lock(command_mutex_);
            return !stopping_ && epoch_==local_epoch && desired_.serial_enabled && !suspended_;
        };
        auto transmit=[&](const std::vector<std::uint8_t>& bytes) {
            if(!permitted())throw std::runtime_error("连接启用意图已变化，取消发送");
            serial.send(bytes,cancel_.get());
        };
        auto change_state=[&](std::string next,std::string error=std::string{}) {
            if(state!=next || (!error.empty() && error!=last_error)) storage_.log("connection",next+(error.empty()?"":": "+error));
            state=std::move(next);context.connection_state=state;minutes.state(state);
            if(!error.empty())last_error=std::move(error);
        };
        auto disconnect=[&](const Clock& at,const std::string& next,const std::string& why) {
            serial.close();query_due=0;
            context.connection_state=next;
            minutes.change_context(at,context,why,false);
            change_state(next);
        };
        auto retry_later=[&](const Clock& at) {
            constexpr std::uint64_t delays[]{1000,2000,5000,10000,30000,60000};
            next_retry=at.tick_ms+delays[std::min<unsigned>(retry++,5)];
        };
        metadata(now,false);
        storage_.log("startup","代理启动；使用含帧头 CRC 的 DM850G 接收配置，其他固件兼容性待确认");
        while(!stopping_) {
            now=Clock::now();
            std::uint64_t epoch;bool suspended;
            std::optional<IntervalRequest> request;
            {std::lock_guard lock(command_mutex_);config=desired_;epoch=epoch_;suspended=suspended_;request.swap(interval_);ResetEvent(wake_.get());}
            if(stopping_)break;
            minutes.stale_after(config.stale_after_ms);
            if(epoch!=local_epoch) {
                disconnect(now,suspended?"suspended":config.serial_enabled?"connecting":"disabled",suspended?"suspend":"serial_selection_changed");
                local_epoch=epoch;retry=0;next_retry=now.tick_ms;
                {std::lock_guard lock(command_mutex_);if(epoch_==local_epoch && !stopping_)ResetEvent(cancel_.get());}
                if(suspended)storage_.flush();
            }
            if(!config.serial_enabled || suspended) {
                change_state(suspended?"suspended":"disabled");
            } else if(!serial.is_open() && now.tick_ms>=next_retry) {
                try {
                    change_state("connecting");
                    auto p=resolve_port(config);
                    if(!p) throw std::runtime_error("选定设备未找到或标识不唯一，请检查端口选择");
                    if(!permitted())continue;
                    serial.open(*p,config);
                    now=Clock::now();
                    // 打开完成后再次检查启用意图，防止关闭操作遇到迟到的打开结果。
                    {std::lock_guard lock(command_mutex_);if(epoch_!=local_epoch || !desired_.serial_enabled || suspended_ || stopping_) {serial.close();continue;}}
                    context.session_id=guid();context.device_id="usb:"+sha256(p->instance);energy_usb=context.device_id;
                    context.metadata_id.clear();sample.reset();info=Json::object();sn.clear();info_received.clear();sn_received.clear();intervals.clear();status_sequence=0;
                    framer.reset();actual_port=p->name;opened=now.tick_ms;last_error.clear();change_state("waiting");
                    metadata(now,false);
                    transmit(information_query(0));
                    last_action="已发送信息查询 0x00；初始化顺序待确认";
                    query_due=GetTickCount64()+1000;retry=0;
                } catch(const std::exception& e) {
                    disconnect(Clock::now(),"disconnected","connection_failed");
                    change_state("disconnected",e.what());retry_later(Clock::now());
                }
            }
            if(serial.is_open() && !permitted()) {serial.close();continue;}
            if(serial.is_open()) {
                try {
                    serial.begin_read();
                    if(auto bytes=serial.complete_read();bytes && !bytes->empty()) {
                        minutes.advance(Clock::now());
                        const auto before=framer.rejected;
                        auto frames=framer.feed(*bytes);
                        minutes.bad_frames(framer.rejected-before);
                        if(framer.rejected!=before)storage_.log("rx_rejected","收到字节但存在无效帧：长度、帧尾或含帧头 CRC 不匹配；详见接收诊断");
                        for(auto& frame:frames) {
                            if(!permitted())break;
                            auto received=Clock::now();
                            if(frame.type==2) {
                                auto next=parse_status(frame.payload,received,++status_sequence);
                                if(sample) {
                                    auto interval=received.tick_ms-sample->received.tick_ms;
                                    if(interval>0 && interval<=10000) {
                                        if(intervals.size()>=8)intervals.erase(intervals.begin());intervals.push_back(interval);
                                    }
                                    if(sample->fault!=next.fault)storage_.log("fault","原始故障码变化："+std::to_string(sample->fault)+" -> "+std::to_string(next.fault));
                                } else if(next.fault)storage_.log("fault","收到非零原始故障码："+std::to_string(next.fault));
                                sample=next;change_state("connected");minutes.sample(next);
                            } else {
                                if(frame.type==3) {info=parse_info(frame.payload);info_received=received.utc;}
                                if(frame.type==5) {sn=parse_serial_number(frame.payload);sn_received=received.utc;if(!sn.empty()){context.device_id="psu:"+sha256(sn);storage_.resolve_energy_session(instance_,context.session_id,energy_usb,context.device_id);}}
                                if(info.contains("warranty_raw")) {
                                    std::string warranty=std::to_string(info["warranty_raw"].get<unsigned>())+"年";
                                    if(info.value("firmware_version_raw",255)<12 && sn.size()>=4) {
                                        auto suffix=sn.substr(sn.size()-4);
                                        if(std::all_of(suffix.begin(),suffix.end(),[](char c){return c>='0' && c<='9';}) && std::stoi(suffix)<1001)warranty="终身（HiMOS显示规则）";
                                    }
                                    info["warranty_display"]=warranty;
                                }
                                metadata(received,sample.has_value());
                            }
                        }
                    }
                    now=Clock::now();
                    if(query_due && now.tick_ms>=query_due) {
                        query_due=0;transmit(information_query(0x35));
                        last_action="已发送信息查询 0x35；回包按类型接收";
                    }
                    if(request && request->epoch==local_epoch && request->session==context.session_id) {
                        bool allowed=false;
                        {std::lock_guard lock(command_mutex_);allowed=epoch_==local_epoch && desired_.serial_enabled && !suspended_ && !stopping_;}
                        if(allowed && state=="connected" && sample && now.tick_ms-sample->received.tick_ms<static_cast<unsigned>(config.stale_after_ms)) {
                            try {transmit(interval_command(request->ms));last_action="回传间隔 "+std::to_string(request->ms)+" ms 已发送，设备确认未知";}
                            catch(const std::exception& e){last_action=std::string("回传间隔未确认：")+e.what();}
                            storage_.log("interval",last_action);
                        } else last_action="连接状态已变化，设置请求已丢弃";
                    }
                    auto age=now.tick_ms-(sample?sample->received.tick_ms:opened);
                    if(age>=30000) {
                        disconnect(now,"disconnected","status_timeout");change_state("disconnected","连续 30 秒无可接受状态帧");retry_later(now);
                    } else if(sample && age>=static_cast<unsigned>(config.stale_after_ms))change_state("stale");
                } catch(const std::exception& e) {
                    disconnect(Clock::now(),"disconnected","serial_error");change_state("disconnected",e.what());retry_later(Clock::now());
                }
            }
            now=Clock::now();minutes.advance(now);
            if(now.tick_ms-published>=1000) {
                Json j={{"schema_version",{{"major",1},{"minor",1}}},{"agent_instance_id",instance_},
                    {"device_session_id",context.session_id},{"snapshot_sequence",++snapshot_sequence},{"status_sequence",sample?sample->sequence:0},
                    {"running",true},{"published_at_utc",now.utc},{"published_tick_ms",now.tick_ms},
                    {"serial_enabled",config.serial_enabled},{"connection_state",state},{"actual_port",actual_port},
                    {"selected_port",config.port},{"device_instance",config.device_instance},
                    {"device_id",context.device_id},{"metadata_id",context.metadata_id},{"device_info",info},{"serial_number",sn},
                    {"device_info_received_at_utc",info_received.empty()?Json(nullptr):Json(info_received)},
                    {"serial_number_received_at_utc",sn_received.empty()?Json(nullptr):Json(sn_received)},
                    {"last_error",last_error},{"last_action",last_action},{"last_requested_interval_ms",config.last_requested_interval_ms},
                    {"rx_profile_validation",kRxValidation},{"stale_after_ms",config.stale_after_ms},
                    {"metrics",metric_json(sample,now,serial.is_open(),config.stale_after_ms)},{"storage",storage_.status()},
                    {"energy",storage_.energy_snapshot(context.device_id,now,state)},
                    {"received_at_utc",sample?Json(sample->received.utc):Json(nullptr)},
                    {"received_tick_ms",sample?Json(sample->received.tick_ms):Json(nullptr)},{"device_sample_time",nullptr},
                    {"fault_code_raw",sample?Json(sample->fault):Json(nullptr)},
                    {"fan_mode_raw_diagnostic",sample?Json(sample->mode):Json(nullptr)},
                    {"reserved_status_bytes",sample?Json(sample->reserved):Json(nullptr)},{"diagnostics",framer.diagnostics()}};
                if(!intervals.empty()) {std::uint64_t total=0;for(auto ms:intervals)total+=ms;j["observed_interval_ms"]=total/intervals.size();}
                else j["observed_interval_ms"]=nullptr;
                {std::lock_guard lock(view_mutex_);view_=j;}
                publisher.publish(j);published=now.tick_ms;
            }
            HANDLE events[]{wake_.get(),serial.read_event()};
            WaitForMultipleObjects(serial.is_open()?2:1,events,FALSE,serial.is_open()?250:1000);
        }
        now=Clock::now();
        serial.close();minutes.finish(now,"agent_exit");
        storage_.log("shutdown","代理退出，串口已释放");storage_.flush();
        Json stopped;
        {std::lock_guard lock(view_mutex_);view_["running"]=false;view_["connection_state"]="stopped";view_["energy"]=storage_.energy_snapshot(context.device_id,now,"stopped");view_["published_tick_ms"]=now.tick_ms;stopped=view_;}
        publisher.publish(stopped);
    } catch(const std::exception& e) {
        // 详情面板移除后，致命错误仍可经运行日志追溯；记录失败不掩盖原状态。
        try {storage_.log("fatal",e.what());storage_.flush();} catch(...) {}
        std::lock_guard lock(view_mutex_);
        view_["running"]=false;view_["connection_state"]="fatal_error";view_["last_error"]=e.what();
    }
}
} // namespace dm

#include "agent/protocol.hpp"
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <cstdio>
namespace dm {
static std::uint8_t crc8(std::span<const std::uint8_t> bytes) {
    std::uint8_t c=0;
    for(auto b:bytes) {
        c^=b;
        for(int bit=0;bit<8;++bit) c=static_cast<std::uint8_t>((c<<1)^((c&0x80)?7:0));
    }
    return c;
}
static std::vector<std::uint8_t> command(std::uint8_t type,std::uint8_t a,std::uint8_t b) {
    std::vector<std::uint8_t> bytes{0x55,0x7e,4,type,a,b};
    bytes.push_back(crc8(std::span(bytes).subspan(2)));
    bytes.push_back(0xae); return bytes;
}
std::vector<std::uint8_t> information_query(std::uint8_t option) { return command(4,option,0); }
std::vector<std::uint8_t> interval_command(int milliseconds) {
    if(milliseconds<1000 || milliseconds>3000 || milliseconds%500) throw std::invalid_argument("Unsupported report interval");
    return command(0x16,0,static_cast<std::uint8_t>((milliseconds-1000)/500));
}
static std::string hex(std::span<const std::uint8_t> bytes) {
    std::ostringstream out;
    for(auto b:bytes) out<<std::hex<<std::setw(2)<<std::setfill('0')<<static_cast<unsigned>(b);
    return out.str();
}
std::vector<Frame> Framer::feed(std::span<const std::uint8_t> bytes) {
    // DM850G 回包：L+4；RX CRC 包含 55 7E 帧头。发送方向范围保持不变。
    received_bytes+=bytes.size();
    if(buffer_.size()+bytes.size()>65536) { buffer_.clear(); ++rejected; }
    if(bytes.size()>65536) { bytes=bytes.last(65536); ++rejected; }
    buffer_.insert(buffer_.end(),bytes.begin(),bytes.end());
    std::vector<Frame> frames;
    const std::array<std::uint8_t,2> header{0x55,0x7e};
    std::size_t position=0;
    while(position+2<=buffer_.size()) {
        auto found=std::search(buffer_.begin()+position,buffer_.end(),header.begin(),header.end());
        if(found==buffer_.end()) {
            position=buffer_.size()-(buffer_.back()==0x55?1:0); break;
        }
        position=static_cast<std::size_t>(found-buffer_.begin());
        if(buffer_.size()-position<4) break;
        const std::size_t length=buffer_[position+2]+4u;
        const auto type=buffer_[position+3];
        const std::size_t minimum=type==2?28:type==3?25:type==5?15:0;
        if(length<6 || (minimum && length<minimum+6)) { ++rejected; ++position; continue; }
        if(buffer_.size()-position<length) break;
        auto candidate=std::span<const std::uint8_t>(buffer_).subspan(position,length);
        auto raw_hex=hex(candidate);
        std::size_t diagnostic_bytes=raw_hex.size();for(auto& saved:diagnostic_)diagnostic_bytes+=saved.size();
        while(!diagnostic_.empty() && (diagnostic_.size()>=32 || diagnostic_bytes>16384)) {diagnostic_bytes-=diagnostic_.front().size();diagnostic_.pop_front();}
        diagnostic_.push_back(std::move(raw_hex));
        if(candidate.back()!=0xae) {
            ++tail_failures; ++rejected; ++position; continue;
        }
        // DM850G 捕获的 RX 校验范围是 [帧头, CRC前)，不同于 TX。
        if(crc8(candidate.first(length-2))!=candidate[length-2]) {
            ++crc_failures; ++rejected; ++position; continue;
        }
        ++validated_frames;
        if(type==2)++status_frames;
        if(minimum) frames.push_back({type,{candidate.begin()+4,candidate.end()-2}});
        else ++unknown;
        position+=length;
    }
    buffer_.erase(buffer_.begin(),buffer_.begin()+position);
    return frames;
}
void Framer::reset() { buffer_.clear(); diagnostic_.clear(); rejected=unknown=received_bytes=validated_frames=status_frames=crc_failures=tail_failures=0; }
Json Framer::diagnostics() const {
    return {{"rx_profile_validation",kRxValidation},{"crc_coverage","header_through_payload"},{"received_bytes",received_bytes},
        {"validated_frames",validated_frames},{"status_frames",status_frames},{"crc_failures",crc_failures},{"tail_failures",tail_failures},
        {"rejected_candidates",rejected},{"unknown_types",unknown},
        {"buffered_bytes",buffer_.size()},{"recent_candidate_frames_hex",diagnostic_}};
}
static std::uint16_t u16(std::span<const std::uint8_t> p,std::size_t n) { return static_cast<std::uint16_t>(p[n]|(p[n+1]<<8)); }
Sample parse_status(std::span<const std::uint8_t> p,const Clock& received,std::uint64_t sequence) {
    if(p.size()<28) throw std::invalid_argument("Status payload is shorter than 28 bytes");
    Sample s; s.received=received; s.sequence=sequence;
    for(std::size_t i=0;i<kMetrics.size();++i) {
        s.raw[i]=u16(p,kMetrics[i].offset); s.values[i]=s.raw[i]*kMetrics[i].scale;
    }
    s.fault=u16(p,26); s.mode=p[8]; std::copy_n(p.begin()+9,3,s.reserved.begin()); return s;
}
static std::string text(std::span<const std::uint8_t> p) {
    std::string s(reinterpret_cast<const char*>(p.data()),p.size());
    while(!s.empty() && (s.back()=='\0' || s.back()==' ')) s.pop_back();
    return utf8(wide(s)); // 对损坏的 UTF-8 使用 Windows 的替代字符。
}
Json parse_info(std::span<const std::uint8_t> p) {
    if(p.size()<25) throw std::invalid_argument("Information payload is shorter than 25 bytes");
    bool date=p[23]<=12 && p[24]<=31;
    Json j={{"model",text(p.first(8))},{"firmware_version_raw",p[9]},
        {"rated_input_current_a",p[10]},{"rated_output_current_a",p[11]},
        {"rated_input_voltage_min_v",p[12]},{"rated_input_voltage_max_v",p[13]},
        {"rated_power_w",(p[14]<<8)|p[15]},{"warranty_raw",p[16]},
        {"certification_code",text(p.subspan(17,1))},{"layout",date?"date":"manufacturer"},
        {"manufacturer",date?"":text(p.subspan(18,7))},{"year_raw",date?p[22]:0},
        {"month_raw",date?p[23]:0},{"day_raw",date?p[24]:0},
        {"evidence","application_code"},{"device_compatibility","per_firmware_not_fully_confirmed"}};
    const std::string code=j["certification_code"];
    j["certification_name"]=code=="G"?"金牌":code=="P"?"白金":code=="T"?"钛金":code=="S"?"银牌":code=="B"?"铜牌":code;
    if(date) {
        char value[64]{};
        sprintf_s(value,"V%u.%u.%02u.%02u",p[9],p[22],p[23],p[24]); j["firmware_display"]=value;
    } else j["firmware_display"]="V"+std::to_string(p[9]);
    return j;
}
std::string parse_serial_number(std::span<const std::uint8_t> p) {
    if(p.size()<15) throw std::invalid_argument("Serial payload is shorter than 15 bytes");
    return text(p.first(15));
}
} // namespace dm

#include "agent/serial.hpp"
#include <setupapi.h>
#include <regex>
#include <algorithm>
#include <stdexcept>
namespace dm {
static const GUID kPortsClass{0x4d36e978,0xe325,0x11ce,{0xbf,0xc1,0x08,0x00,0x2b,0xe1,0x03,0x18}};
std::vector<Port> enumerate_ports() {
    HDEVINFO list=SetupDiGetClassDevsW(&kPortsClass,nullptr,nullptr,DIGCF_PRESENT);
    if(list==INVALID_HANDLE_VALUE) throw std::runtime_error(error_text());
    struct Guard { HDEVINFO value; ~Guard(){SetupDiDestroyDeviceInfoList(value);} } guard{list};
    std::vector<Port> ports;
    SP_DEVINFO_DATA item{sizeof(item)};
    for(DWORD index=0;SetupDiEnumDeviceInfo(list,index,&item);++index) {
        HKEY key=SetupDiOpenDevRegKey(list,&item,DICS_FLAG_GLOBAL,0,DIREG_DEV,KEY_QUERY_VALUE);
        if(key==INVALID_HANDLE_VALUE) continue;
        wchar_t name[64]{}; DWORD size=sizeof(name);
        auto rc=RegGetValueW(key,nullptr,L"PortName",RRF_RT_REG_SZ,nullptr,name,&size);
        RegCloseKey(key);
        if(rc!=ERROR_SUCCESS || !std::regex_match(utf8(name),std::regex("COM[1-9][0-9]{0,4}",std::regex::icase))) continue;
        wchar_t instance[4096]{},description[512]{};
        if(!SetupDiGetDeviceInstanceIdW(list,&item,instance,4096,nullptr)) continue;
        SetupDiGetDeviceRegistryPropertyW(list,&item,SPDRP_FRIENDLYNAME,nullptr,
            reinterpret_cast<PBYTE>(description),sizeof(description),nullptr);
        ports.push_back({utf8(name),utf8(instance),utf8(description)});
    }
    std::sort(ports.begin(),ports.end(),[](const Port& a,const Port& b){ return a.name.size()==b.name.size()?a.name<b.name:a.name.size()<b.name.size(); });
    return ports;
}
std::optional<Port> resolve_port(const Config& c) {
    auto ports=enumerate_ports();
    std::vector<Port> matched;
    for(auto& p:ports) if(!c.device_instance.empty()?p.instance==c.device_instance:p.name==c.port) matched.push_back(p);
    if(matched.size()!=1) return std::nullopt;
    return matched.front();
}
SerialPort::SerialPort() : read_event_(CreateEventW(nullptr,TRUE,FALSE,nullptr)) {
    if(!read_event_) throw std::runtime_error(error_text());
    read_.hEvent=read_event_.get();
}
SerialPort::~SerialPort() { close(); }
void SerialPort::open(const Port& p,const Config& c) {
    close();
    if(!std::regex_match(p.name,std::regex("COM[1-9][0-9]{0,4}",std::regex::icase))) throw std::runtime_error("Invalid COM name");
    auto path=L"\\\\.\\"+wide(p.name);
    port_.reset(CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr));
    if(!port_) throw std::runtime_error("串口被占用、无权限或设备不可用："+error_text());
    try {
        DCB dcb{}; dcb.DCBlength=sizeof(dcb);
        if(!GetCommState(port_.get(),&dcb)) throw std::runtime_error(error_text());
        dcb.BaudRate=115200; dcb.ByteSize=static_cast<BYTE>(c.data_bits);
        dcb.Parity=c.parity=="E"?EVENPARITY:c.parity=="O"?ODDPARITY:NOPARITY;
        dcb.StopBits=c.stop_bits==2?TWOSTOPBITS:ONESTOPBIT;
        dcb.fBinary=TRUE; dcb.fParity=dcb.Parity!=NOPARITY;
        dcb.fOutxCtsFlow=FALSE; dcb.fOutxDsrFlow=FALSE; dcb.fDsrSensitivity=FALSE;
        dcb.fOutX=FALSE; dcb.fInX=FALSE; dcb.fNull=FALSE; dcb.fErrorChar=FALSE; dcb.fAbortOnError=FALSE;
        dcb.fDtrControl=c.dtr?DTR_CONTROL_ENABLE:DTR_CONTROL_DISABLE;
        dcb.fRtsControl=c.rts?RTS_CONTROL_ENABLE:RTS_CONTROL_DISABLE;
        COMMTIMEOUTS timeouts{50,0,1000,0,500};
        if(!SetCommState(port_.get(),&dcb) || !SetCommTimeouts(port_.get(),&timeouts) || !SetupComm(port_.get(),65536,1024))
            throw std::runtime_error(error_text());
        begin_read();
    } catch(...) { close(); throw; }
}
void SerialPort::close() {
    if(port_) {
        CancelIoEx(port_.get(),nullptr);
        if(pending_) {
            DWORD ignored=0;
            GetOverlappedResult(port_.get(),&read_,&ignored,TRUE);
        }
    }
    pending_=false; port_.reset(); ResetEvent(read_event_.get());
}
void SerialPort::begin_read() {
    if(!port_ || pending_) return;
    ResetEvent(read_event_.get());
    read_={}; read_.hEvent=read_event_.get();
    DWORD bytes=0;
    pending_=true;
    if(!ReadFile(port_.get(),buffer_.data(),static_cast<DWORD>(buffer_.size()),&bytes,&read_)) {
        auto error=GetLastError();
        if(error!=ERROR_IO_PENDING) { pending_=false; throw std::runtime_error(error_text(error)); }
    } else SetEvent(read_event_.get());
}
std::optional<std::vector<std::uint8_t>> SerialPort::complete_read() {
    if(!pending_ || WaitForSingleObject(read_event_.get(),0)!=WAIT_OBJECT_0) return std::nullopt;
    DWORD bytes=0;
    if(!GetOverlappedResult(port_.get(),&read_,&bytes,FALSE)) {
        auto error=GetLastError(); pending_=false;
        throw std::runtime_error(error_text(error));
    }
    pending_=false;
    std::vector<std::uint8_t> result(buffer_.begin(),buffer_.begin()+bytes);
    if(bytes)begin_read();else ResetEvent(read_event_.get());
    return result;
}
void SerialPort::send(std::span<const std::uint8_t> bytes,HANDLE interrupt) {
    if(!port_) throw std::runtime_error("串口已关闭");
    if(WaitForSingleObject(interrupt,0)==WAIT_OBJECT_0)throw std::runtime_error("启用意图已变化，取消发送");
    Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));
    if(!event) throw std::runtime_error(error_text());
    OVERLAPPED op{}; op.hEvent=event.get();
    DWORD done=0;
    if(!WriteFile(port_.get(),bytes.data(),static_cast<DWORD>(bytes.size()),&done,&op)) {
        auto error=GetLastError();
        if(error!=ERROR_IO_PENDING) throw std::runtime_error(error_text(error));
        HANDLE events[]{interrupt,event.get()};
        auto wait=WaitForMultipleObjects(2,events,FALSE,500);
        if(wait!=WAIT_OBJECT_0+1) {
            CancelIoEx(port_.get(),&op);
            GetOverlappedResult(port_.get(),&op,&done,TRUE);
            throw std::runtime_error(wait==WAIT_TIMEOUT?"发送超时，未自动重试":"发送已取消，未自动重试");
        }
        if(!GetOverlappedResult(port_.get(),&op,&done,FALSE)) throw std::runtime_error(error_text());
    }
    if(done!=bytes.size()) throw std::runtime_error("发送不完整，未自动重试");
}
} // namespace dm

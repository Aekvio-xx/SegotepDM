#include "common/shared_memory.hpp"
#include <cstring>
#include <stdexcept>
namespace dm {
SnapshotWriter::SnapshotWriter() {
    UserSecurity security;
    auto name=object_name(L"Telemetry");
    mapping_.reset(CreateFileMappingW(INVALID_HANDLE_VALUE,security.get(),PAGE_READWRITE,0,
        static_cast<DWORD>(kMappingBytes),name.c_str()));
    if(!mapping_) throw std::runtime_error(error_text());
    view_=MapViewOfFile(mapping_.get(),FILE_MAP_WRITE|FILE_MAP_READ,0,0,kMappingBytes);
    if(!view_) throw std::runtime_error(error_text());
}
SnapshotWriter::~SnapshotWriter() { if(view_) UnmapViewOfFile(view_); }
void SnapshotWriter::publish(const Json& value) {
    auto text=value.dump(-1,' ',false,Json::error_handler_t::replace);
    if(text.size()>kMappingBytes-kHeaderBytes) throw std::runtime_error("Snapshot capacity exceeded");
    auto* h=static_cast<MappingHeader*>(view_);
    const auto base=static_cast<std::uint32_t>(h->sequence)&~1u;
    InterlockedExchange(&h->sequence,static_cast<LONG>(base+1u));
    MemoryBarrier();
    h->magic=kMappingMagic; h->major=1; h->capacity=static_cast<std::uint32_t>(kMappingBytes);
    h->payload_bytes=static_cast<std::uint32_t>(text.size());
    std::memset(h->reserved,0,sizeof(h->reserved));
    std::memcpy(static_cast<char*>(view_)+kHeaderBytes,text.data(),text.size());
    MemoryBarrier();
    InterlockedExchange(&h->sequence,static_cast<LONG>(base+2u));
}
SnapshotReader::~SnapshotReader() { if(view_) UnmapViewOfFile(view_); }
std::optional<std::string> SnapshotReader::read() {
    auto now=GetTickCount64();
    if(!view_) {
        if(now<next_open_) return std::nullopt;
        next_open_=now+5000;
        auto name=object_name(L"Telemetry");
        mapping_.reset(OpenFileMappingW(FILE_MAP_READ,FALSE,name.c_str()));
        if(!mapping_) return std::nullopt;
        view_=MapViewOfFile(mapping_.get(),FILE_MAP_READ,0,0,kMappingBytes);
        if(!view_) { mapping_.reset(); return std::nullopt; }
    }
    auto* h=static_cast<const MappingHeader*>(view_);
    for(int attempt=0;attempt<2;++attempt) {
        const LONG before=h->sequence;
        MemoryBarrier();
        if(static_cast<unsigned long>(before)&1u) continue;
        const auto magic=h->magic,major=h->major,capacity=h->capacity,bytes=h->payload_bytes;
        if(magic!=kMappingMagic || major!=1 || capacity!=kMappingBytes || !bytes || bytes>kMappingBytes-kHeaderBytes) return std::nullopt;
        std::string text(bytes,'\0');
        std::memcpy(text.data(),static_cast<const char*>(view_)+kHeaderBytes,bytes);
        MemoryBarrier();
        if(before==h->sequence) return text;
    }
    return std::nullopt;
}
} // namespace dm

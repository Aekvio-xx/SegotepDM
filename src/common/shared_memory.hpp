#pragma once
#include "common/win.hpp"
namespace dm {
inline constexpr std::size_t kMappingBytes=65536, kHeaderBytes=64;
inline constexpr std::uint32_t kMappingMagic=0x314d444d;
struct alignas(8) MappingHeader {
    std::uint32_t magic, major, capacity;
    volatile LONG sequence;
    std::uint32_t payload_bytes;
    std::uint32_t reserved[11];
};
static_assert(sizeof(MappingHeader)==kHeaderBytes);
static_assert(offsetof(MappingHeader,sequence)==12);
class SnapshotWriter {
    Handle mapping_;
    void* view_=nullptr;
public:
    SnapshotWriter();
    ~SnapshotWriter();
    void publish(const Json& value);
};
class SnapshotReader {
    Handle mapping_;
    const void* view_=nullptr;
    std::uint64_t next_open_=0;
public:
    ~SnapshotReader();
    std::optional<std::string> read();
};
} // namespace dm

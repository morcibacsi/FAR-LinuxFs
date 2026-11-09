#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i64 = std::int64_t;

// Raised for malformed images, unsupported features and I/O failures.
class FsError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

inline u16 Le16(const void* p)
{
    u16 v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

inline u32 Le32(const void* p)
{
    u32 v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

inline u64 Le64(const void* p)
{
    u64 v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

std::wstring Widen(const std::string& utf8);
std::string Narrow(const std::wstring& wide);

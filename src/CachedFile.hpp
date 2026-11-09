#pragma once

#include "Common.hpp"

#include <string>
#include <vector>

// Read-only random access to a file through a small direct-mapped block cache.
// FAR is commonly 32-bit, so images are never mapped into the address space.
class CachedFile
{
public:
    explicit CachedFile(const std::wstring& path);
    ~CachedFile();
    CachedFile(const CachedFile&) = delete;
    CachedFile& operator=(const CachedFile&) = delete;

    u64 Size() const { return size_; }

    // Returns the number of bytes available, which is short only at end of file.
    size_t TryRead(u64 offset, void* dest, size_t count);
    // Throws FsError unless all bytes could be read.
    void ReadAt(u64 offset, void* dest, size_t count);

private:
    static constexpr unsigned BlockShift = 16;
    static constexpr size_t BlockSize = size_t{1} << BlockShift;
    static constexpr size_t SlotCount = 512;

    void* handle_;
    u64 size_ = 0;
    std::vector<char> data_;
    std::vector<u64> tags_;
    std::vector<u32> lengths_;
};

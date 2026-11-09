#include "CachedFile.hpp"

#include <windows.h>

#include <algorithm>

CachedFile::CachedFile(const std::wstring& path)
{
    handle_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE)
        throw FsError("cannot open image file");

    LARGE_INTEGER size;
    if (!GetFileSizeEx(handle_, &size))
    {
        CloseHandle(handle_);
        throw FsError("cannot determine image size");
    }
    size_ = static_cast<u64>(size.QuadPart);
    data_.resize(BlockSize * SlotCount);
    tags_.assign(SlotCount, ~u64{0});
    lengths_.assign(SlotCount, 0);
}

CachedFile::~CachedFile()
{
    CloseHandle(handle_);
}

size_t CachedFile::TryRead(u64 offset, void* dest, size_t count)
{
    auto out = static_cast<char*>(dest);
    size_t total = 0;
    while (count != 0 && offset < size_)
    {
        const u64 block = offset >> BlockShift;
        const size_t slot = static_cast<size_t>(block % SlotCount);
        char* mem = data_.data() + slot * BlockSize;
        if (tags_[slot] != block)
        {
            tags_[slot] = ~u64{0};
            LARGE_INTEGER pos;
            pos.QuadPart = static_cast<LONGLONG>(block << BlockShift);
            DWORD got = 0;
            if (!SetFilePointerEx(handle_, pos, nullptr, FILE_BEGIN)
                || !::ReadFile(handle_, mem, static_cast<DWORD>(BlockSize), &got, nullptr))
                throw FsError("read error on image file");
            tags_[slot] = block;
            lengths_[slot] = got;
        }

        const size_t inBlock = static_cast<size_t>(offset & (BlockSize - 1));
        if (inBlock >= lengths_[slot])
            break;
        const size_t n = (std::min)(count, lengths_[slot] - inBlock);
        std::memcpy(out, mem + inBlock, n);
        out += n;
        offset += n;
        count -= n;
        total += n;
    }
    return total;
}

void CachedFile::ReadAt(u64 offset, void* dest, size_t count)
{
    if (TryRead(offset, dest, count) != count)
        throw FsError("read past end of image");
}

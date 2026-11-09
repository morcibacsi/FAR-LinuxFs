#pragma once

#include "Common.hpp"

u32 Crc32(u32 crc, const void* data, size_t size);

// Both functions throw FsError on corrupt input and return the output length.
size_t Lzo1xDecompress(const u8* in, size_t inSize, u8* out, size_t outSize);
size_t InflateRaw(const u8* in, size_t inSize, u8* out, size_t outSize);

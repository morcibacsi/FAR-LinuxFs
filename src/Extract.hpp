#pragma once

#include "FsImage.hpp"

#include <string>
#include <vector>

enum class OverwriteChoice
{
    Overwrite,
    Skip,
    Cancel,
};

enum class ErrorChoice
{
    Skip,
    Cancel,
};

class ExtractCallback
{
public:
    virtual ~ExtractCallback() = default;
    virtual OverwriteChoice OnExists(const std::wstring& path) = 0;
    virtual ErrorChoice OnError(const std::wstring& path, const std::wstring& message) = 0;
    // Returns false to cancel.
    virtual bool OnProgress(const FsNode& node, u64 bytesDone, u64 bytesTotal) = 0;
};

struct ExtractStats
{
    u64 files = 0;
    u64 directories = 0;
    u64 symlinks = 0;
    u64 specials = 0;
    u64 skipped = 0;
    u64 errors = 0;
    u64 bytes = 0;
};

// Converts a Unix time to Windows FILETIME ticks, or 0 if it is not representable.
u64 FileTimeTicks(const FsTime& time);

// Adds the \\?\ prefix so that deep Linux trees are not limited by MAX_PATH.
std::wstring ToLongPath(const std::wstring& path);

// Extracts the given nodes and everything below them into destination, which
// must be an absolute path. Returns false if the operation was cancelled.
bool ExtractNodes(FsImage& image, const std::vector<const FsNode*>& nodes, const std::wstring& destination,
    ExtractCallback& callback, ExtractStats& stats);

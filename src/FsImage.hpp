#pragma once

#include "CachedFile.hpp"
#include "Common.hpp"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

enum class NodeKind
{
    File,
    Dir,
    Symlink,
    BlockDev,
    CharDev,
    Fifo,
    Socket,
    Unknown,
};

struct FsTime
{
    i64 sec = 0;
    u32 nsec = 0;
};

struct FsNode
{
    std::wstring name;        // Windows-safe and unique within the directory
    std::wstring description; // "lrwxrwxrwx -> target"
    std::wstring owner;       // "uid:gid"
    NodeKind kind = NodeKind::Unknown;
    u64 inum = 0;
    u64 size = 0;
    u32 mode = 0;
    u32 uid = 0;
    u32 gid = 0;
    u32 nlink = 0;
    FsTime atime;
    FsTime mtime;
    FsTime ctime;
    std::string linkTarget;
    FsNode* parent = nullptr;
    std::vector<std::unique_ptr<FsNode>> children; // sorted by name, ignoring case
};

// Receives file content in order. Returning false aborts the read.
using WriteSink = std::function<bool(const void* data, size_t size)>;

class FsImage
{
public:
    virtual ~FsImage() = default;

    FsNode& Root() { return root_; }
    const std::wstring& Format() const { return format_; }
    const std::vector<std::pair<std::wstring, std::wstring>>& Info() const { return info_; }
    const std::vector<std::wstring>& Warnings() const { return warnings_; }

    // Emits exactly node.size bytes. Returns false if the sink aborted the read.
    virtual bool ReadFile(const FsNode& node, const WriteSink& sink) = 0;

protected:
    // Links a child under its parent, deriving its display name and description.
    // Returns nullptr for names which must never be followed, such as "..".
    FsNode* AddChild(FsNode& parent, const std::string& rawName, std::unique_ptr<FsNode> child);
    // Call once the root node's attributes are final.
    void FinishTree();

    FsNode root_;
    std::wstring format_;
    std::vector<std::pair<std::wstring, std::wstring>> info_;
    std::vector<std::wstring> warnings_;
};

NodeKind KindFromMode(u32 mode);

// Finds a direct child by its display name, ignoring case.
const FsNode* FindChild(const FsNode& dir, const std::wstring& name);

enum class ImageType
{
    None,
    Ubifs,
    Ext,
};

// Classifies an image from its leading bytes.
ImageType DetectImage(const void* data, size_t size);

// Opens an image of any supported type. Throws FsError if that fails.
std::unique_ptr<FsImage> OpenImage(const std::wstring& path);

std::unique_ptr<FsImage> OpenUbifs(std::unique_ptr<CachedFile> file);
std::unique_ptr<FsImage> OpenExt(std::unique_ptr<CachedFile> file);

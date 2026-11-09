#include "FsImage.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <string_view>

namespace
{
    std::wstring Decode(UINT codePage, DWORD flags, const std::string& text)
    {
        if (text.empty())
            return {};
        const int count = MultiByteToWideChar(codePage, flags, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (count == 0)
            return {};
        std::wstring result(static_cast<size_t>(count), L'\0');
        MultiByteToWideChar(codePage, flags, text.data(), static_cast<int>(text.size()), result.data(), count);
        return result;
    }

    std::wstring FoldCase(std::wstring text)
    {
        if (!text.empty())
            CharUpperBuffW(text.data(), static_cast<DWORD>(text.size()));
        return text;
    }

    bool IsReservedDeviceName(const std::wstring& name)
    {
        static constexpr std::array<std::wstring_view, 22> reserved{
            L"CON", L"PRN", L"AUX", L"NUL",
            L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7", L"COM8", L"COM9",
            L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9",
        };
        const std::wstring stem = FoldCase(name.substr(0, name.find(L'.')));
        return std::find(reserved.begin(), reserved.end(), std::wstring_view(stem)) != reserved.end();
    }

    // Linux permits names NTFS cannot store. Rewriting them here means the panel
    // shows exactly the name extraction will create.
    std::wstring SanitizeName(const std::string& raw)
    {
        std::wstring name = Widen(raw);
        for (auto& c : name)
        {
            if (c < 0x20 || wcschr(L"<>:\"/\\|?*", c) != nullptr)
                c = L'_';
        }
        if (name.empty())
            name = L"_";
        if (name.back() == L'.' || name.back() == L' ')
            name.back() = L'_';
        if (IsReservedDeviceName(name))
            name.insert(0, 1, L'_');
        return name;
    }

    std::wstring ModeString(NodeKind kind, u32 mode)
    {
        wchar_t type = L'?';
        switch (kind)
        {
        case NodeKind::File: type = L'-'; break;
        case NodeKind::Dir: type = L'd'; break;
        case NodeKind::Symlink: type = L'l'; break;
        case NodeKind::BlockDev: type = L'b'; break;
        case NodeKind::CharDev: type = L'c'; break;
        case NodeKind::Fifo: type = L'p'; break;
        case NodeKind::Socket: type = L's'; break;
        case NodeKind::Unknown: break;
        }

        std::wstring text(1, type);
        for (int shift = 6; shift >= 0; shift -= 3)
        {
            const u32 bits = (mode >> shift) & 7;
            text += (bits & 4) ? L'r' : L'-';
            text += (bits & 2) ? L'w' : L'-';
            text += (bits & 1) ? L'x' : L'-';
        }
        if (mode & 04000) text[3] = (mode & 0100) ? L's' : L'S';
        if (mode & 02000) text[6] = (mode & 0010) ? L's' : L'S';
        if (mode & 01000) text[9] = (mode & 0001) ? L't' : L'T';
        return text;
    }

    bool LessNoCase(const std::wstring& a, const std::wstring& b)
    {
        return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
    }

    void Describe(FsNode& node)
    {
        node.description = ModeString(node.kind, node.mode);
        if (node.kind == NodeKind::Symlink)
            node.description += L" -> " + Widen(node.linkTarget);
        node.owner = std::to_wstring(node.uid) + L":" + std::to_wstring(node.gid);
    }

    bool IsUbifs(const u8* p, size_t size)
    {
        if (size < 100 || Le32(p) != 0x06101831 || p[20] != 6 || Le32(p + 16) != 4096)
            return false;
        const u32 minIo = Le32(p + 32);
        const u32 lebSize = Le32(p + 36);
        return minIo != 0 && (minIo & (minIo - 1)) == 0 && lebSize != 0 && lebSize % minIo == 0;
    }

    bool IsExt(const u8* p, size_t size)
    {
        if (size < 0x400 + 264)
            return false;
        const u8* sb = p + 0x400;
        return Le16(sb + 56) == 0xEF53 && Le32(sb + 24) <= 6 && Le32(sb + 4) != 0
            && Le32(sb + 32) != 0 && Le32(sb + 40) != 0;
    }
}

std::wstring Widen(const std::string& utf8)
{
    std::wstring result = Decode(CP_UTF8, MB_ERR_INVALID_CHARS, utf8);
    if (result.empty() && !utf8.empty())
        result = Decode(1252, 0, utf8);
    return result;
}

std::string Narrow(const std::wstring& wide)
{
    if (wide.empty())
        return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), result.data(), count, nullptr, nullptr);
    return result;
}

NodeKind KindFromMode(u32 mode)
{
    switch (mode & 0xF000)
    {
    case 0x8000: return NodeKind::File;
    case 0x4000: return NodeKind::Dir;
    case 0xA000: return NodeKind::Symlink;
    case 0x6000: return NodeKind::BlockDev;
    case 0x2000: return NodeKind::CharDev;
    case 0x1000: return NodeKind::Fifo;
    case 0xC000: return NodeKind::Socket;
    default: return NodeKind::Unknown;
    }
}

FsNode* FsImage::AddChild(FsNode& parent, const std::string& rawName, std::unique_ptr<FsNode> child)
{
    if (rawName.empty() || rawName == "." || rawName == ".."
        || rawName.find('/') != std::string::npos || rawName.find('\0') != std::string::npos)
    {
        warnings_.push_back(L"Ignored invalid directory entry: " + Widen(rawName));
        return nullptr;
    }

    const std::wstring base = SanitizeName(rawName);
    std::wstring name = base;
    for (unsigned suffix = 1; FindChild(parent, name) != nullptr; ++suffix)
        name = base + L"~" + std::to_wstring(suffix);

    child->name = std::move(name);
    child->parent = &parent;
    Describe(*child);

    const auto where = std::lower_bound(parent.children.begin(), parent.children.end(), child->name,
        [](const auto& item, const std::wstring& key) { return LessNoCase(item->name, key); });
    FsNode* const result = child.get();
    parent.children.insert(where, std::move(child));
    return result;
}

void FsImage::FinishTree()
{
    Describe(root_);
}

const FsNode* FindChild(const FsNode& dir, const std::wstring& name)
{
    const auto it = std::lower_bound(dir.children.begin(), dir.children.end(), name,
        [](const auto& item, const std::wstring& key) { return LessNoCase(item->name, key); });
    if (it == dir.children.end() || LessNoCase(name, (*it)->name))
        return nullptr;
    return it->get();
}

ImageType DetectImage(const void* data, size_t size)
{
    const auto p = static_cast<const u8*>(data);
    if (IsUbifs(p, size))
        return ImageType::Ubifs;
    if (IsExt(p, size))
        return ImageType::Ext;
    return ImageType::None;
}

std::unique_ptr<FsImage> OpenImage(const std::wstring& path)
{
    auto file = std::make_unique<CachedFile>(path);
    std::vector<u8> head(0x1000);
    head.resize(file->TryRead(0, head.data(), head.size()));
    switch (DetectImage(head.data(), head.size()))
    {
    case ImageType::Ubifs: return OpenUbifs(std::move(file));
    case ImageType::Ext: return OpenExt(std::move(file));
    case ImageType::None: break;
    }
    throw FsError("not a UBIFS or ext2/3/4 image");
}

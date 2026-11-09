#include "Extract.hpp"

#include <windows.h>

#include <algorithm>

namespace
{
    constexpr size_t WriteBufferSize = 1 << 20;

    class Cancelled {};

    std::wstring LastErrorText(DWORD error)
    {
        wchar_t* text = nullptr;
        const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, error, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
        std::wstring result = length != 0 ? std::wstring(text, length) : L"error " + std::to_wstring(error);
        if (text != nullptr)
            LocalFree(text);
        while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n' || result.back() == L' '))
            result.pop_back();
        return result;
    }

    FILETIME ToFileTime(const FsTime& time)
    {
        const u64 ticks = FileTimeTicks(time);
        FILETIME result{};
        result.dwLowDateTime = static_cast<DWORD>(ticks);
        result.dwHighDateTime = static_cast<DWORD>(ticks >> 32);
        return result;
    }

    struct FileHandle
    {
        HANDLE value = INVALID_HANDLE_VALUE;
        FileHandle() = default;
        FileHandle(const FileHandle&) = delete;
        FileHandle& operator=(const FileHandle&) = delete;
        ~FileHandle() { Close(); }
        void Close()
        {
            if (value != INVALID_HANDLE_VALUE)
                CloseHandle(value);
            value = INVALID_HANDLE_VALUE;
        }
    };

    u64 CountBytes(const FsNode& node)
    {
        u64 total = node.kind == NodeKind::File ? node.size : 0;
        for (const auto& child : node.children)
            total += CountBytes(*child);
        return total;
    }

    class Extractor
    {
    public:
        Extractor(FsImage& image, ExtractCallback& callback, ExtractStats& stats, u64 total)
            : image_(image), callback_(callback), stats_(stats), total_(total), buffer_(WriteBufferSize)
        {
        }

        void Extract(const FsNode& node, const std::wstring& parentPath)
        {
            const std::wstring path = parentPath + L"\\" + node.name;
            if (!callback_.OnProgress(node, done_, total_))
                throw Cancelled{};

            try
            {
                if (node.kind == NodeKind::Dir)
                    ExtractDirectory(node, path);
                else
                    ExtractLeaf(node, path);
            }
            catch (const FsError& error)
            {
                ++stats_.errors;
                if (callback_.OnError(path, Widen(error.what())) == ErrorChoice::Cancel)
                    throw Cancelled{};
            }
        }

    private:
        void ExtractDirectory(const FsNode& node, const std::wstring& path)
        {
            if (!CreateDirectoryW(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
                throw FsError(Narrow(LastErrorText(GetLastError())));
            ++stats_.directories;

            for (const auto& child : node.children)
                Extract(*child, path);

            // Applied last: creating children would otherwise update the time again.
            FileHandle handle;
            handle.value = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (handle.value != INVALID_HANDLE_VALUE)
            {
                const FILETIME accessed = ToFileTime(node.atime);
                const FILETIME written = ToFileTime(node.mtime);
                SetFileTime(handle.value, nullptr, &accessed, &written);
            }
        }

        void ExtractLeaf(const FsNode& node, const std::wstring& path)
        {
            FileHandle handle;
            handle.value = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle.value == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_EXISTS)
            {
                switch (callback_.OnExists(path))
                {
                case OverwriteChoice::Skip:
                    ++stats_.skipped;
                    done_ += node.kind == NodeKind::File ? node.size : 0;
                    return;
                case OverwriteChoice::Cancel:
                    throw Cancelled{};
                case OverwriteChoice::Overwrite:
                    handle.value = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                    break;
                }
            }
            if (handle.value == INVALID_HANDLE_VALUE)
                throw FsError(Narrow(LastErrorText(GetLastError())));

            try
            {
                WriteContent(node, handle.value);
            }
            catch (...)
            {
                // Never leave a truncated file which looks like a successful extraction.
                handle.Close();
                DeleteFileW(path.c_str());
                throw;
            }

            const FILETIME accessed = ToFileTime(node.atime);
            const FILETIME written = ToFileTime(node.mtime);
            SetFileTime(handle.value, nullptr, &accessed, &written);

            switch (node.kind)
            {
            case NodeKind::File: ++stats_.files; break;
            case NodeKind::Symlink: ++stats_.symlinks; break;
            default: ++stats_.specials; break;
            }
        }

        void WriteContent(const FsNode& node, HANDLE handle)
        {
            if (node.kind == NodeKind::Symlink)
            {
                WriteAll(handle, node.linkTarget.data(), node.linkTarget.size());
                return;
            }
            if (node.kind != NodeKind::File)
                return;

            used_ = 0;
            const bool finished = image_.ReadFile(node, [&](const void* data, size_t size)
            {
                auto p = static_cast<const char*>(data);
                while (size != 0)
                {
                    const size_t n = (std::min)(size, buffer_.size() - used_);
                    std::memcpy(buffer_.data() + used_, p, n);
                    used_ += n;
                    p += n;
                    size -= n;
                    done_ += n;
                    stats_.bytes += n;
                    if (used_ == buffer_.size())
                        Flush(handle);
                }
                return callback_.OnProgress(node, done_, total_);
            });
            if (!finished)
                throw Cancelled{};
            Flush(handle);
        }

        void Flush(HANDLE handle)
        {
            WriteAll(handle, buffer_.data(), used_);
            used_ = 0;
        }

        static void WriteAll(HANDLE handle, const char* data, size_t size)
        {
            while (size != 0)
            {
                DWORD written = 0;
                if (!WriteFile(handle, data, static_cast<DWORD>((std::min)(size, size_t{1} << 30)), &written, nullptr))
                    throw FsError(Narrow(LastErrorText(GetLastError())));
                data += written;
                size -= written;
            }
        }

        FsImage& image_;
        ExtractCallback& callback_;
        ExtractStats& stats_;
        u64 total_;
        u64 done_ = 0;
        std::vector<char> buffer_;
        size_t used_ = 0;
    };

    void CreateDirectories(const std::wstring& longPath)
    {
        // The first separators belong to the \\?\ prefix and the volume root.
        size_t pos = longPath.rfind(L"UNC\\", 8) == 4 ? longPath.find(L'\\', longPath.find(L'\\', 8) + 1) : 6;
        while (pos != std::wstring::npos)
        {
            pos = longPath.find(L'\\', pos + 1);
            CreateDirectoryW(longPath.substr(0, pos).c_str(), nullptr);
        }
        if (GetFileAttributesW(longPath.c_str()) == INVALID_FILE_ATTRIBUTES)
            throw FsError(Narrow(LastErrorText(GetLastError())));
    }
}

u64 FileTimeTicks(const FsTime& time)
{
    constexpr i64 UnixEpoch = 11644473600;
    if (time.sec < -UnixEpoch || time.sec > 910692730085)
        return 0;
    return static_cast<u64>(time.sec + UnixEpoch) * 10000000 + time.nsec / 100;
}

std::wstring ToLongPath(const std::wstring& path)
{
    if (path.rfind(L"\\\\?\\", 0) == 0)
        return path;
    if (path.rfind(L"\\\\", 0) == 0)
        return L"\\\\?\\UNC\\" + path.substr(2);
    return L"\\\\?\\" + path;
}

bool ExtractNodes(FsImage& image, const std::vector<const FsNode*>& nodes, const std::wstring& destination,
    ExtractCallback& callback, ExtractStats& stats)
{
    std::wstring root = ToLongPath(destination);
    while (root.size() > 7 && (root.back() == L'\\' || root.back() == L'/'))
        root.pop_back();

    try
    {
        CreateDirectories(root);
    }
    catch (const FsError& error)
    {
        ++stats.errors;
        callback.OnError(destination, Widen(error.what()));
        return true;
    }

    u64 total = 0;
    for (const FsNode* node : nodes)
        total += CountBytes(*node);

    Extractor extractor(image, callback, stats, total);
    try
    {
        for (const FsNode* node : nodes)
            extractor.Extract(*node, root);
    }
    catch (const Cancelled&)
    {
        return false;
    }
    return true;
}

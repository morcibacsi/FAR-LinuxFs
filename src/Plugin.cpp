// FAR Manager 3 panel plugin which browses UBIFS and ext2/3/4 images like archives.

#include "Extract.hpp"

#include <windows.h>

#include <plugin.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace
{
    // {CA7366B4-EB32-4F41-A060-9727CCD17A7C}
    constexpr UUID PluginGuid = { 0xca7366b4, 0xeb32, 0x4f41, { 0xa0, 0x60, 0x97, 0x27, 0xcc, 0xd1, 0x7a, 0x7c } };
    // {2DD458BE-1B7E-4F29-BBC4-4E40D5B374CA}
    constexpr UUID MessageGuid = { 0x2dd458be, 0x1b7e, 0x4f29, { 0xbb, 0xc4, 0x4e, 0x40, 0xd5, 0xb3, 0x74, 0xca } };
    // {BC24E8D0-263B-4B3A-8A52-EEB9EF1E94C7}
    constexpr UUID DestinationGuid = { 0xbc24e8d0, 0x263b, 0x4b3a, { 0x8a, 0x52, 0xee, 0xb9, 0xef, 0x1e, 0x94, 0xc7 } };

    constexpr const wchar_t* PluginTitle = L"UBIFS Extract";
    constexpr size_t PathBufferSize = 32768;
    constexpr ULONGLONG ProgressIntervalMs = 150;
    constexpr size_t MaxDisplayedPath = 64;
    constexpr size_t MaxDisplayedWarnings = 5;

    PluginStartupInfo Psi;
    FarStandardFunctions Fsf;

    intptr_t ShowMessage(FARMESSAGEFLAGS flags, const std::vector<const wchar_t*>& items, intptr_t buttons)
    {
        return Psi.Message(&PluginGuid, &MessageGuid, flags, nullptr, items.data(), items.size(), buttons);
    }

    void ShowError(const std::wstring& text)
    {
        ShowMessage(FMSG_WARNING, { PluginTitle, text.c_str(), L"OK" }, 1);
    }

    // Keeps the end of a path, which is the part that tells files apart.
    std::wstring ShortenPath(const std::wstring& path)
    {
        std::wstring text = path.rfind(L"\\\\?\\", 0) == 0 ? path.substr(4) : path;
        if (text.size() > MaxDisplayedPath)
            text = L"..." + text.substr(text.size() - (MaxDisplayedPath - 3));
        return text;
    }

    bool EscapePressed()
    {
        const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        INPUT_RECORD record;
        DWORD count = 0;
        while (PeekConsoleInputW(input, &record, 1, &count) && count != 0)
        {
            ReadConsoleInputW(input, &record, 1, &count);
            if (record.EventType == KEY_EVENT && record.Event.KeyEvent.bKeyDown
                && record.Event.KeyEvent.wVirtualKeyCode == VK_ESCAPE)
                return true;
        }
        return false;
    }

    FILETIME ToFileTime(const FsTime& time)
    {
        const u64 ticks = FileTimeTicks(time);
        FILETIME result;
        result.dwLowDateTime = static_cast<DWORD>(ticks);
        result.dwHighDateTime = static_cast<DWORD>(ticks >> 32);
        return result;
    }

    class FarExtractCallback final : public ExtractCallback
    {
    public:
        explicit FarExtractCallback(bool interactive) : interactive_(interactive) {}

        ~FarExtractCallback() override
        {
            if (taskbarActive_)
                Psi.AdvControl(&PluginGuid, ACTL_SETPROGRESSSTATE, TBPS_NOPROGRESS, nullptr);
        }

        OverwriteChoice OnExists(const std::wstring& path) override
        {
            if (!interactive_ || overwriteAll_)
                return OverwriteChoice::Overwrite;
            if (skipAllExisting_)
                return OverwriteChoice::Skip;

            const std::wstring shortened = ShortenPath(path);
            switch (ShowMessage(FMSG_WARNING | FMSG_LEFTALIGN, { PluginTitle, L"The file already exists:", shortened.c_str(),
                L"&Overwrite", L"&Skip", L"Overwrite &all", L"S&kip all", L"&Cancel" }, 5))
            {
            case 0: return OverwriteChoice::Overwrite;
            case 1: return OverwriteChoice::Skip;
            case 2: overwriteAll_ = true; return OverwriteChoice::Overwrite;
            case 3: skipAllExisting_ = true; return OverwriteChoice::Skip;
            default: return OverwriteChoice::Cancel;
            }
        }

        ErrorChoice OnError(const std::wstring& path, const std::wstring& message) override
        {
            lastError_ = ShortenPath(path) + L": " + message;
            if (!interactive_ || skipAllErrors_)
                return ErrorChoice::Skip;

            const std::wstring shortened = ShortenPath(path);
            switch (ShowMessage(FMSG_WARNING | FMSG_LEFTALIGN, { PluginTitle, L"Cannot extract:", shortened.c_str(), message.c_str(),
                L"&Skip", L"Skip &all", L"&Cancel" }, 3))
            {
            case 0: return ErrorChoice::Skip;
            case 1: skipAllErrors_ = true; return ErrorChoice::Skip;
            default: return ErrorChoice::Cancel;
            }
        }

        bool OnProgress(const FsNode& node, u64 bytesDone, u64 bytesTotal) override
        {
            if (!interactive_)
                return true;

            const ULONGLONG now = GetTickCount64();
            if (shown_ && now - lastUpdate_ < ProgressIntervalMs)
                return true;
            lastUpdate_ = now;

            if (EscapePressed() && ShowMessage(FMSG_WARNING, { PluginTitle, L"Cancel the extraction?", L"&Yes", L"&No" }, 2) == 0)
                return false;

            const unsigned percent = bytesTotal != 0 ? static_cast<unsigned>(bytesDone * 100 / bytesTotal) : 100;
            const std::wstring name = ShortenPath(node.name);
            const std::wstring progress = std::to_wstring(percent) + L"%  (" + std::to_wstring(bytesDone >> 20)
                + L" of " + std::to_wstring(bytesTotal >> 20) + L" MB)";
            ShowMessage(FMSG_LEFTALIGN | (shown_ ? FMSG_KEEPBACKGROUND : FMSG_NONE), { PluginTitle, L"Extracting", name.c_str(), progress.c_str() }, 0);
            shown_ = true;

            ProgressValue value{ sizeof value, bytesDone, bytesTotal != 0 ? bytesTotal : 1 };
            Psi.AdvControl(&PluginGuid, ACTL_SETPROGRESSSTATE, TBPS_NORMAL, nullptr);
            Psi.AdvControl(&PluginGuid, ACTL_SETPROGRESSVALUE, 0, &value);
            taskbarActive_ = true;
            return true;
        }

        const std::wstring& LastError() const { return lastError_; }

    private:
        bool interactive_;
        bool overwriteAll_ = false;
        bool skipAllExisting_ = false;
        bool skipAllErrors_ = false;
        bool shown_ = false;
        bool taskbarActive_ = false;
        ULONGLONG lastUpdate_ = 0;
        std::wstring lastError_;
    };

    class Panel
    {
    public:
        Panel(std::wstring path, std::unique_ptr<FsImage> image)
            : hostFile_(std::move(path)), image_(std::move(image)), current_(&image_->Root())
        {
            for (const auto& [name, value] : image_->Info())
                infoLines_.push_back({ name.c_str(), value.c_str(), IPLFLAGS_NONE });
            UpdateTitles();
        }

        void FillOpenPanelInfo(OpenPanelInfo& info) const
        {
            info.StructSize = sizeof info;
            info.Flags = OPIF_ADDDOTS | OPIF_SHOWPRESERVECASE;
            info.HostFile = hostFile_.c_str();
            info.CurDir = currentDir_.c_str();
            info.Format = image_->Format().c_str();
            info.PanelTitle = title_.c_str();
            info.InfoLines = infoLines_.data();
            info.InfoLinesNumber = infoLines_.size();
        }

        void FillFindData(GetFindDataInfo& info) const
        {
            const auto& children = current_->children;
            auto items = std::make_unique<PluginPanelItem[]>(children.size());
            for (size_t i = 0; i != children.size(); ++i)
            {
                const FsNode& node = *children[i];
                PluginPanelItem& item = items[i];
                item = {};
                item.FileName = node.name.c_str();
                item.Description = node.description.c_str();
                item.Owner = node.owner.c_str();
                item.FileSize = node.size;
                item.NumberOfLinks = node.nlink;
                item.FileAttributes = node.kind == NodeKind::Dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
                item.CreationTime = ToFileTime(node.mtime);
                item.LastWriteTime = item.CreationTime;
                item.LastAccessTime = ToFileTime(node.atime);
                item.ChangeTime = ToFileTime(node.ctime);
            }
            info.ItemsNumber = children.size();
            info.PanelItem = items.release();
        }

        bool SetDirectory(const wchar_t* directory)
        {
            const FsNode* target = current_;
            const wchar_t* p = directory;
            if (*p == L'\\' || *p == L'/')
                target = &image_->Root();

            while (*p != L'\0')
            {
                while (*p == L'\\' || *p == L'/')
                    ++p;
                const wchar_t* end = p;
                while (*end != L'\0' && *end != L'\\' && *end != L'/')
                    ++end;
                if (end == p)
                    break;

                const std::wstring part(p, end);
                p = end;
                if (part == L".")
                    continue;
                if (part == L"..")
                {
                    if (target->parent == nullptr)
                        return false;
                    target = target->parent;
                    continue;
                }

                const FsNode* const child = FindChild(*target, part);
                if (child == nullptr || child->kind != NodeKind::Dir)
                    return false;
                target = child;
            }

            current_ = target;
            UpdateTitles();
            return true;
        }

        intptr_t GetFiles(GetFilesInfo& info)
        {
            const bool interactive = !(info.OpMode & OPM_SILENT);
            std::wstring destination = info.DestPath != nullptr ? info.DestPath : L"";
            std::vector<wchar_t> buffer(PathBufferSize);

            if (interactive)
            {
                if (!Psi.InputBox(&PluginGuid, &DestinationGuid, PluginTitle, L"Extract to:", L"UbiFsExtractDestination",
                    destination.c_str(), buffer.data(), buffer.size(), nullptr, FIB_BUTTONS | FIB_EDITPATH | FIB_NOUSELASTHISTORY))
                    return -1;
                destination = buffer.data();
            }

            if (Fsf.ConvertPath(CPM_FULL, destination.c_str(), buffer.data(), buffer.size()) != 0)
                destination = buffer.data();
            destination_ = destination;
            info.DestPath = destination_.c_str();

            std::vector<const FsNode*> nodes;
            for (size_t i = 0; i != info.ItemsNumber; ++i)
            {
                if (const FsNode* const node = FindChild(*current_, info.PanelItem[i].FileName))
                    nodes.push_back(node);
            }

            FarExtractCallback callback(interactive);
            ExtractStats stats;
            if (!ExtractNodes(*image_, nodes, destination_, callback, stats))
                return -1;

            // Silent extractions serve F3 and friends, which need to see failures too.
            if (stats.errors != 0 && (interactive || (info.OpMode & (OPM_VIEW | OPM_EDIT))))
            {
                const std::wstring summary = std::to_wstring(stats.errors) + L" item(s) could not be extracted.";
                ShowMessage(FMSG_WARNING | FMSG_LEFTALIGN, { PluginTitle, summary.c_str(), callback.LastError().c_str(), L"OK" }, 1);
            }

            for (size_t i = 0; i != info.ItemsNumber; ++i)
                info.PanelItem[i].Flags &= ~PPIF_SELECTED;
            return stats.errors == 0 ? 1 : 0;
        }

        void ShowWarnings() const
        {
            const auto& warnings = image_->Warnings();
            if (warnings.empty())
                return;

            const std::wstring heading = std::to_wstring(warnings.size()) + L" problem(s) found in this image:";
            std::vector<const wchar_t*> items{ PluginTitle, heading.c_str() };
            for (size_t i = 0; i != (std::min)(warnings.size(), MaxDisplayedWarnings); ++i)
                items.push_back(warnings[i].c_str());
            items.push_back(L"OK");
            ShowMessage(FMSG_WARNING | FMSG_LEFTALIGN, items, 1);
        }

    private:
        void UpdateTitles()
        {
            std::wstring relative;
            for (const FsNode* node = current_; node->parent != nullptr; node = node->parent)
                relative = relative.empty() ? node->name : node->name + L"\\" + relative;

            currentDir_ = relative;
            const size_t slash = hostFile_.find_last_of(L"\\/");
            const std::wstring imageName = slash == std::wstring::npos ? hostFile_ : hostFile_.substr(slash + 1);
            title_ = image_->Format() + L":" + imageName;
            if (!relative.empty())
                title_ += L"\\" + relative;
        }

        std::wstring hostFile_;
        std::unique_ptr<FsImage> image_;
        const FsNode* current_;
        std::wstring currentDir_;
        std::wstring title_;
        std::wstring destination_;
        std::vector<InfoPanelLine> infoLines_;
    };

    Panel* AsPanel(HANDLE handle)
    {
        return static_cast<Panel*>(handle);
    }

    HANDLE OpenImagePanel(const wchar_t* path, OPERATION_MODES mode)
    {
        const bool silent = (mode & OPM_SILENT) != 0;
        try
        {
            HANDLE savedScreen = nullptr;
            if (!silent)
            {
                savedScreen = Psi.SaveScreen(0, 0, -1, -1);
                ShowMessage(FMSG_NONE, { PluginTitle, L"Reading the image..." }, 0);
            }

            std::unique_ptr<FsImage> image;
            try
            {
                image = OpenImage(path);
            }
            catch (...)
            {
                if (!silent)
                    Psi.RestoreScreen(savedScreen);
                throw;
            }

            if (!silent)
                Psi.RestoreScreen(savedScreen);
            auto panel = std::make_unique<Panel>(path, std::move(image));
            if (!silent)
                panel->ShowWarnings();
            return panel.release();
        }
        catch (const std::exception& error)
        {
            if (!silent)
                ShowError(Widen(error.what()));
            return nullptr;
        }
    }
}

extern "C"
{
    void WINAPI GetGlobalInfoW(GlobalInfo* info)
    {
        info->StructSize = sizeof *info;
        info->MinFarVersion = FARMANAGERVERSION;
        info->Version = MAKEFARVERSION(1, 0, 0, 0, VS_RELEASE);
        info->Guid = PluginGuid;
        info->Title = PluginTitle;
        info->Description = L"Browse and extract UBIFS and ext2/3/4 images";
        info->Author = L"UbiFsExtract";
    }

    void WINAPI SetStartupInfoW(const PluginStartupInfo* info)
    {
        Psi = *info;
        Fsf = *info->FSF;
        Psi.FSF = &Fsf;
    }

    void WINAPI GetPluginInfoW(PluginInfo* info)
    {
        info->StructSize = sizeof *info;
        info->Flags = PF_NONE;
    }

    HANDLE WINAPI AnalyseW(const AnalyseInfo* info)
    {
        if (info->FileName == nullptr || info->Buffer == nullptr
            || DetectImage(info->Buffer, info->BufferSize) == ImageType::None)
            return nullptr;
        // Any non-null handle accepts the file: OpenW does the real work.
        return reinterpret_cast<HANDLE>(1);
    }

    HANDLE WINAPI OpenW(const OpenInfo* info)
    {
        if (info->OpenFrom != OPEN_ANALYSE)
            return nullptr;
        const auto analyse = reinterpret_cast<const OpenAnalyseInfo*>(info->Data);
        return OpenImagePanel(analyse->Info->FileName, analyse->Info->OpMode);
    }

    void WINAPI ClosePanelW(const ClosePanelInfo* info)
    {
        delete AsPanel(info->hPanel);
    }

    void WINAPI GetOpenPanelInfoW(OpenPanelInfo* info)
    {
        AsPanel(info->hPanel)->FillOpenPanelInfo(*info);
    }

    intptr_t WINAPI GetFindDataW(GetFindDataInfo* info)
    {
        try
        {
            AsPanel(info->hPanel)->FillFindData(*info);
            return TRUE;
        }
        catch (const std::exception&)
        {
            return FALSE;
        }
    }

    void WINAPI FreeFindDataW(const FreeFindDataInfo* info)
    {
        // The strings belong to the tree, so only the array itself is released.
        delete[] info->PanelItem;
    }

    intptr_t WINAPI SetDirectoryW(const SetDirectoryInfo* info)
    {
        try
        {
            return AsPanel(info->hPanel)->SetDirectory(info->Dir) ? TRUE : FALSE;
        }
        catch (const std::exception&)
        {
            return FALSE;
        }
    }

    intptr_t WINAPI GetFilesW(GetFilesInfo* info)
    {
        try
        {
            return AsPanel(info->hPanel)->GetFiles(*info);
        }
        catch (const std::exception& error)
        {
            if (!(info->OpMode & OPM_SILENT))
                ShowError(Widen(error.what()));
            return 0;
        }
    }
}

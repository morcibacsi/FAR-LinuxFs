// Command-line front end to the image readers, for scripting and verification.

#include "Extract.hpp"

#include <windows.h>

#include <cstdio>
#include <cwchar>

namespace
{
    void Print(const std::wstring& text)
    {
        const std::string bytes = Narrow(text) + "\n";
        std::fwrite(bytes.data(), 1, bytes.size(), stdout);
    }

    void PrintTree(const FsNode& node, const std::wstring& path)
    {
        for (const auto& child : node.children)
        {
            const std::wstring childPath = path + L"/" + child->name;
            Print(child->description + L"  " + child->owner + L"  " + std::to_wstring(child->size) + L"  " + childPath);
            if (child->kind == NodeKind::Dir)
                PrintTree(*child, childPath);
        }
    }

    class ConsoleCallback final : public ExtractCallback
    {
    public:
        OverwriteChoice OnExists(const std::wstring&) override { return OverwriteChoice::Overwrite; }

        ErrorChoice OnError(const std::wstring& path, const std::wstring& message) override
        {
            Print(L"ERROR " + path + L": " + message);
            return ErrorChoice::Skip;
        }

        bool OnProgress(const FsNode&, u64, u64) override { return true; }
    };

    int Run(int argc, wchar_t** argv)
    {
        if (argc < 3)
        {
            Print(L"usage: ubifsx info|list <image>\n       ubifsx extract <image> <directory>");
            return 2;
        }

        const std::wstring command = argv[1];
        auto image = OpenImage(argv[2]);
        for (const auto& [name, value] : image->Info())
            Print(name + L": " + value);
        for (const auto& warning : image->Warnings())
            Print(L"WARNING " + warning);

        if (command == L"info")
            return 0;
        if (command == L"list")
        {
            PrintTree(image->Root(), L"");
            return 0;
        }
        if (command == L"extract" && argc >= 4)
        {
            wchar_t full[32768];
            if (GetFullPathNameW(argv[3], 32768, full, nullptr) == 0)
                throw FsError("invalid destination path");

            std::vector<const FsNode*> nodes;
            for (const auto& child : image->Root().children)
                nodes.push_back(child.get());

            ConsoleCallback callback;
            ExtractStats stats;
            ExtractNodes(*image, nodes, full, callback, stats);
            Print(L"files=" + std::to_wstring(stats.files) + L" dirs=" + std::to_wstring(stats.directories)
                + L" symlinks=" + std::to_wstring(stats.symlinks) + L" specials=" + std::to_wstring(stats.specials)
                + L" bytes=" + std::to_wstring(stats.bytes) + L" errors=" + std::to_wstring(stats.errors));
            return stats.errors != 0 ? 1 : 0;
        }

        Print(L"unknown command");
        return 2;
    }
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        return Run(argc, argv);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }
}

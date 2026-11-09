// Read-only UBIFS reader. It reconstructs the state a mounted file system would
// see: the committed index from the newest master node, overlaid with the journal.
// Offsets refer to fs/ubifs/ubifs-media.h in the Linux kernel.

#include "Decompress.hpp"
#include "FsImage.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace
{
    constexpr u32 NodeMagic = 0x06101831;
    constexpr u32 CrcInit = 0xFFFFFFFF;
    constexpr u32 CommonHeaderSize = 24;
    constexpr u32 BlockSize = 4096;
    constexpr u32 MaxIndexLevels = 64;
    constexpr u32 MaxDirDepth = 256;
    constexpr u8 PaddingByte = 0xCE;

    constexpr u32 FirstMasterLeb = 1;
    constexpr u32 MasterLebs = 2;
    constexpr u32 FirstLogLeb = FirstMasterLeb + MasterLebs;
    constexpr u32 RootInode = 1;
    constexpr u32 MasterNodeSize = 512;

    constexpr u32 InoNodeSize = 160;
    constexpr u32 DentNodeSize = 56;
    constexpr u32 DataNodeSize = 48;
    constexpr u32 TrunNodeSize = 56;
    constexpr u32 PadNodeSize = 28;
    constexpr u32 IdxNodeSize = 28;
    constexpr u32 BranchSize = 20;

    constexpr u32 SuperFlagEncryption = 0x10;
    constexpr u32 SuperFlagAuthentication = 0x20;

    enum NodeType : u8
    {
        NodeIno = 0,
        NodeData = 1,
        NodeDent = 2,
        NodeXent = 3,
        NodeTrun = 4,
        NodePad = 5,
        NodeSuper = 6,
        NodeMaster = 7,
        NodeRef = 8,
        NodeIndex = 9,
        NodeCommitStart = 10,
    };

    enum GroupType : u8
    {
        NoNodeGroup = 0,
        InNodeGroup = 1,
        LastOfNodeGroup = 2,
    };

    enum KeyType : u32
    {
        KeyIno = 0,
        KeyData = 1,
        KeyDent = 2,
        KeyXent = 3,
    };

    enum Compression : u16
    {
        CompressNone = 0,
        CompressLzo = 1,
        CompressZlib = 2,
        CompressZstd = 3,
    };

    u64 MakeKey(u32 inum, u32 type, u32 value)
    {
        return (u64{inum} << 32) | (type << 29) | (value & 0x1FFFFFFF);
    }

    u32 KeyInum(u64 key) { return static_cast<u32>(key >> 32); }
    u32 KeyKind(u64 key) { return static_cast<u32>(key) >> 29; }
    u32 KeyValue(u64 key) { return static_cast<u32>(key) & 0x1FFFFFFF; }

    u64 ReadKey(const u8* p)
    {
        return (u64{Le32(p)} << 32) | Le32(p + 4);
    }

    u32 Align8(u32 value)
    {
        return (value + 7) & ~u32{7};
    }

    const wchar_t* CompressionName(u32 type)
    {
        switch (type)
        {
        case CompressNone: return L"none";
        case CompressLzo: return L"LZO";
        case CompressZlib: return L"zlib";
        case CompressZstd: return L"zstd";
        default: return L"unknown";
        }
    }

    struct Location
    {
        u32 lnum;
        u32 offs;
        u32 len;
    };

    struct JournalEntry
    {
        u64 sqnum = 0;
        u8 type = 0;
        Location location{};
        u64 key = 0;
        u32 nlink = 0;
        u64 targetInum = 0;
        std::string name;
        u32 truncInum = 0;
        u64 oldSize = 0;
        u64 newSize = 0;
    };

    struct InodeInfo
    {
        u64 size = 0;
        FsTime atime;
        FsTime mtime;
        FsTime ctime;
        u32 nlink = 0;
        u32 uid = 0;
        u32 gid = 0;
        u32 mode = 0;
        std::string inlineData;
    };

    class UbifsImage final : public FsImage
    {
    public:
        explicit UbifsImage(std::unique_ptr<CachedFile> file);
        bool ReadFile(const FsNode& node, const WriteSink& sink) override;

    private:
        struct ScannedNode
        {
            const u8* data;
            u32 offs;
            u32 len;
        };

        void ReadSuperblock();
        void ReadMaster();
        void WalkIndex(const Location& location, int expectedLevel);
        void ReplayJournal();
        void CollectBud(u32 lnum, u32 offs, std::vector<JournalEntry>& entries);
        void ApplyJournal(const JournalEntry& entry);
        void ApplyDirEntry(const JournalEntry& entry);
        void RemoveRange(u64 first, u64 last);
        void LoadInodes();
        void BuildDirectory(FsNode& directory, u32 depth);

        u64 LebOffset(u32 lnum, u32 offs) const;
        std::vector<u8> ReadNode(const Location& location, u8 expectedType);
        // Stops silently at the first byte which is not a valid node, as the
        // unwritten tail of a LEB and a torn write both look like that.
        std::vector<ScannedNode> ScanLeb(u32 lnum, u32 start, std::vector<u8>& buffer);
        void DecodeBlock(const u8* node, u32 nodeLen, u8* out);
        std::string ReadEntryName(const Location& location);

        std::unique_ptr<CachedFile> file_;
        u32 lebSize_ = 0;
        u32 lebCount_ = 0;
        u32 minIo_ = 0;
        u32 logLebs_ = 0;
        u32 defaultCompression_ = 0;
        u32 flags_ = 0;

        u64 commitNumber_ = 0;
        u32 logHeadLeb_ = 0;
        Location root_index_{};

        std::multimap<u64, Location> tree_;
        std::unordered_map<u32, InodeInfo> inodes_;
        std::unordered_set<u32> visitedDirectories_;
    };

    UbifsImage::UbifsImage(std::unique_ptr<CachedFile> file) : file_(std::move(file))
    {
        format_ = L"UBIFS";
        ReadSuperblock();
        ReadMaster();
        WalkIndex(root_index_, -1);
        ReplayJournal();
        LoadInodes();

        const auto root = inodes_.find(RootInode);
        if (root == inodes_.end() || KindFromMode(root->second.mode) != NodeKind::Dir)
            throw FsError("UBIFS root directory is missing");

        root_.kind = NodeKind::Dir;
        root_.inum = RootInode;
        root_.mode = root->second.mode;
        root_.uid = root->second.uid;
        root_.gid = root->second.gid;
        root_.nlink = root->second.nlink;
        root_.atime = root->second.atime;
        root_.mtime = root->second.mtime;
        root_.ctime = root->second.ctime;
        visitedDirectories_.insert(RootInode);
        BuildDirectory(root_, 0);
        FinishTree();

        info_.emplace_back(L"Format", L"UBIFS");
        info_.emplace_back(L"LEB size", std::to_wstring(lebSize_));
        info_.emplace_back(L"LEB count", std::to_wstring(lebCount_));
        info_.emplace_back(L"Min I/O size", std::to_wstring(minIo_));
        info_.emplace_back(L"Compression", CompressionName(defaultCompression_));
        info_.emplace_back(L"Commit number", std::to_wstring(commitNumber_));
    }

    u64 UbifsImage::LebOffset(u32 lnum, u32 offs) const
    {
        return u64{lnum} * lebSize_ + offs;
    }

    void UbifsImage::ReadSuperblock()
    {
        std::vector<u8> node(BlockSize);
        file_->ReadAt(0, node.data(), node.size());
        const u8* p = node.data();
        if (Le32(p) != NodeMagic || p[20] != NodeSuper || Le32(p + 16) != BlockSize)
            throw FsError("UBIFS superblock not found");
        if (Crc32(CrcInit, p + 8, BlockSize - 8) != Le32(p + 4))
            throw FsError("UBIFS superblock CRC mismatch");

        flags_ = Le32(p + 28);
        minIo_ = Le32(p + 32);
        lebSize_ = Le32(p + 36);
        lebCount_ = Le32(p + 40);
        logLebs_ = Le32(p + 56);
        defaultCompression_ = Le16(p + 84);

        if (p[27] != 0)
            throw FsError("unsupported UBIFS key format");
        if (flags_ & SuperFlagEncryption)
            throw FsError("encrypted UBIFS images are not supported");
        if (flags_ & SuperFlagAuthentication)
            throw FsError("authenticated UBIFS images are not supported");
        if (minIo_ == 0 || (minIo_ & (minIo_ - 1)) != 0 || lebSize_ < BlockSize || lebSize_ % minIo_ != 0)
            throw FsError("invalid UBIFS geometry");
        if (lebCount_ <= FirstLogLeb + logLebs_ || logLebs_ == 0)
            throw FsError("invalid UBIFS layout");
    }

    void UbifsImage::ReadMaster()
    {
        std::vector<u8> buffer(lebSize_);
        u64 bestSqnum = 0;
        bool found = false;

        for (u32 lnum = FirstMasterLeb; lnum != FirstMasterLeb + MasterLebs; ++lnum)
        {
            for (const auto& scanned : ScanLeb(lnum, 0, buffer))
            {
                const u8* p = scanned.data;
                if (p[20] != NodeMaster || scanned.len != MasterNodeSize)
                    continue;
                const u64 sqnum = Le64(p + 8);
                if (found && sqnum <= bestSqnum)
                    continue;
                found = true;
                bestSqnum = sqnum;
                commitNumber_ = Le64(p + 32);
                logHeadLeb_ = Le32(p + 44);
                root_index_ = { Le32(p + 48), Le32(p + 52), Le32(p + 56) };
            }
        }

        if (!found)
            throw FsError("no valid UBIFS master node");
        if (logHeadLeb_ < FirstLogLeb || logHeadLeb_ >= FirstLogLeb + logLebs_)
            throw FsError("invalid UBIFS log position");
    }

    std::vector<u8> UbifsImage::ReadNode(const Location& location, u8 expectedType)
    {
        if (location.lnum >= lebCount_ || location.offs > lebSize_ || location.len > lebSize_ - location.offs
            || location.len < CommonHeaderSize)
            throw FsError("UBIFS node reference out of range");

        std::vector<u8> node(location.len);
        file_->ReadAt(LebOffset(location.lnum, location.offs), node.data(), node.size());
        const u8* p = node.data();
        if (Le32(p) != NodeMagic || Le32(p + 16) != location.len || p[20] != expectedType)
            throw FsError("UBIFS node header mismatch");
        if (Crc32(CrcInit, p + 8, location.len - 8) != Le32(p + 4))
            throw FsError("UBIFS node CRC mismatch");
        return node;
    }

    std::vector<UbifsImage::ScannedNode> UbifsImage::ScanLeb(u32 lnum, u32 start, std::vector<u8>& buffer)
    {
        std::vector<ScannedNode> nodes;
        buffer.resize(lebSize_);
        const size_t available = file_->TryRead(LebOffset(lnum, 0), buffer.data(), buffer.size());
        const u8* const leb = buffer.data();

        u32 offs = start;
        while (available >= CommonHeaderSize && offs <= available - CommonHeaderSize)
        {
            if (leb[offs] == PaddingByte)
            {
                while (offs < available && leb[offs] == PaddingByte)
                    ++offs;
                offs = Align8(offs);
                continue;
            }

            const u8* p = leb + offs;
            if (Le32(p) != NodeMagic)
                break;
            const u32 len = Le32(p + 16);
            if (len < CommonHeaderSize || len > available - offs)
                break;
            if (Crc32(CrcInit, p + 8, len - 8) != Le32(p + 4))
                break;

            if (p[20] == NodePad)
            {
                if (len != PadNodeSize)
                    break;
                const u32 padding = Le32(p + 24);
                if (padding > lebSize_)
                    break;
                offs += Align8(PadNodeSize + padding);
                continue;
            }

            nodes.push_back({ p, offs, len });
            offs += Align8(len);
        }
        return nodes;
    }

    void UbifsImage::WalkIndex(const Location& location, int expectedLevel)
    {
        const std::vector<u8> node = ReadNode(location, NodeIndex);
        if (location.len < IdxNodeSize)
            throw FsError("truncated UBIFS index node");

        const u8* p = node.data();
        const u32 childCount = Le16(p + 24);
        const u32 level = Le16(p + 26);
        if (level > MaxIndexLevels || (expectedLevel >= 0 && level != static_cast<u32>(expectedLevel)))
            throw FsError("inconsistent UBIFS index level");
        if (location.len != IdxNodeSize + childCount * BranchSize)
            throw FsError("inconsistent UBIFS index node size");

        for (u32 i = 0; i != childCount; ++i)
        {
            const u8* branch = p + IdxNodeSize + i * BranchSize;
            const Location child{ Le32(branch), Le32(branch + 4), Le32(branch + 8) };
            if (level != 0)
            {
                WalkIndex(child, static_cast<int>(level) - 1);
                continue;
            }

            // A crafted index can share subtrees to grow exponentially.
            if (tree_.size() >= file_->Size() / CommonHeaderSize)
                throw FsError("UBIFS index is larger than the image");
            tree_.emplace(ReadKey(branch + 12), child);
        }
    }

    void UbifsImage::CollectBud(u32 lnum, u32 offs, std::vector<JournalEntry>& entries)
    {
        if (lnum >= lebCount_ || offs >= lebSize_)
            return;

        std::vector<u8> buffer;
        std::vector<ScannedNode> nodes = ScanLeb(lnum, offs, buffer);
        // A node group is atomic: an unterminated group at the end was never completed.
        while (!nodes.empty() && nodes.back().data[21] == InNodeGroup)
            nodes.pop_back();

        for (const auto& scanned : nodes)
        {
            const u8* p = scanned.data;
            JournalEntry entry;
            entry.sqnum = Le64(p + 8);
            entry.type = p[20];
            entry.location = { lnum, scanned.offs, scanned.len };

            switch (entry.type)
            {
            case NodeIno:
                if (scanned.len < InoNodeSize)
                    continue;
                entry.key = ReadKey(p + 24);
                entry.nlink = Le32(p + 92);
                break;
            case NodeData:
                if (scanned.len < DataNodeSize)
                    continue;
                entry.key = ReadKey(p + 24);
                break;
            case NodeDent:
            case NodeXent:
            {
                if (scanned.len < DentNodeSize)
                    continue;
                const u32 nameLength = Le16(p + 50);
                if (nameLength > scanned.len - DentNodeSize)
                    continue;
                entry.key = ReadKey(p + 24);
                entry.targetInum = Le64(p + 40);
                entry.name.assign(reinterpret_cast<const char*>(p + DentNodeSize), nameLength);
                break;
            }
            case NodeTrun:
                if (scanned.len < TrunNodeSize)
                    continue;
                entry.truncInum = Le32(p + 24);
                entry.oldSize = Le64(p + 40);
                entry.newSize = Le64(p + 48);
                break;
            default:
                continue;
            }
            entries.push_back(std::move(entry));
        }
    }

    void UbifsImage::ReplayJournal()
    {
        std::vector<JournalEntry> entries;
        std::set<u32> buds;
        std::vector<u8> buffer;
        u64 commitStartSqnum = 0;
        bool haveCommitStart = false;
        u32 lnum = logHeadLeb_;

        for (u32 i = 0; i != logLebs_; ++i)
        {
            const std::vector<ScannedNode> nodes = ScanLeb(lnum, 0, buffer);
            if (nodes.empty())
                break;

            if (!haveCommitStart)
            {
                const u8* first = nodes.front().data;
                if (first[20] != NodeCommitStart || Le64(first + 24) != commitNumber_)
                    throw FsError("UBIFS log does not start with the current commit");
                commitStartSqnum = Le64(first + 8);
                haveCommitStart = true;
            }
            else if (Le64(nodes.front().data + 8) < commitStartSqnum)
            {
                // An older log LEB: its contents are already committed to the index.
                break;
            }

            for (const auto& scanned : nodes)
            {
                if (scanned.data[20] != NodeRef || scanned.len < 64)
                    continue;
                const u32 budLeb = Le32(scanned.data + 24);
                const u32 budOffs = Le32(scanned.data + 28);
                if (buds.insert(budLeb).second)
                    CollectBud(budLeb, budOffs, entries);
            }

            lnum = lnum + 1 == FirstLogLeb + logLebs_ ? FirstLogLeb : lnum + 1;
        }

        std::stable_sort(entries.begin(), entries.end(), [](const JournalEntry& a, const JournalEntry& b)
        {
            return a.sqnum < b.sqnum;
        });
        for (const auto& entry : entries)
            ApplyJournal(entry);

        if (!entries.empty())
            info_.emplace_back(L"Journal nodes replayed", std::to_wstring(entries.size()));
    }

    void UbifsImage::RemoveRange(u64 first, u64 last)
    {
        tree_.erase(tree_.lower_bound(first), tree_.upper_bound(last));
    }

    std::string UbifsImage::ReadEntryName(const Location& location)
    {
        u8 header[CommonHeaderSize];
        file_->ReadAt(LebOffset(location.lnum, location.offs), header, sizeof header);
        const std::vector<u8> node = ReadNode(location, header[20] == NodeXent ? u8{NodeXent} : u8{NodeDent});
        if (node.size() < DentNodeSize)
            throw FsError("truncated UBIFS directory entry");
        const u32 length = Le16(node.data() + 50);
        if (length > node.size() - DentNodeSize)
            throw FsError("invalid UBIFS directory entry name");
        return std::string(reinterpret_cast<const char*>(node.data() + DentNodeSize), length);
    }

    void UbifsImage::ApplyDirEntry(const JournalEntry& entry)
    {
        // Keys hold a name hash, so colliding names must be told apart by reading them.
        const auto [first, last] = tree_.equal_range(entry.key);
        for (auto it = first; it != last; ++it)
        {
            if (ReadEntryName(it->second) == entry.name)
            {
                tree_.erase(it);
                break;
            }
        }
        if (entry.targetInum != 0)
            tree_.emplace(entry.key, entry.location);
    }

    void UbifsImage::ApplyJournal(const JournalEntry& entry)
    {
        switch (entry.type)
        {
        case NodeIno:
            if (entry.nlink == 0)
            {
                const u32 inum = KeyInum(entry.key);
                RemoveRange(u64{inum} << 32, (u64{inum} << 32) | 0xFFFFFFFF);
                break;
            }
            tree_.erase(entry.key);
            tree_.emplace(entry.key, entry.location);
            break;

        case NodeData:
            tree_.erase(entry.key);
            tree_.emplace(entry.key, entry.location);
            break;

        case NodeDent:
        case NodeXent:
            ApplyDirEntry(entry);
            break;

        case NodeTrun:
        {
            u64 firstBlock = entry.newSize / BlockSize;
            if (entry.newSize % BlockSize != 0)
                ++firstBlock;
            if (entry.oldSize == 0)
                break;
            const u64 lastBlock = (entry.oldSize - 1) / BlockSize;
            if (firstBlock > lastBlock || firstBlock > 0x1FFFFFFF)
                break;
            RemoveRange(MakeKey(entry.truncInum, KeyData, static_cast<u32>(firstBlock)),
                MakeKey(entry.truncInum, KeyData, static_cast<u32>((std::min)(lastBlock, u64{0x1FFFFFFF}))));
            break;
        }

        default:
            break;
        }
    }

    void UbifsImage::LoadInodes()
    {
        inodes_.reserve(tree_.size() / 8);
        for (const auto& [key, location] : tree_)
        {
            if (KeyKind(key) != KeyIno || KeyValue(key) != 0)
                continue;

            const std::vector<u8> node = ReadNode(location, NodeIno);
            if (node.size() < InoNodeSize)
                throw FsError("truncated UBIFS inode");

            const u8* p = node.data();
            InodeInfo inode;
            inode.size = Le64(p + 48);
            inode.atime = { static_cast<i64>(Le64(p + 56)), Le32(p + 80) };
            inode.ctime = { static_cast<i64>(Le64(p + 64)), Le32(p + 84) };
            inode.mtime = { static_cast<i64>(Le64(p + 72)), Le32(p + 88) };
            inode.nlink = Le32(p + 92);
            inode.uid = Le32(p + 96);
            inode.gid = Le32(p + 100);
            inode.mode = Le32(p + 104);

            const u32 dataLength = Le32(p + 112);
            if (dataLength > node.size() - InoNodeSize)
                throw FsError("invalid UBIFS inode data length");
            inode.inlineData.assign(reinterpret_cast<const char*>(p + InoNodeSize), dataLength);
            inodes_.emplace(KeyInum(key), std::move(inode));
        }
    }

    void UbifsImage::BuildDirectory(FsNode& directory, u32 depth)
    {
        if (depth >= MaxDirDepth)
        {
            warnings_.push_back(L"Directory tree is too deep at " + directory.name);
            return;
        }

        const u32 inum = static_cast<u32>(directory.inum);
        const auto first = tree_.lower_bound(MakeKey(inum, KeyDent, 0));
        const auto last = tree_.upper_bound(MakeKey(inum, KeyDent, 0x1FFFFFFF));
        std::vector<FsNode*> subdirectories;

        for (auto it = first; it != last; ++it)
        {
            const std::vector<u8> node = ReadNode(it->second, NodeDent);
            if (node.size() < DentNodeSize)
                throw FsError("truncated UBIFS directory entry");

            const u8* p = node.data();
            const u64 childInum = Le64(p + 40);
            const u32 nameLength = Le16(p + 50);
            if (nameLength > node.size() - DentNodeSize)
                throw FsError("invalid UBIFS directory entry name");
            const std::string name(reinterpret_cast<const char*>(p + DentNodeSize), nameLength);

            const auto inode = inodes_.find(static_cast<u32>(childInum));
            if (childInum > 0xFFFFFFFF || inode == inodes_.end())
            {
                warnings_.push_back(L"Directory entry without an inode: " + Widen(name));
                continue;
            }

            auto child = std::make_unique<FsNode>();
            child->kind = KindFromMode(inode->second.mode);
            child->inum = childInum;
            child->mode = inode->second.mode;
            child->uid = inode->second.uid;
            child->gid = inode->second.gid;
            child->nlink = inode->second.nlink;
            child->atime = inode->second.atime;
            child->mtime = inode->second.mtime;
            child->ctime = inode->second.ctime;
            if (child->kind == NodeKind::File)
                child->size = inode->second.size;
            else if (child->kind == NodeKind::Symlink)
            {
                child->linkTarget = inode->second.inlineData;
                child->size = child->linkTarget.size();
            }

            const NodeKind kind = child->kind;
            FsNode* const added = AddChild(directory, name, std::move(child));
            if (added != nullptr && kind == NodeKind::Dir)
                subdirectories.push_back(added);
        }

        for (FsNode* subdirectory : subdirectories)
        {
            if (!visitedDirectories_.insert(static_cast<u32>(subdirectory->inum)).second)
            {
                warnings_.push_back(L"Directory linked more than once: " + subdirectory->name);
                continue;
            }
            BuildDirectory(*subdirectory, depth + 1);
        }
    }

    void UbifsImage::DecodeBlock(const u8* node, u32 nodeLen, u8* out)
    {
        const u32 size = Le32(node + 40);
        const u32 compression = Le16(node + 44);
        if (size > BlockSize)
            throw FsError("invalid UBIFS data node size");

        const u8* payload = node + DataNodeSize;
        const size_t payloadSize = nodeLen - DataNodeSize;
        size_t produced = 0;
        switch (compression)
        {
        case CompressNone:
            if (payloadSize != size)
                throw FsError("invalid uncompressed UBIFS data node");
            std::memcpy(out, payload, payloadSize);
            produced = payloadSize;
            break;
        case CompressLzo:
            produced = Lzo1xDecompress(payload, payloadSize, out, size);
            break;
        case CompressZlib:
            produced = InflateRaw(payload, payloadSize, out, size);
            break;
        case CompressZstd:
            throw FsError("zstd-compressed UBIFS data is not supported");
        default:
            throw FsError("unknown UBIFS compression type");
        }
        if (produced != size)
            throw FsError("UBIFS data node decompressed to the wrong size");
        std::memset(out + produced, 0, BlockSize - produced);
    }

    bool UbifsImage::ReadFile(const FsNode& node, const WriteSink& sink)
    {
        const u32 inum = static_cast<u32>(node.inum);
        auto it = tree_.lower_bound(MakeKey(inum, KeyData, 0));
        const auto last = tree_.upper_bound(MakeKey(inum, KeyData, 0x1FFFFFFF));

        u8 block[BlockSize];
        u64 remaining = node.size;
        for (u64 index = 0; remaining != 0; ++index)
        {
            while (it != last && u64{KeyValue(it->first)} < index)
                ++it;

            if (it != last && u64{KeyValue(it->first)} == index)
            {
                const std::vector<u8> data = ReadNode(it->second, NodeData);
                if (data.size() < DataNodeSize)
                    throw FsError("truncated UBIFS data node");
                DecodeBlock(data.data(), static_cast<u32>(data.size()), block);
                ++it;
            }
            else
            {
                // UBIFS does not store blocks of zeros: this is a sparse hole.
                std::memset(block, 0, BlockSize);
            }

            const size_t count = static_cast<size_t>((std::min)(remaining, u64{BlockSize}));
            if (!sink(block, count))
                return false;
            remaining -= count;
        }
        return true;
    }
}

std::unique_ptr<FsImage> OpenUbifs(std::unique_ptr<CachedFile> file)
{
    return std::make_unique<UbifsImage>(std::move(file));
}

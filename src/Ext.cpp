// Read-only ext2/3/4 reader. Directories are walked linearly, which also covers
// htree-indexed ones because their index blocks look like empty directory blocks.

#include "FsImage.hpp"

#include <algorithm>
#include <unordered_set>

namespace
{
    constexpr u64 SuperblockOffset = 1024;
    constexpr u32 RootInode = 2;
    constexpr u32 MaxDirDepth = 256;
    constexpr u64 MaxDirectorySize = 256u << 20;
    constexpr size_t InodeBlockArea = 60;
    constexpr u32 MaxExtentDepth = 5;

    constexpr u32 IncompatFiletype = 0x0002;
    constexpr u32 IncompatRecover = 0x0004;
    constexpr u32 IncompatExtents = 0x0040;
    constexpr u32 Incompat64Bit = 0x0080;
    constexpr u32 IncompatMmp = 0x0100;
    constexpr u32 IncompatFlexBg = 0x0200;
    constexpr u32 IncompatEaInode = 0x0400;
    constexpr u32 IncompatCsumSeed = 0x2000;
    constexpr u32 IncompatLargeDir = 0x4000;
    constexpr u32 IncompatCasefold = 0x20000;
    constexpr u32 IncompatSupported = IncompatFiletype | IncompatRecover | IncompatExtents | Incompat64Bit
        | IncompatMmp | IncompatFlexBg | IncompatEaInode | IncompatCsumSeed | IncompatLargeDir | IncompatCasefold;

    constexpr u32 CompatHasJournal = 0x0004;

    constexpr u32 InodeFlagExtents = 0x80000;
    constexpr u16 ExtentMagic = 0xF30A;
    constexpr u16 MaxInitializedExtent = 32768;

    struct Extent
    {
        u32 logical;
        u32 length;
        u64 physical;
        bool unwritten;
    };

    struct RawInode
    {
        u32 mode = 0;
        u32 uid = 0;
        u32 gid = 0;
        u64 size = 0;
        u32 links = 0;
        u32 blocks = 0;
        u32 flags = 0;
        bool hasXattrBlock = false;
        FsTime atime;
        FsTime ctime;
        FsTime mtime;
        u8 blockArea[InodeBlockArea]{};
    };

    FsTime DecodeTime(u32 seconds, bool haveExtra, u32 extra)
    {
        FsTime result;
        result.sec = static_cast<i64>(static_cast<std::int32_t>(seconds));
        if (haveExtra)
        {
            result.sec += static_cast<i64>(extra & 3) << 32;
            result.nsec = extra >> 2;
        }
        return result;
    }

    class ExtImage final : public FsImage
    {
    public:
        explicit ExtImage(std::unique_ptr<CachedFile> file);
        bool ReadFile(const FsNode& node, const WriteSink& sink) override;

    private:
        void ReadSuperblock();
        RawInode ReadInode(u32 inum);
        std::vector<Extent> MapBlocks(const RawInode& inode);
        void WalkExtents(const u8* node, size_t size, u32 depthLimit, std::vector<Extent>& out);
        void WalkIndirect(u32 block, u32 level, u64 limit, u64& logical, std::vector<Extent>& out);
        void AddMappedBlock(u64 logical, u32 block, std::vector<Extent>& out);
        std::string ReadWholeFile(const RawInode& inode);
        bool EmitContent(const RawInode& inode, u64 size, const WriteSink& sink);
        void BuildDirectory(FsNode& directory, const RawInode& inode, u32 depth);
        void FillNode(FsNode& node, u32 inum, const RawInode& inode);

        std::unique_ptr<CachedFile> file_;
        u32 blockSize_ = 0;
        u64 blockCount_ = 0;
        u32 inodeCount_ = 0;
        u32 firstDataBlock_ = 0;
        u32 inodesPerGroup_ = 0;
        u32 inodeSize_ = 0;
        u32 descriptorSize_ = 0;
        u32 incompat_ = 0;
        std::unordered_set<u32> visitedDirectories_;
    };

    ExtImage::ExtImage(std::unique_ptr<CachedFile> file) : file_(std::move(file))
    {
        ReadSuperblock();
        const RawInode root = ReadInode(RootInode);
        if (KindFromMode(root.mode) != NodeKind::Dir)
            throw FsError("ext root directory is missing");

        FillNode(root_, RootInode, root);
        root_.size = 0;
        visitedDirectories_.insert(RootInode);
        BuildDirectory(root_, root, 0);
        FinishTree();
    }

    void ExtImage::ReadSuperblock()
    {
        u8 sb[1024];
        file_->ReadAt(SuperblockOffset, sb, sizeof sb);
        if (Le16(sb + 56) != 0xEF53)
            throw FsError("ext superblock not found");

        const u32 logBlockSize = Le32(sb + 24);
        if (logBlockSize > 6)
            throw FsError("invalid ext block size");
        blockSize_ = u32{1024} << logBlockSize;
        inodeCount_ = Le32(sb);
        firstDataBlock_ = Le32(sb + 20);
        inodesPerGroup_ = Le32(sb + 40);
        const u32 revision = Le32(sb + 76);
        inodeSize_ = revision >= 1 ? Le16(sb + 88) : 128;
        const u32 compat = Le32(sb + 92);
        incompat_ = revision >= 1 ? Le32(sb + 96) : 0;

        if (incompat_ & ~IncompatSupported)
            throw FsError("this ext image uses features which are not supported");
        if (inodesPerGroup_ == 0 || inodeSize_ < 128 || inodeSize_ > blockSize_)
            throw FsError("invalid ext geometry");

        blockCount_ = Le32(sb + 4);
        descriptorSize_ = 32;
        if (incompat_ & Incompat64Bit)
        {
            blockCount_ |= u64{Le32(sb + 336)} << 32;
            descriptorSize_ = Le16(sb + 254);
            if (descriptorSize_ < 64 || descriptorSize_ > blockSize_)
                throw FsError("invalid ext group descriptor size");
        }

        if (incompat_ & IncompatRecover)
            warnings_.push_back(L"The journal was not replayed: this image was not unmounted cleanly, so recent changes may be missing.");

        format_ = (incompat_ & (IncompatExtents | Incompat64Bit)) ? L"ext4" : (compat & CompatHasJournal) ? L"ext3" : L"ext2";
        info_.emplace_back(L"Format", format_);
        info_.emplace_back(L"Block size", std::to_wstring(blockSize_));
        info_.emplace_back(L"Blocks", std::to_wstring(blockCount_));
        info_.emplace_back(L"Inodes", std::to_wstring(inodeCount_));
    }

    RawInode ExtImage::ReadInode(u32 inum)
    {
        if (inum == 0 || inum > inodeCount_)
            throw FsError("ext inode number out of range");

        const u32 group = (inum - 1) / inodesPerGroup_;
        const u32 index = (inum - 1) % inodesPerGroup_;
        u8 descriptor[64]{};
        const u64 tableOffset = u64{firstDataBlock_ + 1} * blockSize_;
        file_->ReadAt(tableOffset + u64{group} * descriptorSize_, descriptor, (std::min)(descriptorSize_, u32{64}));
        u64 inodeTable = Le32(descriptor + 8);
        if (descriptorSize_ >= 64)
            inodeTable |= u64{Le32(descriptor + 40)} << 32;

        std::vector<u8> buffer(inodeSize_);
        file_->ReadAt(inodeTable * blockSize_ + u64{index} * inodeSize_, buffer.data(), buffer.size());
        const u8* p = buffer.data();

        RawInode inode;
        inode.mode = Le16(p);
        inode.uid = Le16(p + 2) | (u32{Le16(p + 120)} << 16);
        inode.size = Le32(p + 4) | (u64{Le32(p + 108)} << 32);
        inode.gid = Le16(p + 24) | (u32{Le16(p + 122)} << 16);
        inode.links = Le16(p + 26);
        inode.blocks = Le32(p + 28);
        inode.flags = Le32(p + 32);
        inode.hasXattrBlock = Le32(p + 104) != 0;
        std::memcpy(inode.blockArea, p + 40, InodeBlockArea);

        const u32 extraSize = inodeSize_ > 128 ? Le16(p + 128) : 0;
        const auto fits = [&](u32 fieldEnd) { return inodeSize_ > 128 && 128 + extraSize >= fieldEnd && fieldEnd <= inodeSize_; };
        inode.ctime = DecodeTime(Le32(p + 12), fits(136), fits(136) ? Le32(p + 132) : 0);
        inode.mtime = DecodeTime(Le32(p + 16), fits(140), fits(140) ? Le32(p + 136) : 0);
        inode.atime = DecodeTime(Le32(p + 8), fits(144), fits(144) ? Le32(p + 140) : 0);
        return inode;
    }

    void ExtImage::WalkExtents(const u8* node, size_t size, u32 depthLimit, std::vector<Extent>& out)
    {
        if (size < 12 || Le16(node) != ExtentMagic)
            throw FsError("invalid ext extent header");
        const u32 entries = Le16(node + 2);
        const u32 depth = Le16(node + 6);
        if (depth > depthLimit || 12 + size_t{entries} * 12 > size)
            throw FsError("invalid ext extent tree");

        for (u32 i = 0; i != entries; ++i)
        {
            const u8* entry = node + 12 + size_t{i} * 12;
            if (depth != 0)
            {
                if (depthLimit == 0)
                    throw FsError("invalid ext extent tree");
                const u64 leaf = Le32(entry + 4) | (u64{Le16(entry + 8)} << 32);
                if (leaf >= blockCount_)
                    throw FsError("ext extent index out of range");
                std::vector<u8> child(blockSize_);
                file_->ReadAt(leaf * blockSize_, child.data(), child.size());
                WalkExtents(child.data(), child.size(), depth - 1, out);
                continue;
            }

            const u32 rawLength = Le16(entry + 4);
            const bool unwritten = rawLength > MaxInitializedExtent;
            const u32 length = unwritten ? rawLength - MaxInitializedExtent : rawLength;
            const u64 physical = Le32(entry + 8) | (u64{Le16(entry + 6)} << 32);
            if (physical > blockCount_ || length > blockCount_ - physical)
                throw FsError("ext extent out of range");
            out.push_back({ Le32(entry), length, physical, unwritten });
        }
    }

    void ExtImage::AddMappedBlock(u64 logical, u32 block, std::vector<Extent>& out)
    {
        if (block == 0)
            return;
        if (block >= blockCount_)
            throw FsError("ext block pointer out of range");
        if (!out.empty())
        {
            Extent& last = out.back();
            if (u64{last.logical} + last.length == logical && last.physical + last.length == block && last.length < 0xFFFF)
            {
                ++last.length;
                return;
            }
        }
        out.push_back({ static_cast<u32>(logical), 1, block, false });
    }

    void ExtImage::WalkIndirect(u32 block, u32 level, u64 limit, u64& logical, std::vector<Extent>& out)
    {
        const u64 perPointer = [&]
        {
            u64 span = 1;
            for (u32 i = 0; i != level; ++i)
                span *= blockSize_ / 4;
            return span;
        }();

        if (block == 0)
        {
            logical += perPointer * (blockSize_ / 4);
            return;
        }
        if (block >= blockCount_)
            throw FsError("ext indirect block out of range");

        std::vector<u8> pointers(blockSize_);
        file_->ReadAt(u64{block} * blockSize_, pointers.data(), pointers.size());
        for (u32 i = 0; i != blockSize_ / 4 && logical < limit; ++i)
        {
            const u32 target = Le32(pointers.data() + i * 4);
            if (level == 0)
            {
                AddMappedBlock(logical, target, out);
                ++logical;
            }
            else
            {
                WalkIndirect(target, level - 1, limit, logical, out);
            }
        }
    }

    std::vector<Extent> ExtImage::MapBlocks(const RawInode& inode)
    {
        std::vector<Extent> extents;
        if (inode.flags & InodeFlagExtents)
        {
            WalkExtents(inode.blockArea, InodeBlockArea, MaxExtentDepth, extents);
            std::sort(extents.begin(), extents.end(), [](const Extent& a, const Extent& b) { return a.logical < b.logical; });
            return extents;
        }

        const u64 limit = inode.size / blockSize_ + 1;
        u64 logical = 0;
        for (u32 i = 0; i != 12 && logical < limit; ++i, ++logical)
            AddMappedBlock(logical, Le32(inode.blockArea + i * 4), extents);
        for (u32 level = 0; level != 3 && logical < limit; ++level)
            WalkIndirect(Le32(inode.blockArea + (12 + level) * 4), level, limit, logical, extents);
        return extents;
    }

    bool ExtImage::EmitContent(const RawInode& inode, u64 size, const WriteSink& sink)
    {
        const std::vector<Extent> extents = MapBlocks(inode);
        constexpr size_t ChunkSize = 1 << 20;
        std::vector<u8> chunk(ChunkSize);

        size_t next = 0;
        u64 position = 0;
        while (position != size)
        {
            const u64 block = position / blockSize_;
            const u32 inBlock = static_cast<u32>(position % blockSize_);
            while (next != extents.size() && u64{extents[next].logical} + extents[next].length <= block)
                ++next;

            u64 available;
            bool mapped = false;
            u64 physicalOffset = 0;
            if (next != extents.size() && u64{extents[next].logical} <= block)
            {
                const Extent& extent = extents[next];
                available = (u64{extent.logical} + extent.length - block) * blockSize_ - inBlock;
                mapped = !extent.unwritten;
                physicalOffset = (extent.physical + (block - extent.logical)) * blockSize_ + inBlock;
            }
            else
            {
                const u64 holeEnd = next != extents.size() ? u64{extents[next].logical} * blockSize_ : size;
                available = (std::min)(holeEnd, size) - position;
            }

            const size_t count = static_cast<size_t>((std::min)({ available, size - position, u64{ChunkSize} }));
            if (mapped)
                file_->ReadAt(physicalOffset, chunk.data(), count);
            else
                std::memset(chunk.data(), 0, count);

            if (!sink(chunk.data(), count))
                return false;
            position += count;
        }
        return true;
    }

    std::string ExtImage::ReadWholeFile(const RawInode& inode)
    {
        if (inode.size > MaxDirectorySize)
            throw FsError("ext metadata file is unreasonably large");
        std::string content;
        content.reserve(static_cast<size_t>(inode.size));
        EmitContent(inode, inode.size, [&](const void* data, size_t size)
        {
            content.append(static_cast<const char*>(data), size);
            return true;
        });
        return content;
    }

    void ExtImage::FillNode(FsNode& node, u32 inum, const RawInode& inode)
    {
        node.kind = KindFromMode(inode.mode);
        node.inum = inum;
        node.mode = inode.mode;
        node.uid = inode.uid;
        node.gid = inode.gid;
        node.nlink = inode.links;
        node.atime = inode.atime;
        node.mtime = inode.mtime;
        node.ctime = inode.ctime;

        if (node.kind == NodeKind::File)
        {
            node.size = inode.size;
        }
        else if (node.kind == NodeKind::Symlink)
        {
            const u32 xattrBlocks = inode.hasXattrBlock ? blockSize_ / 512 : 0;
            const bool isFast = !(inode.flags & InodeFlagExtents) && inode.size < InodeBlockArea && inode.blocks == xattrBlocks;
            if (isFast)
                node.linkTarget.assign(reinterpret_cast<const char*>(inode.blockArea), static_cast<size_t>(inode.size));
            else
                node.linkTarget = ReadWholeFile(inode);
            node.size = node.linkTarget.size();
        }
    }

    void ExtImage::BuildDirectory(FsNode& directory, const RawInode& inode, u32 depth)
    {
        if (depth >= MaxDirDepth)
        {
            warnings_.push_back(L"Directory tree is too deep at " + directory.name);
            return;
        }

        const std::string content = ReadWholeFile(inode);
        std::vector<std::pair<FsNode*, RawInode>> subdirectories;

        for (size_t blockStart = 0; blockStart < content.size(); blockStart += blockSize_)
        {
            const size_t blockEnd = (std::min)(content.size(), blockStart + blockSize_);
            size_t pos = blockStart;
            while (pos + 8 <= blockEnd)
            {
                const u8* entry = reinterpret_cast<const u8*>(content.data()) + pos;
                const u32 childInum = Le32(entry);
                const u32 recordLength = Le16(entry + 4);
                if (recordLength < 8 || (recordLength & 3) != 0 || pos + recordLength > blockEnd)
                    break;

                const u32 nameLength = (incompat_ & IncompatFiletype) ? entry[6] : Le16(entry + 6);
                const size_t entryEnd = pos + recordLength;
                pos = entryEnd;
                if (childInum == 0 || nameLength == 0 || size_t{nameLength} + 8 > recordLength)
                    continue;

                const std::string name(reinterpret_cast<const char*>(entry + 8), nameLength);
                if (name == "." || name == "..")
                    continue;

                RawInode childInode;
                try
                {
                    childInode = ReadInode(childInum);
                }
                catch (const FsError&)
                {
                    warnings_.push_back(L"Directory entry with an invalid inode: " + Widen(name));
                    continue;
                }

                auto child = std::make_unique<FsNode>();
                try
                {
                    FillNode(*child, childInum, childInode);
                }
                catch (const FsError& error)
                {
                    warnings_.push_back(L"Cannot read " + Widen(name) + L": " + Widen(error.what()));
                    continue;
                }

                const NodeKind kind = child->kind;
                FsNode* const added = AddChild(directory, name, std::move(child));
                if (added != nullptr && kind == NodeKind::Dir)
                    subdirectories.emplace_back(added, childInode);
            }
        }

        for (const auto& [subdirectory, subInode] : subdirectories)
        {
            if (!visitedDirectories_.insert(static_cast<u32>(subdirectory->inum)).second)
            {
                warnings_.push_back(L"Directory linked more than once: " + subdirectory->name);
                continue;
            }
            try
            {
                BuildDirectory(*subdirectory, subInode, depth + 1);
            }
            catch (const FsError& error)
            {
                warnings_.push_back(L"Cannot read directory " + subdirectory->name + L": " + Widen(error.what()));
            }
        }
    }

    bool ExtImage::ReadFile(const FsNode& node, const WriteSink& sink)
    {
        return EmitContent(ReadInode(static_cast<u32>(node.inum)), node.size, sink);
    }
}

std::unique_ptr<FsImage> OpenExt(std::unique_ptr<CachedFile> file)
{
    return std::make_unique<ExtImage>(std::move(file));
}

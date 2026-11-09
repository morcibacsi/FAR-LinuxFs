#include "Decompress.hpp"

#include <array>

u32 Crc32(u32 crc, const void* data, size_t size)
{
    static const std::array<u32, 256> table = []
    {
        std::array<u32, 256> t{};
        for (u32 i = 0; i != 256; ++i)
        {
            u32 c = i;
            for (int bit = 0; bit != 8; ++bit)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();

    auto p = static_cast<const u8*>(data);
    while (size-- != 0)
        crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return crc;
}

size_t Lzo1xDecompress(const u8* in, size_t inSize, u8* out, size_t outSize)
{
    const auto corrupt = []() { throw FsError("corrupt LZO stream"); };
    size_t ip = 0;
    size_t op = 0;
    size_t t = 0;
    size_t next = 0;
    size_t state = 0;
    size_t matchPos = 0;

    const auto needIn = [&](size_t n) { if (inSize - ip < n) corrupt(); };
    const auto needOut = [&](size_t n) { if (outSize - op < n) corrupt(); };
    const auto readLengthExtension = [&]()
    {
        // Each zero byte stands for 255 more bytes of length.
        size_t zeros = 0;
        needIn(1);
        while (in[ip] == 0)
        {
            ++ip;
            ++zeros;
            needIn(1);
        }
        return zeros * 255 + in[ip++];
    };
    const auto setBack = [&](size_t distance)
    {
        if (distance > op)
            corrupt();
        matchPos = op - distance;
    };
    const auto copyLiterals = [&](size_t n)
    {
        needIn(n);
        needOut(n);
        if (n != 0)
            std::memcpy(out + op, in + ip, n);
        ip += n;
        op += n;
    };
    const auto copyMatch = [&](size_t n)
    {
        needOut(n);
        // The regions may overlap: a match can repeat bytes it has just produced.
        for (size_t i = 0; i != n; ++i)
            out[op++] = out[matchPos++];
    };

    needIn(1);
    bool trailingLiterals = false;
    if (in[0] > 17)
    {
        t = in[ip++] - size_t{17};
        if (t < 4)
        {
            next = t;
            trailingLiterals = true;
        }
        else
        {
            copyLiterals(t);
            state = 4;
        }
    }

    for (;;)
    {
        if (trailingLiterals)
        {
            trailingLiterals = false;
            state = next;
            copyLiterals(next);
        }

        needIn(1);
        t = in[ip++];
        if (t < 16)
        {
            if (state == 0)
            {
                if (t == 0)
                    t = 15 + readLengthExtension();
                copyLiterals(t + 3);
                state = 4;
                continue;
            }

            next = t & 3;
            needIn(1);
            if (state != 4)
            {
                setBack(1 + (t >> 2) + (size_t{in[ip++]} << 2));
                copyMatch(2);
                trailingLiterals = true;
                continue;
            }

            setBack(1 + 0x800 + (t >> 2) + (size_t{in[ip++]} << 2));
            t = 3;
        }
        else if (t >= 64)
        {
            next = t & 3;
            needIn(1);
            setBack(1 + ((t >> 2) & 7) + (size_t{in[ip++]} << 3));
            t = (t >> 5) + 1;
        }
        else if (t >= 32)
        {
            t = (t & 31) + 2;
            if (t == 2)
                t += 31 + readLengthExtension();
            needIn(2);
            const size_t word = Le16(in + ip);
            ip += 2;
            setBack(1 + (word >> 2));
            next = word & 3;
        }
        else
        {
            const size_t high = (t & 8) << 11;
            t = (t & 7) + 2;
            if (t == 2)
                t += 7 + readLengthExtension();
            needIn(2);
            const size_t word = Le16(in + ip);
            ip += 2;
            next = word & 3;
            const size_t distance = high + (word >> 2);
            if (distance == 0)
            {
                if (t != 3 || ip != inSize)
                    corrupt();
                return op;
            }
            setBack(distance + 0x4000);
        }

        copyMatch(t);
        trailingLiterals = true;
    }
}

namespace
{
    constexpr int MaxBits = 15;

    struct Huffman
    {
        std::array<u16, MaxBits + 1> count{};
        std::array<u16, 288> symbol{};
    };

    class BitReader
    {
    public:
        BitReader(const u8* data, size_t size) : data_(data), size_(size) {}

        unsigned Bits(int need)
        {
            u32 value = bitBuffer_;
            while (bitCount_ < need)
            {
                value |= u32{Byte()} << bitCount_;
                bitCount_ += 8;
            }
            bitBuffer_ = value >> need;
            bitCount_ -= need;
            return value & ((u32{1} << need) - 1);
        }

        void AlignToByte()
        {
            bitBuffer_ = 0;
            bitCount_ = 0;
        }

        u8 Byte()
        {
            if (pos_ == size_)
                throw FsError("truncated deflate stream");
            return data_[pos_++];
        }

    private:
        const u8* data_;
        size_t size_;
        size_t pos_ = 0;
        u32 bitBuffer_ = 0;
        int bitCount_ = 0;
    };

    int DecodeSymbol(BitReader& reader, const Huffman& table)
    {
        int code = 0;
        int first = 0;
        int index = 0;
        for (size_t length = 1; length <= MaxBits; ++length)
        {
            code |= static_cast<int>(reader.Bits(1));
            const int count = table.count[length];
            if (code - count < first)
                return table.symbol[static_cast<size_t>(index + (code - first))];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        throw FsError("invalid deflate code");
    }

    void BuildTable(Huffman& table, const u16* lengths, size_t symbols)
    {
        table.count.fill(0);
        for (size_t s = 0; s != symbols; ++s)
            ++table.count[lengths[s]];

        int left = 1;
        for (size_t length = 1; length <= MaxBits; ++length)
        {
            left = left * 2 - table.count[length];
            if (left < 0)
                throw FsError("over-subscribed deflate table");
        }

        std::array<u16, MaxBits + 1> offsets{};
        for (size_t length = 1; length < MaxBits; ++length)
            offsets[length + 1] = static_cast<u16>(offsets[length] + table.count[length]);
        for (size_t s = 0; s != symbols; ++s)
        {
            if (lengths[s] != 0)
                table.symbol[offsets[lengths[s]]++] = static_cast<u16>(s);
        }
    }

    void InflateCodes(BitReader& reader, const Huffman& lengthCodes, const Huffman& distanceCodes, u8* out, size_t outSize, size_t& op)
    {
        static constexpr u16 lengthBase[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
        static constexpr u8 lengthExtra[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
        static constexpr u16 distanceBase[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
        static constexpr u8 distanceExtra[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

        for (;;)
        {
            int symbol = DecodeSymbol(reader, lengthCodes);
            if (symbol < 256)
            {
                if (op == outSize)
                    throw FsError("deflate output overflow");
                out[op++] = static_cast<u8>(symbol);
                continue;
            }
            if (symbol == 256)
                return;

            symbol -= 257;
            if (symbol >= 29)
                throw FsError("invalid deflate length");
            const size_t length = lengthBase[symbol] + size_t{reader.Bits(lengthExtra[symbol])};

            const int distanceSymbol = DecodeSymbol(reader, distanceCodes);
            if (distanceSymbol >= 30)
                throw FsError("invalid deflate distance");
            const size_t distance = distanceBase[distanceSymbol] + size_t{reader.Bits(distanceExtra[distanceSymbol])};
            if (distance > op || outSize - op < length)
                throw FsError("invalid deflate back-reference");
            for (size_t i = 0; i != length; ++i, ++op)
                out[op] = out[op - distance];
        }
    }

    void InflateStored(BitReader& reader, u8* out, size_t outSize, size_t& op)
    {
        reader.AlignToByte();
        unsigned length = reader.Byte();
        length |= unsigned{reader.Byte()} << 8;
        unsigned complement = reader.Byte();
        complement |= unsigned{reader.Byte()} << 8;
        if ((length ^ complement) != 0xFFFF)
            throw FsError("invalid stored deflate block");
        if (outSize - op < length)
            throw FsError("deflate output overflow");
        while (length-- != 0)
            out[op++] = reader.Byte();
    }

    void InflateFixed(BitReader& reader, u8* out, size_t outSize, size_t& op)
    {
        std::array<u16, 288> lengths;
        size_t s = 0;
        for (; s != 144; ++s) lengths[s] = 8;
        for (; s != 256; ++s) lengths[s] = 9;
        for (; s != 280; ++s) lengths[s] = 7;
        for (; s != 288; ++s) lengths[s] = 8;
        Huffman lengthCodes;
        BuildTable(lengthCodes, lengths.data(), lengths.size());

        std::array<u16, 30> distances;
        distances.fill(5);
        Huffman distanceCodes;
        BuildTable(distanceCodes, distances.data(), distances.size());
        InflateCodes(reader, lengthCodes, distanceCodes, out, outSize, op);
    }

    void InflateDynamic(BitReader& reader, u8* out, size_t outSize, size_t& op)
    {
        static constexpr u8 order[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
        const size_t literalCount = reader.Bits(5) + size_t{257};
        const size_t distanceCount = reader.Bits(5) + size_t{1};
        const size_t codeCount = reader.Bits(4) + size_t{4};
        if (literalCount > 286 || distanceCount > 30)
            throw FsError("invalid deflate header");

        std::array<u16, 320> lengths{};
        for (size_t i = 0; i != codeCount; ++i)
            lengths[order[i]] = static_cast<u16>(reader.Bits(3));
        Huffman codeLengthCodes;
        BuildTable(codeLengthCodes, lengths.data(), 19);

        lengths.fill(0);
        size_t index = 0;
        while (index < literalCount + distanceCount)
        {
            const int symbol = DecodeSymbol(reader, codeLengthCodes);
            if (symbol < 16)
            {
                lengths[index++] = static_cast<u16>(symbol);
                continue;
            }

            u16 value = 0;
            size_t repeat;
            if (symbol == 16)
            {
                if (index == 0)
                    throw FsError("invalid deflate repeat");
                value = lengths[index - 1];
                repeat = reader.Bits(2) + size_t{3};
            }
            else if (symbol == 17)
            {
                repeat = reader.Bits(3) + size_t{3};
            }
            else
            {
                repeat = reader.Bits(7) + size_t{11};
            }
            if (index + repeat > literalCount + distanceCount)
                throw FsError("invalid deflate repeat");
            while (repeat-- != 0)
                lengths[index++] = value;
        }

        Huffman lengthCodes;
        BuildTable(lengthCodes, lengths.data(), literalCount);
        Huffman distanceCodes;
        BuildTable(distanceCodes, lengths.data() + literalCount, distanceCount);
        InflateCodes(reader, lengthCodes, distanceCodes, out, outSize, op);
    }
}

size_t InflateRaw(const u8* in, size_t inSize, u8* out, size_t outSize)
{
    BitReader reader(in, inSize);
    size_t op = 0;
    bool last = false;
    while (!last)
    {
        last = reader.Bits(1) != 0;
        switch (reader.Bits(2))
        {
        case 0: InflateStored(reader, out, outSize, op); break;
        case 1: InflateFixed(reader, out, outSize, op); break;
        case 2: InflateDynamic(reader, out, outSize, op); break;
        default: throw FsError("invalid deflate block type");
        }
    }
    return op;
}

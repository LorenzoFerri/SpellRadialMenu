#include "render/assets/dcx.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace radial_spell_menu::dcx {
namespace {

using OodleDecompressFn = long long(__stdcall *)(
    const void*, long long, void*, long long, int, int, int, void*, long long, void*, void*, void*, long long, int);

std::uint32_t ReadBe32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return (std::uint32_t(bytes[offset]) << 24) |
           (std::uint32_t(bytes[offset + 1]) << 16) |
           (std::uint32_t(bytes[offset + 2]) << 8) |
           std::uint32_t(bytes[offset + 3]);
}

bool HasBytes(const std::vector<std::uint8_t>& bytes, std::size_t offset, const char* text)
{
    for (std::size_t i = 0; text[i] != '\0'; ++i) {
        if (offset + i >= bytes.size() || bytes[offset + i] != static_cast<std::uint8_t>(text[i])) return false;
    }
    return true;
}

bool DecompressKrak(const std::vector<std::uint8_t>& dcx, std::vector<std::uint8_t>& out)
{
    out.clear();
    if (dcx.size() < 0x4c || !HasBytes(dcx, 0, "DCX\0") || !HasBytes(dcx, 0x28, "KRAK")) return false;

    const std::uint32_t raw_size = ReadBe32(dcx, 0x1c);
    const std::uint32_t comp_size = ReadBe32(dcx, 0x20);
    if (0x4cull + comp_size > dcx.size()) return false;

    HMODULE oodle = LoadLibraryA("oo2core_6_win64.dll");
    if (!oodle) return false;
    auto decompress = reinterpret_cast<OodleDecompressFn>(GetProcAddress(oodle, "OodleLZ_Decompress"));
    if (!decompress) return false;

    out.resize(raw_size);
    const long long written = decompress(dcx.data() + 0x4c, comp_size, out.data(), raw_size, 1, 0, 0, nullptr, 0, nullptr, nullptr, nullptr, 0, 3);
    if (written != raw_size) {
        out.clear();
        return false;
    }
    return true;
}

struct BitReader {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::size_t bit_pos = 0;

    bool ReadBits(int count, std::uint32_t& value)
    {
        value = 0;
        if (count < 0 || bit_pos + static_cast<std::size_t>(count) > size * 8ull) return false;
        for (int i = 0; i < count; ++i) {
            value |= ((data[bit_pos >> 3] >> (bit_pos & 7)) & 1u) << i;
            ++bit_pos;
        }
        return true;
    }

    bool AlignByte()
    {
        bit_pos = (bit_pos + 7u) & ~7ull;
        return bit_pos <= size * 8ull;
    }
};

std::uint32_t ReverseBits(std::uint32_t code, int length)
{
    std::uint32_t reversed = 0;
    for (int i = 0; i < length; ++i) {
        reversed = (reversed << 1) | (code & 1u);
        code >>= 1;
    }
    return reversed;
}

struct HuffmanEntry {
    std::uint16_t symbol = 0;
    std::uint8_t bits = 0;
};

struct HuffmanTable {
    int max_bits = 0;
    std::vector<HuffmanEntry> entries;

    bool Build(const std::vector<std::uint8_t>& lengths, int max_allowed_bits)
    {
        max_bits = max_allowed_bits;
        entries.assign(1u << max_bits, {});

        std::vector<std::uint16_t> count(static_cast<std::size_t>(max_bits + 1), 0);
        for (std::uint8_t length : lengths) {
            if (length > max_bits) return false;
            if (length != 0) ++count[length];
        }

        std::vector<std::uint16_t> next_code(static_cast<std::size_t>(max_bits + 1), 0);
        std::uint32_t code = 0;
        for (int bits = 1; bits <= max_bits; ++bits) {
            code = (code + count[bits - 1]) << 1;
            next_code[bits] = static_cast<std::uint16_t>(code);
        }

        for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol) {
            const int length = lengths[symbol];
            if (length == 0) continue;

            const std::uint32_t canonical = next_code[length]++;
            const std::uint32_t reversed = ReverseBits(canonical, length);
            const std::uint32_t stride = 1u << length;
            for (std::uint32_t i = reversed; i < entries.size(); i += stride) {
                entries[i] = {static_cast<std::uint16_t>(symbol), static_cast<std::uint8_t>(length)};
            }
        }

        return true;
    }

    bool Decode(BitReader& reader, std::uint16_t& symbol) const
    {
        std::uint32_t index = 0;
        const std::size_t start = reader.bit_pos;
        for (int bit = 0; bit < max_bits; ++bit) {
            std::uint32_t value = 0;
            if (!reader.ReadBits(1, value)) return false;
            index |= value << bit;
            const HuffmanEntry& entry = entries[index];
            if (entry.bits == bit + 1) {
                symbol = entry.symbol;
                return true;
            }
        }
        reader.bit_pos = start;
        return false;
    }
};

bool BuildFixedTables(HuffmanTable& litlen, HuffmanTable& dist)
{
    std::vector<std::uint8_t> lit_lengths(288, 0);
    for (int i = 0; i <= 143; ++i) lit_lengths[i] = 8;
    for (int i = 144; i <= 255; ++i) lit_lengths[i] = 9;
    for (int i = 256; i <= 279; ++i) lit_lengths[i] = 7;
    for (int i = 280; i <= 287; ++i) lit_lengths[i] = 8;

    std::vector<std::uint8_t> dist_lengths(32, 5);
    return litlen.Build(lit_lengths, 15) && dist.Build(dist_lengths, 15);
}

bool BuildDynamicTables(BitReader& reader, HuffmanTable& litlen, HuffmanTable& dist)
{
    std::uint32_t hlit = 0, hdist = 0, hclen = 0;
    if (!reader.ReadBits(5, hlit) || !reader.ReadBits(5, hdist) || !reader.ReadBits(4, hclen)) return false;
    hlit += 257;
    hdist += 1;
    hclen += 4;

    static constexpr int kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    std::vector<std::uint8_t> code_lengths(19, 0);
    for (std::uint32_t i = 0; i < hclen; ++i) {
        std::uint32_t length = 0;
        if (!reader.ReadBits(3, length)) return false;
        code_lengths[kOrder[i]] = static_cast<std::uint8_t>(length);
    }

    HuffmanTable code_table;
    if (!code_table.Build(code_lengths, 7)) return false;

    std::vector<std::uint8_t> lengths(static_cast<std::size_t>(hlit + hdist), 0);
    for (std::size_t i = 0; i < lengths.size();) {
        std::uint16_t symbol = 0;
        if (!code_table.Decode(reader, symbol)) return false;
        if (symbol <= 15) {
            lengths[i++] = static_cast<std::uint8_t>(symbol);
        } else if (symbol == 16) {
            if (i == 0) return false;
            std::uint32_t repeat = 0;
            if (!reader.ReadBits(2, repeat)) return false;
            repeat += 3;
            const std::uint8_t previous = lengths[i - 1];
            while (repeat-- > 0 && i < lengths.size()) lengths[i++] = previous;
        } else if (symbol == 17) {
            std::uint32_t repeat = 0;
            if (!reader.ReadBits(3, repeat)) return false;
            repeat += 3;
            while (repeat-- > 0 && i < lengths.size()) lengths[i++] = 0;
        } else if (symbol == 18) {
            std::uint32_t repeat = 0;
            if (!reader.ReadBits(7, repeat)) return false;
            repeat += 11;
            while (repeat-- > 0 && i < lengths.size()) lengths[i++] = 0;
        } else {
            return false;
        }
    }

    std::vector<std::uint8_t> lit_lengths(lengths.begin(), lengths.begin() + hlit);
    std::vector<std::uint8_t> dist_lengths(lengths.begin() + hlit, lengths.end());
    return litlen.Build(lit_lengths, 15) && dist.Build(dist_lengths, 15);
}

bool InflateBlock(BitReader& reader, const HuffmanTable& litlen, const HuffmanTable& dist, std::vector<std::uint8_t>& out)
{
    static constexpr int kLengthBase[29] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258,
    };
    static constexpr int kLengthExtra[29] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
    };
    static constexpr int kDistBase[30] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
        193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
        6145, 8193, 12289, 16385, 24577,
    };
    static constexpr int kDistExtra[30] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
        6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13,
    };

    for (;;) {
        std::uint16_t symbol = 0;
        if (!litlen.Decode(reader, symbol)) return false;
        if (symbol < 256) {
            out.push_back(static_cast<std::uint8_t>(symbol));
        } else if (symbol == 256) {
            return true;
        } else if (symbol <= 285) {
            const int length_index = symbol - 257;
            std::uint32_t extra = 0;
            if (!reader.ReadBits(kLengthExtra[length_index], extra)) return false;
            const std::size_t length = static_cast<std::size_t>(kLengthBase[length_index] + extra);

            std::uint16_t dist_symbol = 0;
            if (!dist.Decode(reader, dist_symbol) || dist_symbol >= 30) return false;
            extra = 0;
            if (!reader.ReadBits(kDistExtra[dist_symbol], extra)) return false;
            const std::size_t distance = static_cast<std::size_t>(kDistBase[dist_symbol] + extra);
            if (distance == 0 || distance > out.size()) return false;

            for (std::size_t i = 0; i < length; ++i) {
                out.push_back(out[out.size() - distance]);
            }
        } else {
            return false;
        }
    }
}

bool InflateZlib(const std::uint8_t* data, std::size_t size, std::uint32_t raw_size, std::vector<std::uint8_t>& out)
{
    out.clear();
    if (!data || size < 6) return false;
    if ((data[0] & 0x0f) != 8) return false;
    if ((((std::uint32_t)data[0] << 8) + data[1]) % 31u != 0) return false;

    BitReader reader{data + 2, size - 6, 0};
    bool final_block = false;
    while (!final_block) {
        std::uint32_t final = 0, type = 0;
        if (!reader.ReadBits(1, final) || !reader.ReadBits(2, type)) return false;
        final_block = final != 0;

        if (type == 0) {
            if (!reader.AlignByte()) return false;
            if (reader.bit_pos / 8 + 4 > reader.size) return false;
            const std::size_t pos = reader.bit_pos / 8;
            const std::uint16_t len = std::uint16_t(reader.data[pos]) | (std::uint16_t(reader.data[pos + 1]) << 8);
            const std::uint16_t nlen = std::uint16_t(reader.data[pos + 2]) | (std::uint16_t(reader.data[pos + 3]) << 8);
            if ((len ^ 0xffffu) != nlen || pos + 4ull + len > reader.size) return false;
            out.insert(out.end(), reader.data + pos + 4, reader.data + pos + 4 + len);
            reader.bit_pos = (pos + 4ull + len) * 8ull;
        } else if (type == 1 || type == 2) {
            HuffmanTable litlen;
            HuffmanTable dist;
            const bool built = type == 1 ? BuildFixedTables(litlen, dist) : BuildDynamicTables(reader, litlen, dist);
            if (!built || !InflateBlock(reader, litlen, dist, out)) return false;
        } else {
            return false;
        }

        if (out.size() > raw_size) return false;
    }

    return out.size() == raw_size;
}

bool DecompressDflt(const std::vector<std::uint8_t>& dcx, std::vector<std::uint8_t>& out, const char** error)
{
    if (error) *error = "";
    out.clear();
    if (dcx.size() < 0x52 || !HasBytes(dcx, 0, "DCX\0") || !HasBytes(dcx, 0x28, "DFLT")) {
        if (error) *error = "not_dflt";
        return false;
    }

    const std::uint32_t raw_size = ReadBe32(dcx, 0x1c);
    const std::uint32_t comp_size = ReadBe32(dcx, 0x20);
    if (comp_size <= 6 || 0x4cull + comp_size > dcx.size()) {
        if (error) *error = "bad_size";
        return false;
    }

    if (!InflateZlib(dcx.data() + 0x4c, comp_size, raw_size, out)) {
        if (error) *error = "inflate_failed";
        return false;
    }
    return true;
}

}  // namespace

bool StartsWithDcx(const std::vector<std::uint8_t>& bytes)
{
    return bytes.size() >= 4 && HasBytes(bytes, 0, "DCX\0");
}

bool Decompress(const std::vector<std::uint8_t>& dcx, std::vector<std::uint8_t>& out, const char** dflt_error)
{
    if (dflt_error) *dflt_error = "";
    return DecompressKrak(dcx, out) || DecompressDflt(dcx, out, dflt_error);
}

}  // namespace radial_spell_menu::dcx

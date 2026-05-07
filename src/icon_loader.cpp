#include "icon_loader.h"

#include "asset_reader.h"
#include "common.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <windows.h>
#include <bcrypt.h>

namespace radial_spell_menu::icon_loader {
namespace {

struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

struct IconEntry {
    std::size_t atlas_index = 0;
    Rect rect{};
};

struct Atlas {
    std::string name;
    ID3D12Resource* texture = nullptr;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_srv{};
    float width = 1.0f;
    float height = 1.0f;
};

using OodleDecompressFn = long long(__stdcall *)(
    const void*, long long, void*, long long, int, int, int, void*, long long, void*, void*, void*, long long, int);

std::array<Atlas, kMaxAtlases> g_atlases = {};
std::size_t g_atlas_count = 0;
std::unordered_map<std::uint32_t, IconEntry> g_icons;
bool g_initialized = false;
bool g_failed = false;
const char* g_last_dflt_error = "";

std::uint32_t ReadLe32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return std::uint32_t(bytes[offset]) |
           (std::uint32_t(bytes[offset + 1]) << 8) |
           (std::uint32_t(bytes[offset + 2]) << 16) |
           (std::uint32_t(bytes[offset + 3]) << 24);
}

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

bool DecompressDcxKrak(const std::vector<std::uint8_t>& dcx, std::vector<std::uint8_t>& out)
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

bool DecompressDcxDflt(const std::vector<std::uint8_t>& dcx, std::vector<std::uint8_t>& out)
{
    g_last_dflt_error = "";
    out.clear();
    if (dcx.size() < 0x52 || !HasBytes(dcx, 0, "DCX\0") || !HasBytes(dcx, 0x28, "DFLT")) {
        g_last_dflt_error = "not_dflt";
        return false;
    }

    const std::uint32_t raw_size = ReadBe32(dcx, 0x1c);
    const std::uint32_t comp_size = ReadBe32(dcx, 0x20);
    if (comp_size <= 6 || 0x4cull + comp_size > dcx.size()) {
        g_last_dflt_error = "bad_size";
        return false;
    }

    if (!InflateZlib(dcx.data() + 0x4c, comp_size, raw_size, out)) {
        g_last_dflt_error = "inflate_failed";
        return false;
    }
    return true;
}

bool DecompressDcx(const std::vector<std::uint8_t>& dcx, std::vector<std::uint8_t>& out)
{
    return DecompressDcxKrak(dcx, out) || DecompressDcxDflt(dcx, out);
}

bool DecryptData0AesRanges(std::vector<std::uint8_t>& bytes)
{
    // Data0.bhd marks only selected byte ranges as AES-encrypted for this entry.
    static constexpr std::uint8_t kKey[16] = {
        0x01, 0xed, 0xbd, 0xd5, 0xd1, 0xae, 0x25, 0xcc,
        0xc5, 0x53, 0xc0, 0xcb, 0xff, 0x79, 0x76, 0x5f,
    };
    static constexpr std::pair<std::uint32_t, std::uint32_t> kRanges[] = {
        {0, 1024},
        {2048, 6144},
        {1048576, 1049600},
    };

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) return false;
    if (BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_ECB)), sizeof(BCRYPT_CHAIN_MODE_ECB), 0) != 0 ||
        BCryptGenerateSymmetricKey(algorithm, &key, nullptr, 0, const_cast<PUCHAR>(kKey), sizeof(kKey), 0) != 0) {
        if (key) BCryptDestroyKey(key);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }

    bool ok = true;
    for (const auto& [begin, end] : kRanges) {
        if (begin >= bytes.size()) continue;
        const std::uint32_t clamped_end = std::min<std::uint32_t>(end, static_cast<std::uint32_t>(bytes.size()));
        const std::uint32_t size = clamped_end - begin;
        if (size == 0 || (size % 16) != 0) {
            ok = false;
            break;
        }
        ULONG written = 0;
        if (BCryptDecrypt(key, bytes.data() + begin, size, nullptr, nullptr, 0, bytes.data() + begin, size, &written, 0) != 0 || written != size) {
            ok = false;
            break;
        }
    }

    BCryptDestroyKey(key);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

bool StartsWithDcx(const std::vector<std::uint8_t>& bytes)
{
    return bytes.size() >= 4 && HasBytes(bytes, 0, "DCX\0");
}

std::string ReadTpfName(const std::vector<std::uint8_t>& tpf, std::uint32_t offset, std::uint8_t encoding)
{
    std::string name;
    if (offset >= tpf.size()) return name;

    if (encoding == 1) {
        for (std::size_t i = offset; i + 1 < tpf.size(); i += 2) {
            const std::uint16_t c = std::uint16_t(tpf[i]) | (std::uint16_t(tpf[i + 1]) << 8);
            if (c == 0) break;
            name.push_back(c >= 32 && c <= 126 ? static_cast<char>(c) : '?');
        }
    } else {
        for (std::size_t i = offset; i < tpf.size() && tpf[i] != 0; ++i) {
            name.push_back(tpf[i] >= 32 && tpf[i] <= 126 ? static_cast<char>(tpf[i]) : '?');
        }
    }

    return name;
}

std::string StripExtension(std::string name)
{
    const std::size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name.erase(0, slash + 1);
    const std::size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) name.erase(dot);
    return name;
}

bool ExtractTpfTexture(const std::vector<std::uint8_t>& tpf, const std::string& target_name, std::vector<std::uint8_t>& dds)
{
    dds.clear();
    if (tpf.size() < 0x10 || !HasBytes(tpf, 0, "TPF\0")) return false;

    const std::uint32_t count = ReadLe32(tpf, 8);
    const std::uint8_t platform = tpf[0x0c];
    const std::uint8_t encoding = tpf[0x0e];
    constexpr std::size_t kPcTextureHeaderSize = 0x14;
    if (platform != 0 || count > 512 || 0x10ull + count * kPcTextureHeaderSize > tpf.size()) return false;

    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t entry = 0x10ull + i * kPcTextureHeaderSize;
        const std::uint32_t file_offset = ReadLe32(tpf, entry);
        const std::uint32_t file_size = ReadLe32(tpf, entry + 4);
        const std::uint32_t name_offset = ReadLe32(tpf, entry + 12);
        if (file_offset > tpf.size() || file_size > tpf.size() - file_offset) continue;
        if (StripExtension(ReadTpfName(tpf, name_offset, encoding)) != target_name) continue;

        dds.assign(tpf.begin() + file_offset, tpf.begin() + file_offset + file_size);
        return true;
    }

    return false;
}

int ReadXmlInt(const std::string& text, std::size_t line_start, const char* attr)
{
    const std::string needle = std::string(attr) + "=\"";
    const std::size_t pos = text.find(needle, line_start);
    if (pos == std::string::npos) return 0;
    return std::atoi(text.c_str() + pos + needle.size());
}

std::uint32_t ParseIconId(const std::string& text, std::size_t name_pos)
{
    constexpr std::size_t kIconPrefixLen = 14; // MENU_ItemIcon_
    std::size_t pos = name_pos + kIconPrefixLen;
    std::uint32_t id = 0;
    bool saw_digit = false;

    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
        saw_digit = true;
        id = (id * 10u) + static_cast<std::uint32_t>(text[pos] - '0');
        ++pos;
    }

    // Ignore disabled/log variants like MENU_ItemIcon_06000d.png.
    if (!saw_digit || pos >= text.size() || text[pos] != '.') return 0;
    return id;
}

std::size_t FindOrAddAtlas(std::string name)
{
    for (std::size_t i = 0; i < g_atlas_count; ++i) {
        if (g_atlases[i].name == name) return i;
    }
    if (g_atlas_count >= g_atlases.size()) return g_atlases.size();

    const std::size_t index = g_atlas_count++;
    g_atlases[index].name = std::move(name);
    return index;
}

void ParseLayouts(const std::vector<std::uint8_t>& bnd)
{
    const std::string text(reinterpret_cast<const char*>(bnd.data()), bnd.size());
    constexpr const char* kAtlasMarker = "<TextureAtlas imagePath=\"";
    std::size_t atlas = 0;
    while ((atlas = text.find(kAtlasMarker, atlas)) != std::string::npos) {
        const std::size_t image_begin = atlas + std::strlen(kAtlasMarker);
        const std::size_t image_end = text.find('"', image_begin);
        if (image_end == std::string::npos) break;

        const std::size_t atlas_end = text.find("</TextureAtlas>", atlas);
        if (atlas_end == std::string::npos) break;

        const std::string atlas_name = StripExtension(text.substr(image_begin, image_end - image_begin));
        std::size_t atlas_index = g_atlases.size();

        std::size_t pos = atlas;
        while ((pos = text.find("<SubTexture", pos)) != std::string::npos && pos < atlas_end) {
            const std::size_t name = text.find("MENU_ItemIcon_", pos);
            if (name == std::string::npos || name > atlas_end) break;
            const std::uint32_t id = ParseIconId(text, name);
            Rect rect{};
            rect.x = static_cast<float>(ReadXmlInt(text, pos, "x"));
            rect.y = static_cast<float>(ReadXmlInt(text, pos, "y"));
            rect.w = static_cast<float>(ReadXmlInt(text, pos, "width"));
            rect.h = static_cast<float>(ReadXmlInt(text, pos, "height"));
            if (id != 0 && rect.w > 0.0f && rect.h > 0.0f) {
                if (atlas_index >= g_atlases.size()) atlas_index = FindOrAddAtlas(atlas_name);
                if (atlas_index < g_atlases.size()) g_icons[id] = {atlas_index, rect};
            }
            pos += 11;
        }

        atlas = atlas_end + 15;
    }
}

bool UploadBc7Texture(
    ID3D12Device* device,
    ID3D12CommandQueue* queue,
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_srv,
    Atlas& atlas,
    const std::vector<std::uint8_t>& dds)
{
    if (dds.size() < 148 || !HasBytes(dds, 0, "DDS ") || !HasBytes(dds, 84, "DX10")) return false;

    const std::uint32_t height = ReadLe32(dds, 12);
    const std::uint32_t width = ReadLe32(dds, 16);
    const std::uint32_t format = ReadLe32(dds, 128);
    if (width == 0 || height == 0 || format != DXGI_FORMAT_BC7_UNORM) return false;

    const std::size_t data_offset = 148;
    if (dds.size() <= data_offset) return false;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_BC7_UNORM;
    desc.SampleDesc.Count = 1;

    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&atlas.texture)))) {
        return false;
    }

    const std::uint64_t row_pitch = ((width + 3ull) / 4ull) * 16ull;
    const std::uint64_t upload_size = row_pitch * ((height + 3ull) / 4ull);
    if (data_offset + upload_size > dds.size()) return false;

    D3D12_HEAP_PROPERTIES upload_heap{};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC upload_desc{};
    upload_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    upload_desc.Width = upload_size;
    upload_desc.Height = 1;
    upload_desc.DepthOrArraySize = 1;
    upload_desc.MipLevels = 1;
    upload_desc.SampleDesc.Count = 1;
    upload_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* upload = nullptr;
    if (FAILED(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)))) return false;

    void* mapped = nullptr;
    upload->Map(0, nullptr, &mapped);
    std::memcpy(mapped, dds.data() + data_offset, static_cast<std::size_t>(upload_size));
    upload->Unmap(0, nullptr);

    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list)))) {
        SafeRelease(upload);
        return false;
    }

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = atlas.texture;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = upload;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_BC7_UNORM;
    src.PlacedFootprint.Footprint.Width = width;
    src.PlacedFootprint.Footprint.Height = height;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(row_pitch);
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = atlas.texture;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &barrier);
    list->Close();

    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);

    ID3D12Fence* fence = nullptr;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    queue->Signal(fence, 1);
    if (fence->GetCompletedValue() < 1) {
        fence->SetEventOnCompletion(1, event);
        const DWORD wait = WaitForSingleObject(event, 2000);
        if (wait != WAIT_OBJECT_0) {
            Log("Icon loader: timed out waiting for texture upload fence.");
            CloseHandle(event);
            SafeRelease(fence);
            SafeRelease(list);
            SafeRelease(allocator);
            SafeRelease(upload);
            return false;
        }
    }
    CloseHandle(event);
    SafeRelease(fence);
    SafeRelease(list);
    SafeRelease(allocator);
    SafeRelease(upload);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_BC7_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(atlas.texture, &srv, cpu_srv);

    atlas.width = static_cast<float>(width);
    atlas.height = static_cast<float>(height);
    return true;
}

}  // namespace

bool TryInitialize(
    ID3D12Device* device,
    ID3D12CommandQueue* queue,
    const D3D12_CPU_DESCRIPTOR_HANDLE* cpu_srvs,
    const D3D12_GPU_DESCRIPTOR_HANDLE* gpu_srvs,
    std::size_t srv_count)
{
    if (g_initialized) return true;
    if (g_failed || !device || !queue || !cpu_srvs || !gpu_srvs || srv_count < g_atlases.size()) return false;

    struct Candidate {
        const wchar_t* tpf_path;
        const wchar_t* layout_path;
        bool decrypt_tpf;
        const char* label;
    };
    constexpr Candidate candidates[] = {
        {L"data0:/menu/low/01_common.tpf.dcx", L"data0:/menu/low/01_common.sblytbnd.dcx", true, "low"},
        {L"data0:/menu/lo/01_common.tpf.dcx", L"data0:/menu/lo/01_common.sblytbnd.dcx", false, "lo"},
        {L"data0:/menu/hi/01_common.tpf.dcx", L"data0:/menu/hi/01_common.sblytbnd.dcx", true, "hi"},
    };

    bool saw_assets = false;
    bool uploaded_any = false;
    const char* selected_label = nullptr;
    std::size_t selected_uploaded_count = 0;
    for (const Candidate& candidate : candidates) {
        std::vector<std::uint8_t> tpf_dcx;
        std::vector<std::uint8_t> sblyt_dcx;
        if (!asset_reader::ReadFile(candidate.tpf_path, tpf_dcx, 160ull * 1024ull * 1024ull) ||
            !asset_reader::ReadFile(candidate.layout_path, sblyt_dcx, 4ull * 1024ull * 1024ull)) {
            continue;
        }

        saw_assets = true;
        if (!StartsWithDcx(tpf_dcx)) {
            if (!candidate.decrypt_tpf || !DecryptData0AesRanges(tpf_dcx)) {
                Log("Icon loader: %s TPF is encrypted or invalid.", candidate.label);
                continue;
            }
        }

        std::vector<std::uint8_t> tpf;
        std::vector<std::uint8_t> sblyt;
        if (!DecompressDcx(tpf_dcx, tpf) || !DecompressDcx(sblyt_dcx, sblyt)) {
            Log("Icon loader: failed to decompress %s icon assets (dflt=%s).",
                candidate.label,
                g_last_dflt_error);
            continue;
        }

        g_icons.clear();
        g_atlas_count = 0;
        ParseLayouts(sblyt);
        std::size_t uploaded_count = 0;
        for (std::size_t i = 0; i < g_atlas_count; ++i) {
            std::vector<std::uint8_t> dds;
            if (!ExtractTpfTexture(tpf, g_atlases[i].name, dds)) continue;
            if (UploadBc7Texture(device, queue, cpu_srvs[i], g_atlases[i], dds)) {
                g_atlases[i].gpu_srv = gpu_srvs[i];
                uploaded_any = true;
                ++uploaded_count;
            }
        }
        selected_label = candidate.label;
        selected_uploaded_count = uploaded_count;
        break;
    }

    if (!saw_assets) return false;
    if (g_icons.empty() || !uploaded_any) {
        Log("Icon loader: failed to upload atlases (icons=%zu uploaded=%d).", g_icons.size(), static_cast<int>(uploaded_any));
        g_failed = true;
        return false;
    }

    g_initialized = true;
    Log("Icon loader ready: source=%s icons=%zu atlases=%zu uploaded_atlases=%zu.",
        selected_label ? selected_label : "unknown",
        g_icons.size(),
        g_atlas_count,
        selected_uploaded_count);
    return true;
}

radial_menu::IconTextureInfo Resolve(std::uint32_t icon_id)
{
    if (!g_initialized) return {};

    auto it = g_icons.find(icon_id);
    std::uint32_t resolved_icon_id = icon_id;
    if (it == g_icons.end()) {
        resolved_icon_id = icon_id + 2000u;
        it = g_icons.find(resolved_icon_id);
    }
    if (it == g_icons.end()) return {};

    const IconEntry& entry = it->second;
    if (entry.atlas_index >= g_atlas_count) return {};
    const Atlas& atlas = g_atlases[entry.atlas_index];
    if (!atlas.gpu_srv.ptr) return {};
    const Rect& r = entry.rect;
    return {(ImTextureID)atlas.gpu_srv.ptr, {r.x / atlas.width, r.y / atlas.height}, {(r.x + r.w) / atlas.width, (r.y + r.h) / atlas.height}};
}

void Shutdown()
{
    for (Atlas& atlas : g_atlases) {
        SafeRelease(atlas.texture);
        atlas.name.clear();
        atlas.gpu_srv = {};
        atlas.width = 1.0f;
        atlas.height = 1.0f;
    }
    g_icons.clear();
    g_atlas_count = 0;
    g_initialized = false;
    g_failed = false;
}

}  // namespace radial_spell_menu::icon_loader

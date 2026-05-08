#include "render/assets/icon_assets.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace radial_menu_mod::icon_assets {
namespace {

std::uint32_t ReadLe32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return std::uint32_t(bytes[offset]) |
           (std::uint32_t(bytes[offset + 1]) << 8) |
           (std::uint32_t(bytes[offset + 2]) << 16) |
           (std::uint32_t(bytes[offset + 3]) << 24);
}

bool HasBytes(const std::vector<std::uint8_t>& bytes, std::size_t offset, const char* text)
{
    for (std::size_t i = 0; text[i] != '\0'; ++i) {
        if (offset + i >= bytes.size() || bytes[offset + i] != static_cast<std::uint8_t>(text[i])) return false;
    }
    return true;
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

int ReadXmlInt(const std::string& text, std::size_t line_start, const char* attr)
{
    const std::string needle = std::string(attr) + "=\"";
    const std::size_t pos = text.find(needle, line_start);
    if (pos == std::string::npos) return 0;
    return std::atoi(text.c_str() + pos + needle.size());
}

std::uint32_t ParseIconId(const std::string& text, std::size_t name_pos)
{
    constexpr std::size_t kIconPrefixLen = 14;
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

}  // namespace

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

std::vector<LayoutIcon> ParseLayoutIcons(const std::vector<std::uint8_t>& bnd)
{
    std::vector<LayoutIcon> icons;
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
        std::size_t pos = atlas;
        while ((pos = text.find("<SubTexture", pos)) != std::string::npos && pos < atlas_end) {
            const std::size_t name = text.find("MENU_ItemIcon_", pos);
            if (name == std::string::npos || name > atlas_end) break;

            Rect rect{};
            rect.x = static_cast<float>(ReadXmlInt(text, pos, "x"));
            rect.y = static_cast<float>(ReadXmlInt(text, pos, "y"));
            rect.w = static_cast<float>(ReadXmlInt(text, pos, "width"));
            rect.h = static_cast<float>(ReadXmlInt(text, pos, "height"));

            const std::uint32_t id = ParseIconId(text, name);
            if (id != 0 && rect.w > 0.0f && rect.h > 0.0f) {
                icons.push_back({id, atlas_name, rect});
            }
            pos += 11;
        }

        atlas = atlas_end + 15;
    }
    return icons;
}

}  // namespace radial_menu_mod::icon_assets

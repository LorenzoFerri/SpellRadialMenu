#include "game/metadata/seamless_coop_metadata.h"

#include <windows.h>

#include <cstring>

namespace radial_spell_menu::seamless_coop_metadata {
namespace {

std::size_t GetImageSize(HMODULE module)
{
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    if (!base) return 0;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    return nt->OptionalHeader.SizeOfImage;
}

template <typename T>
T ReadUnaligned(const std::uint8_t* ptr)
{
    T value{};
    std::memcpy(&value, ptr, sizeof(value));
    return value;
}

}  // namespace

std::uint32_t ResolveIconId(std::uint32_t item_id)
{
    HMODULE module = GetModuleHandleA("ersc.dll");
    if (!module) return 0;

    const auto* bytes = reinterpret_cast<const std::uint8_t*>(module);
    const std::size_t size = GetImageSize(module);
    if (size < sizeof(std::uint32_t)) return 0;

    // Seamless constructs item-like records in its DLL; extract the icon write near the item ID registration.
    constexpr std::uint8_t kIconWritePattern[] = {0x66, 0xC7, 0x40, 0x30};
    for (std::size_t i = 0; i + sizeof(std::uint32_t) <= size; ++i) {
        if (ReadUnaligned<std::uint32_t>(bytes + i) != item_id) continue;

        const std::size_t start = i >= 128 ? i - 128 : 0;
        for (std::size_t pos = i; pos-- > start;) {
            if (pos + sizeof(kIconWritePattern) + sizeof(std::uint16_t) > size) continue;
            if (std::memcmp(bytes + pos, kIconWritePattern, sizeof(kIconWritePattern)) != 0) continue;

            const auto icon_id = ReadUnaligned<std::uint16_t>(bytes + pos + sizeof(kIconWritePattern));
            if (icon_id != 0) return static_cast<std::uint32_t>(icon_id);
        }
    }

    return 0;
}

}  // namespace radial_spell_menu::seamless_coop_metadata

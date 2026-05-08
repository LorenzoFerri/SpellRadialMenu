#include "game/params/param_repository.h"

#include "core/common.h"

#include <array>
#include <string>

namespace radial_spell_menu::param_repository {
namespace {

constexpr std::array<std::uint8_t, 24> kSoloParamRepositoryPattern = {
    0x48, 0x8B, 0x0D, 0x00, 0x00, 0x00, 0x00,
    0x48, 0x85, 0xC9, 0x0F, 0x84, 0x00, 0x00, 0x00, 0x00,
    0x45, 0x33, 0xC0, 0xBA, 0x8E, 0x00, 0x00, 0x00,
};
constexpr std::array<bool, 24> kSoloParamRepositoryMask = {
    true, true, true, false, false, false, false,
    true, true, true, true, true, false, false, false, false,
    true, true, true, true, true, true, true, true,
};

constexpr std::uintptr_t kParamContainerStep1 = 0x80;
constexpr std::uintptr_t kParamContainerStep2 = 0x80;
constexpr std::uintptr_t kParamTypeNameOffset = 0x10;
constexpr std::uintptr_t kParamRowCountOffset = 0x0A;
constexpr std::uintptr_t kParamRowTableStart = 0x40;
constexpr std::uintptr_t kParamRowStride = 0x18;
constexpr std::uintptr_t kRowDescIdOffset = 0x00;
constexpr std::uintptr_t kRowDescDataOffset = 0x08;

std::uintptr_t g_solo_param_address = 0;

std::uintptr_t GetParamFileBase(std::uintptr_t repo, std::uintptr_t param_offset)
{
    if (!IsReadableMemory(reinterpret_cast<const void*>(repo + param_offset), sizeof(std::uintptr_t))) return 0;
    const auto holder = *reinterpret_cast<const std::uintptr_t*>(repo + param_offset);
    if (!holder || !IsReadableMemory(reinterpret_cast<const void*>(holder + kParamContainerStep1), sizeof(std::uintptr_t))) return 0;
    const auto container = *reinterpret_cast<const std::uintptr_t*>(holder + kParamContainerStep1);
    if (!container || !IsReadableMemory(reinterpret_cast<const void*>(container + kParamContainerStep2), sizeof(std::uintptr_t))) return 0;
    return *reinterpret_cast<const std::uintptr_t*>(container + kParamContainerStep2);
}

std::string ReadParamTypeName(std::uintptr_t base)
{
    if (!base || !IsReadableMemory(reinterpret_cast<const void*>(base + kParamTypeNameOffset), sizeof(std::int32_t))) return {};

    const auto type_name_offset = *reinterpret_cast<const std::int32_t*>(base + kParamTypeNameOffset);
    if (type_name_offset <= 0 || type_name_offset > 1024 * 1024) return {};

    const char* text = reinterpret_cast<const char*>(base + static_cast<std::uintptr_t>(type_name_offset));
    if (!IsReadableMemory(text, 1)) return {};

    std::string result;
    for (std::size_t i = 0; i < 96 && IsReadableMemory(text + i, 1); ++i) {
        if (text[i] == '\0') break;
        if (text[i] < 32 || text[i] > 126) return {};
        result.push_back(text[i]);
    }
    return result;
}

}  // namespace

std::uintptr_t ResolveSoloParamRepository()
{
    if (!g_solo_param_address) {
        const auto addr = FindPattern(kSoloParamRepositoryPattern, kSoloParamRepositoryMask);
        if (!addr) return 0;
        g_solo_param_address = ResolveRipRelative(addr, 3, 7);
    }

    return *reinterpret_cast<const std::uintptr_t*>(g_solo_param_address);
}

std::uintptr_t LocateParamOffsetByType(std::uintptr_t repo, const char* type_name, std::uintptr_t skip_offset)
{
    for (std::uintptr_t offset = 0; offset < 0x1000; offset += sizeof(void*)) {
        if (offset == skip_offset) continue;

        const std::uintptr_t base = GetParamFileBase(repo, offset);
        if (ReadParamTypeName(base) == type_name) return offset;
    }

    return 0;
}

const std::uint8_t* FindRowData(std::uintptr_t repo, std::uintptr_t param_offset, std::uint32_t row_id)
{
    const auto base = GetParamFileBase(repo, param_offset);
    if (!base || !IsReadableMemory(reinterpret_cast<const void*>(base + kParamRowCountOffset), sizeof(std::uint16_t))) return nullptr;

    const auto row_count = *reinterpret_cast<const std::uint16_t*>(base + kParamRowCountOffset);
    if (row_count == 0 || row_count > 10000) return nullptr;
    if (!IsReadableMemory(reinterpret_cast<const void*>(base + kParamRowTableStart), kParamRowStride * row_count)) return nullptr;

    for (std::uint16_t i = 0; i < row_count; ++i) {
        const auto desc = base + kParamRowTableStart + kParamRowStride * i;
        const auto current_row_id = *reinterpret_cast<const std::int32_t*>(desc + kRowDescIdOffset);
        if (current_row_id != static_cast<std::int32_t>(row_id)) continue;

        const auto data_offset = *reinterpret_cast<const std::int64_t*>(desc + kRowDescDataOffset);
        const auto data = base + static_cast<std::uintptr_t>(data_offset);
        return IsReadableMemory(reinterpret_cast<const void*>(data), 0x40) ? reinterpret_cast<const std::uint8_t*>(data) : nullptr;
    }

    return nullptr;
}

}  // namespace radial_spell_menu::param_repository

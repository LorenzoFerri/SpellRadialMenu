#include "spell_metadata.h"

#include "common.h"

#include <MinHook.h>
#include <windows.h>

#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace radial_spell_menu {

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

constexpr std::array<std::uint8_t, 15> kMsgRepositoryInstancePattern = {
    0x48, 0x8B, 0x3D, 0x00, 0x00, 0x00, 0x00,
    0x44, 0x0F, 0xB6, 0x30, 0x48, 0x85, 0xFF, 0x75,
};
constexpr std::array<bool, 15> kMsgRepositoryInstanceMask = {
    true, true, true, false, false, false, false,
    true, true, true, true, true, true, true, true,
};

constexpr std::array<std::uint8_t, 14> kMsgRepositoryLookupPattern = {
    0x8B, 0xDA, 0x44, 0x8B, 0xCA, 0x33, 0xD2,
    0x48, 0x8B, 0xF9, 0x44, 0x8D, 0x42, 0x6F,
};
constexpr std::array<bool, 14> kMsgRepositoryLookupMask = {
    true, true, true, true, true, true, true,
    true, true, true, true, true, true, true,
};

constexpr std::uintptr_t kMagicParamOffset      = 0x478;
constexpr std::uintptr_t kParamContainerStep1   = 0x80;
constexpr std::uintptr_t kParamContainerStep2   = 0x80;
constexpr std::uintptr_t kParamTypeNameOffset   = 0x10;
constexpr std::uintptr_t kParamRowCountOffset   = 0x0A;
constexpr std::uintptr_t kParamRowTableStart    = 0x40;
constexpr std::uintptr_t kParamRowStride        = 0x18;
constexpr std::uintptr_t kRowDescIdOffset       = 0x00;
constexpr std::uintptr_t kRowDescDataOffset     = 0x08;
constexpr std::uintptr_t kMagicReqIntOffset     = 0x22;
constexpr std::uintptr_t kMagicReqFaithOffset   = 0x23;
constexpr std::uintptr_t kGoodsIconIdOffset     = 0x30;
constexpr std::uintptr_t kGoodsTypeOffset       = 0x3E;
constexpr std::uint8_t   kGoodsTypeSorcery = 5;
constexpr std::uint8_t   kGoodsTypeIncantation = 16;
constexpr std::uint8_t   kGoodsTypeSpellTool = 17;
constexpr std::uint8_t   kGoodsTypeSpellBuff = 18;

constexpr unsigned int kGoodsNameCategory     = 0x0A;
constexpr unsigned int kMagicNameCategory     = 0x0E;
constexpr unsigned int kGoodsNameDlc1Category = 0x13F;
constexpr unsigned int kMagicNameDlc1Category = 0x145;
constexpr unsigned int kGoodsNameDlc2Category = 0x1A3;
constexpr unsigned int kMagicNameDlc2Category = 0x1A9;
constexpr unsigned int kMessageVersion    = 0;
constexpr unsigned int kMaxMessageVersion = 3;

using MsgRepositoryLookupFn = const wchar_t* (*)(void*, unsigned int, unsigned int, int);

std::uintptr_t       g_solo_param_address        = 0;
std::uintptr_t       g_msg_repository_address    = 0;
LPVOID               g_msg_lookup_target         = nullptr;
MsgRepositoryLookupFn g_orig_msg_lookup          = nullptr;
MsgRepositoryLookupFn g_direct_msg_lookup        = nullptr;
bool                 g_hook_attempted            = false;
bool                 g_logged_lookup_failure     = false;

std::mutex g_cache_mutex;
std::unordered_map<std::uint32_t, ResolvedSpellMetadata> g_metadata_cache;
std::uintptr_t g_goods_param_offset = 0;
bool g_logged_goods_fallback = false;

const wchar_t* HookedMsgRepositoryLookup(void* repo, unsigned int version, unsigned int category, int msg_id);

std::string Utf8FromWide(const wchar_t* text)
{
    if (!text || !text[0]) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, s.data(), n, nullptr, nullptr);
    s.pop_back();
    return s;
}

bool EnsureMsgHookInstalled()
{
    if (g_hook_attempted) return g_msg_lookup_target != nullptr;
    g_hook_attempted = true;

    const auto stub = FindPattern(kMsgRepositoryLookupPattern, kMsgRepositoryLookupMask);
    if (!stub) return false;

    const auto target = ResolveRipRelative(stub + kMsgRepositoryLookupPattern.size(), 1, 5);
    if (!target) return false;

    g_msg_lookup_target  = reinterpret_cast<LPVOID>(target);
    g_direct_msg_lookup  = reinterpret_cast<MsgRepositoryLookupFn>(target);

    MH_STATUS s = MH_CreateHook(g_msg_lookup_target, reinterpret_cast<LPVOID>(&HookedMsgRepositoryLookup),
                                 reinterpret_cast<LPVOID*>(&g_orig_msg_lookup));
    if (s == MH_OK) MH_EnableHook(g_msg_lookup_target);
    return true;
}

bool IsReadablePointer(const void* ptr, std::size_t bytes)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!ptr || VirtualQuery(ptr, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) || (mbi.Protect & PAGE_NOACCESS)) return false;

    const DWORD protect = mbi.Protect & 0xFFu;
    const bool readable = protect == PAGE_READONLY || protect == PAGE_READWRITE || protect == PAGE_WRITECOPY ||
                          protect == PAGE_EXECUTE_READ || protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
    if (!readable) return false;

    const auto begin = reinterpret_cast<std::uintptr_t>(ptr);
    const auto region_begin = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
    const auto region_end = region_begin + static_cast<std::uintptr_t>(mbi.RegionSize);
    return begin >= region_begin && begin + bytes >= begin && begin + bytes <= region_end;
}

std::uintptr_t GetParamFileBase(std::uintptr_t repo, std::uintptr_t param_offset)
{
    if (!IsReadablePointer(reinterpret_cast<const void*>(repo + param_offset), sizeof(std::uintptr_t))) return 0;
    const auto holder = *reinterpret_cast<const std::uintptr_t*>(repo + param_offset);
    if (!holder || !IsReadablePointer(reinterpret_cast<const void*>(holder + kParamContainerStep1), sizeof(std::uintptr_t))) return 0;
    const auto container = *reinterpret_cast<const std::uintptr_t*>(holder + kParamContainerStep1);
    if (!container || !IsReadablePointer(reinterpret_cast<const void*>(container + kParamContainerStep2), sizeof(std::uintptr_t))) return 0;
    const auto base = *reinterpret_cast<const std::uintptr_t*>(container + kParamContainerStep2);
    return base;
}

std::string ReadParamTypeName(std::uintptr_t base)
{
    if (!base || !IsReadablePointer(reinterpret_cast<const void*>(base + kParamTypeNameOffset), sizeof(std::int32_t))) return {};

    const auto type_name_offset = *reinterpret_cast<const std::int32_t*>(base + kParamTypeNameOffset);
    if (type_name_offset <= 0 || type_name_offset > 1024 * 1024) return {};

    const char* text = reinterpret_cast<const char*>(base + static_cast<std::uintptr_t>(type_name_offset));
    if (!IsReadablePointer(text, 1)) return {};

    std::string result;
    for (std::size_t i = 0; i < 96 && IsReadablePointer(text + i, 1); ++i) {
        if (text[i] == '\0') break;
        if (text[i] < 32 || text[i] > 126) return {};
        result.push_back(text[i]);
    }
    return result;
}

std::uintptr_t LocateEquipParamGoodsOffset(std::uintptr_t repo)
{
    if (g_goods_param_offset) return g_goods_param_offset;

    for (std::uintptr_t offset = 0; offset < 0x1000; offset += sizeof(void*)) {
        if (offset == kMagicParamOffset) continue;

        const std::uintptr_t base = GetParamFileBase(repo, offset);
        if (ReadParamTypeName(base) == "EQUIP_PARAM_GOODS_ST") {
            g_goods_param_offset = offset;
            return offset;
        }
    }

    return 0;
}

const std::uint8_t* FindParamRowData(std::uintptr_t repo, std::uintptr_t param_offset, std::uint32_t row_id)
{
    const auto base = GetParamFileBase(repo, param_offset);
    if (!base || !IsReadablePointer(reinterpret_cast<const void*>(base + kParamRowCountOffset), sizeof(std::uint16_t))) return nullptr;

    const auto row_count = *reinterpret_cast<const std::uint16_t*>(base + kParamRowCountOffset);
    if (row_count == 0 || row_count > 10000) return nullptr;
    if (!IsReadablePointer(reinterpret_cast<const void*>(base + kParamRowTableStart), kParamRowStride * row_count)) return nullptr;

    for (std::uint16_t i = 0; i < row_count; ++i) {
        const auto desc = base + kParamRowTableStart + kParamRowStride * i;
        const auto current_row_id = *reinterpret_cast<const std::int32_t*>(desc + kRowDescIdOffset);
        if (current_row_id != static_cast<std::int32_t>(row_id)) continue;

        const auto data_offset = *reinterpret_cast<const std::int64_t*>(desc + kRowDescDataOffset);
        const auto data = base + static_cast<std::uintptr_t>(data_offset);
        return IsReadablePointer(reinterpret_cast<const void*>(data), 0x40) ? reinterpret_cast<const std::uint8_t*>(data) : nullptr;
    }

    return nullptr;
}

std::uint32_t ReadGoodsIconId(std::uintptr_t repo, std::uint32_t spell_id)
{
    std::uintptr_t offset = LocateEquipParamGoodsOffset(repo);
    if (!offset) {
        for (std::uintptr_t candidate = 0; candidate < 0x1000; candidate += sizeof(void*)) {
            if (candidate == kMagicParamOffset) continue;
            const std::uint8_t* row = FindParamRowData(repo, candidate, spell_id);
            if (!row) continue;

            const std::uint8_t goods_type = row[kGoodsTypeOffset];
            if (goods_type != kGoodsTypeSorcery &&
                goods_type != kGoodsTypeIncantation &&
                goods_type != kGoodsTypeSpellTool &&
                goods_type != kGoodsTypeSpellBuff) continue;

            const auto icon_id = *reinterpret_cast<const std::uint16_t*>(row + kGoodsIconIdOffset);
            if (icon_id == 0) continue;

            offset = candidate;
            g_goods_param_offset = candidate;
            if (!g_logged_goods_fallback) {
                Log("Spell metadata: located EquipParamGoods by spell goods row fallback.");
                g_logged_goods_fallback = true;
            }
            break;
        }
    }
    if (!offset) return 0;

    const std::uint8_t* data = FindParamRowData(repo, offset, spell_id);
    if (!data) return 0;

    const auto icon_id = *reinterpret_cast<const std::uint16_t*>(data + kGoodsIconIdOffset);
    return icon_id != 0 ? static_cast<std::uint32_t>(icon_id) : 0;
}

MsgRepositoryLookupFn GetMsgLookup()
{
    if (!EnsureMsgHookInstalled()) {
        if (!g_logged_lookup_failure) {
            Log("Failed to resolve MsgRepository lookup; spell names will fall back to generic labels.");
            g_logged_lookup_failure = true;
        }
        return nullptr;
    }
    return g_orig_msg_lookup ? g_orig_msg_lookup : g_direct_msg_lookup;
}

void* GetMsgRepositoryInstance()
{
    if (!g_msg_repository_address) {
        const auto addr = FindPattern(kMsgRepositoryInstancePattern, kMsgRepositoryInstanceMask);
        if (!addr) return nullptr;
        g_msg_repository_address = ResolveRipRelative(addr, 3, 7);
    }
    return *reinterpret_cast<void**>(g_msg_repository_address);
}

std::string LookupMagicName(std::uint32_t msg_id)
{
    const auto lookup = GetMsgLookup();
    if (!lookup) return {};
    void* const repo = GetMsgRepositoryInstance();
    if (!repo) return {};

    constexpr unsigned int kCategories[] = {
        kMagicNameCategory,
        kMagicNameDlc1Category,
        kMagicNameDlc2Category,
        kGoodsNameCategory,
        kGoodsNameDlc1Category,
        kGoodsNameDlc2Category,
    };
    for (unsigned int category : kCategories) {
        for (unsigned int version = kMessageVersion; version <= kMaxMessageVersion; ++version) {
            const wchar_t* result = lookup(repo, version, category, static_cast<int>(msg_id));
            std::string name = Utf8FromWide(result);
            if (!name.empty()) return name;
        }
    }
    return {};
}

const wchar_t* HookedMsgRepositoryLookup(void* repo, unsigned int version, unsigned int category, int msg_id)
{
    return g_orig_msg_lookup
        ? g_orig_msg_lookup(repo, version, category, msg_id)
        : g_direct_msg_lookup(repo, version, category, msg_id);
}

struct RuntimeMagicMetadata { std::uint32_t icon_id = 0; SpellCategory category = SpellCategory::unknown; };

RuntimeMagicMetadata ReadRuntimeMagicMetadata(std::uint32_t spell_id)
{
    if (!g_solo_param_address) {
        const auto addr = FindPattern(kSoloParamRepositoryPattern, kSoloParamRepositoryMask);
        if (!addr) return {};
        g_solo_param_address = ResolveRipRelative(addr, 3, 7);
    }

    const auto repo = *reinterpret_cast<const std::uintptr_t*>(g_solo_param_address);
    if (!repo) return {};
    const std::uint8_t* data = FindParamRowData(repo, kMagicParamOffset, spell_id);
    if (data) {

        RuntimeMagicMetadata meta{};
        if (const std::uint32_t goods_icon_id = ReadGoodsIconId(repo, spell_id)) {
            meta.icon_id = goods_icon_id;
        }
        const auto faith = *reinterpret_cast<const std::uint8_t*>(data + kMagicReqFaithOffset);
        const auto intel = *reinterpret_cast<const std::uint8_t*>(data + kMagicReqIntOffset);
        meta.category = faith > 0 ? SpellCategory::incantation
                      : intel > 0 ? SpellCategory::sorcery
                      : SpellCategory::unknown;
        return meta;
    }
    return {};
}

}  // namespace

bool InitializeSpellMetadata()
{
    EnsureMsgHookInstalled();
    return true;
}

ResolvedSpellMetadata ResolveSpellMetadata(std::uint32_t spell_id)
{
    {
        std::lock_guard lock(g_cache_mutex);
        if (const auto it = g_metadata_cache.find(spell_id); it != g_metadata_cache.end()) {
            return it->second;
        }
    }

    const auto runtime = ReadRuntimeMagicMetadata(spell_id);
    std::string name = LookupMagicName(spell_id);
    if (name.empty()) {
        char buf[32] = {};
        std::snprintf(buf, sizeof(buf), "Spell %u", spell_id);
        name = buf;
    }

    ResolvedSpellMetadata metadata{
        .name = std::move(name),
        .icon_id = runtime.icon_id,
        .category = runtime.category,
    };

    std::lock_guard lock(g_cache_mutex);
    g_metadata_cache[spell_id] = metadata;

    return metadata;
}

}  // namespace radial_spell_menu

#include "spell_metadata.h"

#include "common.h"

#include <MinHook.h>
#include <windows.h>

#include <cstdio>
#include <mutex>
#include <unordered_map>

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
constexpr std::uintptr_t kParamRowCountOffset   = 0x0A;
constexpr std::uintptr_t kParamRowTableStart    = 0x40;
constexpr std::uintptr_t kParamRowStride        = 0x18;
constexpr std::uintptr_t kRowDescIdOffset       = 0x00;
constexpr std::uintptr_t kRowDescDataOffset     = 0x08;
constexpr std::uintptr_t kMagicIconIdOffset     = 0x14;
constexpr std::uintptr_t kMagicReqIntOffset     = 0x22;
constexpr std::uintptr_t kMagicReqFaithOffset   = 0x23;
constexpr std::int32_t   kLikelyMaxIconId       = 99999;

constexpr unsigned int kMagicNameCategory = 10;
constexpr unsigned int kMessageVersion    = 0;

using MsgRepositoryLookupFn = const wchar_t* (*)(void*, unsigned int, unsigned int, int);

std::uintptr_t       g_solo_param_address        = 0;
std::uintptr_t       g_msg_repository_address    = 0;
LPVOID               g_msg_lookup_target         = nullptr;
MsgRepositoryLookupFn g_orig_msg_lookup          = nullptr;
MsgRepositoryLookupFn g_direct_msg_lookup        = nullptr;
bool                 g_hook_attempted            = false;
bool                 g_logged_lookup_failure     = false;

std::mutex                                    g_cache_mutex;
std::unordered_map<std::uint32_t, std::string> g_name_cache;

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
    const wchar_t* result = lookup(repo, kMessageVersion, kMagicNameCategory, static_cast<int>(msg_id));
    return Utf8FromWide(result);
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
    const auto holder    = *reinterpret_cast<const std::uintptr_t*>(repo + kMagicParamOffset);
    if (!holder) return {};
    const auto container = *reinterpret_cast<const std::uintptr_t*>(holder + kParamContainerStep1);
    if (!container) return {};
    const auto base      = *reinterpret_cast<const std::uintptr_t*>(container + kParamContainerStep2);
    if (!base) return {};

    const auto row_count = *reinterpret_cast<const std::uint16_t*>(base + kParamRowCountOffset);
    for (std::uint16_t i = 0; i < row_count; ++i) {
        const auto desc    = base + kParamRowTableStart + kParamRowStride * i;
        const auto row_id  = *reinterpret_cast<const std::int32_t*>(desc + kRowDescIdOffset);
        if (row_id != static_cast<std::int32_t>(spell_id)) continue;

        const auto data_offset = *reinterpret_cast<const std::int64_t*>(desc + kRowDescDataOffset);
        const auto data = base + static_cast<std::uintptr_t>(data_offset);

        RuntimeMagicMetadata meta{};
        const auto icon32 = *reinterpret_cast<const std::int32_t*>(data + kMagicIconIdOffset);
        if (icon32 > 0 && icon32 <= kLikelyMaxIconId) {
            meta.icon_id = static_cast<std::uint32_t>(icon32);
        } else {
            const auto icon16 = *reinterpret_cast<const std::int16_t*>(data + kMagicIconIdOffset);
            meta.icon_id = icon16 > 0 ? static_cast<std::uint32_t>(icon16) : 0;
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
        if (const auto it = g_name_cache.find(spell_id); it != g_name_cache.end())
            return { it->second, ReadRuntimeMagicMetadata(spell_id).icon_id,
                     ReadRuntimeMagicMetadata(spell_id).category };
    }

    const auto runtime = ReadRuntimeMagicMetadata(spell_id);
    std::string name = LookupMagicName(spell_id);
    if (name.empty()) {
        char buf[32] = {};
        std::snprintf(buf, sizeof(buf), "Spell %u", spell_id);
        name = buf;
    } else {
        std::lock_guard lock(g_cache_mutex);
        g_name_cache[spell_id] = name;
    }

    return { name, runtime.icon_id, runtime.category };
}

}  // namespace radial_spell_menu

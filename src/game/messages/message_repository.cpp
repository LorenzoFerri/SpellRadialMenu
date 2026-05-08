#include "game/messages/message_repository.h"

#include "core/common.h"

#include <MinHook.h>
#include <windows.h>

#include <array>

namespace radial_spell_menu::message_repository {
namespace {

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

constexpr unsigned int kGoodsNameCategory     = 0x0A;
constexpr unsigned int kMagicNameCategory     = 0x0E;
constexpr unsigned int kGoodsNameDlc1Category = 0x13F;
constexpr unsigned int kMagicNameDlc1Category = 0x145;
constexpr unsigned int kGoodsNameDlc2Category = 0x1A3;
constexpr unsigned int kMagicNameDlc2Category = 0x1A9;
constexpr unsigned int kMessageVersion = 0;
constexpr unsigned int kMaxMessageVersion = 3;

using MsgRepositoryLookupFn = const wchar_t* (*)(void*, unsigned int, unsigned int, int);

std::uintptr_t g_msg_repository_address = 0;
LPVOID g_msg_lookup_target = nullptr;
MsgRepositoryLookupFn g_orig_msg_lookup = nullptr;
MsgRepositoryLookupFn g_direct_msg_lookup = nullptr;
bool g_hook_attempted = false;
bool g_logged_lookup_failure = false;

const wchar_t* HookedMsgRepositoryLookup(void* repo, unsigned int version, unsigned int category, int msg_id)
{
    return g_orig_msg_lookup
        ? g_orig_msg_lookup(repo, version, category, msg_id)
        : g_direct_msg_lookup(repo, version, category, msg_id);
}

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

    g_msg_lookup_target = reinterpret_cast<LPVOID>(target);
    g_direct_msg_lookup = reinterpret_cast<MsgRepositoryLookupFn>(target);

    MH_STATUS status = MH_CreateHook(g_msg_lookup_target, reinterpret_cast<LPVOID>(&HookedMsgRepositoryLookup),
        reinterpret_cast<LPVOID*>(&g_orig_msg_lookup));
    if (status == MH_OK) MH_EnableHook(g_msg_lookup_target);
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

std::string LookupName(std::uint32_t msg_id, const unsigned int* categories, std::size_t category_count)
{
    const auto lookup = GetMsgLookup();
    if (!lookup) return {};
    void* const repo = GetMsgRepositoryInstance();
    if (!repo) return {};

    for (std::size_t i = 0; i < category_count; ++i) {
        for (unsigned int version = kMessageVersion; version <= kMaxMessageVersion; ++version) {
            const wchar_t* result = lookup(repo, version, categories[i], static_cast<int>(msg_id));
            std::string name = Utf8FromWide(result);
            if (!name.empty()) return name;
        }
    }
    return {};
}

}  // namespace

bool Initialize()
{
    EnsureMsgHookInstalled();
    return true;
}

std::string LookupMagicName(std::uint32_t msg_id)
{
    constexpr unsigned int kCategories[] = {
        kMagicNameCategory,
        kMagicNameDlc1Category,
        kMagicNameDlc2Category,
        kGoodsNameCategory,
        kGoodsNameDlc1Category,
        kGoodsNameDlc2Category,
    };
    return LookupName(msg_id, kCategories, std::size(kCategories));
}

std::string LookupGoodsName(std::uint32_t msg_id)
{
    constexpr unsigned int kCategories[] = {
        kGoodsNameCategory,
        kGoodsNameDlc1Category,
        kGoodsNameDlc2Category,
    };
    return LookupName(msg_id, kCategories, std::size(kCategories));
}

}  // namespace radial_spell_menu::message_repository

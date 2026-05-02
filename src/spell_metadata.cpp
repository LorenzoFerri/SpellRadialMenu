#include "spell_metadata.h"

#include "common.h"

#include <MinHook.h>
#include <windows.h>

#include <array>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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

constexpr std::uintptr_t kMagicParamOffset = 0x478;
constexpr std::uintptr_t kParamContainerStep1 = 0x80;
constexpr std::uintptr_t kParamContainerStep2 = 0x80;
constexpr std::uintptr_t kParamRowCountOffset = 0x0A;
constexpr std::uintptr_t kParamRowTableStart = 0x40;
constexpr std::uintptr_t kParamRowStride = 0x18;
constexpr std::uintptr_t kRowDescIdOffset = 0x00;
constexpr std::uintptr_t kRowDescDataOffset = 0x08;
constexpr std::uintptr_t kMagicIconIdOffset = 0x14;
constexpr std::uintptr_t kMagicReqIntOffset = 0x22;
constexpr std::uintptr_t kMagicReqFaithOffset = 0x23;
constexpr std::int32_t kLikelyMaxIconId = 99999;

constexpr unsigned int kMagicNameCategory = 10;
constexpr unsigned int kMessageVersion = 0;

using MsgRepositoryLookupFn = const wchar_t* (*)(void*, unsigned int, unsigned int, int);

struct RuntimeMagicMetadata {
    std::uint32_t icon_id = 0;
    SpellCategory category = SpellCategory::unknown;
};

struct NameTraceState {
    bool armed = false;
    std::uint32_t focused_spell_id = 0;
    std::unordered_set<std::uint32_t> visible_spell_ids;
    std::unordered_map<std::uint32_t, std::wstring> visible_raw_names;
    std::wstring focused_raw_name;
};

std::uintptr_t g_solo_param_repository_address = 0;
std::uintptr_t g_msg_repository_instance_address = 0;
LPVOID g_msg_repository_lookup_target = nullptr;
MsgRepositoryLookupFn g_original_msg_repository_lookup = nullptr;
MsgRepositoryLookupFn g_direct_msg_repository_lookup = nullptr;
bool g_msg_repository_hook_attempted = false;
bool g_msg_repository_hook_installed = false;
bool g_logged_msg_repository_failure = false;

std::mutex g_name_state_mutex;
NameTraceState g_name_trace = {};
std::unordered_map<std::uint32_t, std::uint32_t> g_spell_name_remaps;
std::unordered_map<std::uint32_t, std::string> g_runtime_name_cache;

const wchar_t* HookedMsgRepositoryLookup(void* repository, unsigned int version, unsigned int category, int msg_id);

std::uintptr_t GetModuleBase()
{
    return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
}

std::size_t GetModuleSize()
{
    const auto base = GetModuleBase();
    if (base == 0) {
        return 0;
    }

    auto* const dos_header = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos_header->e_magic != IMAGE_DOS_SIGNATURE) {
        return 0;
    }

    auto* const nt_headers =
        reinterpret_cast<const IMAGE_NT_HEADERS*>(base + static_cast<std::uintptr_t>(dos_header->e_lfanew));
    if (nt_headers->Signature != IMAGE_NT_SIGNATURE) {
        return 0;
    }

    return nt_headers->OptionalHeader.SizeOfImage;
}

template <std::size_t N>
std::uintptr_t FindPattern(
    const std::array<std::uint8_t, N>& pattern,
    const std::array<bool, N>& mask)
{
    const auto module_base = GetModuleBase();
    const auto module_size = GetModuleSize();
    if (module_base == 0 || module_size < pattern.size()) {
        return 0;
    }

    auto* const bytes = reinterpret_cast<const std::uint8_t*>(module_base);
    for (std::size_t i = 0; i <= (module_size - pattern.size()); ++i) {
        bool matches = true;
        for (std::size_t j = 0; j < pattern.size(); ++j) {
            if (mask[j] && bytes[i + j] != pattern[j]) {
                matches = false;
                break;
            }
        }

        if (matches) {
            return module_base + i;
        }
    }

    return 0;
}

std::uintptr_t ResolveRipRelativeTarget(std::uintptr_t instruction_address, std::uintptr_t displacement_offset, std::uintptr_t instruction_size)
{
    const auto displacement = *reinterpret_cast<const std::int32_t*>(instruction_address + displacement_offset);
    return instruction_address + instruction_size + displacement;
}

std::string Utf8FromWide(const wchar_t* text)
{
    if (text == nullptr || text[0] == L'\0') {
        return {};
    }

    const int required = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1) {
        return {};
    }

    std::string utf8(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8.data(), required, nullptr, nullptr);
    utf8.pop_back();
    return utf8;
}

std::uintptr_t ResolveSoloParamRepositoryAddress()
{
    if (g_solo_param_repository_address != 0) {
        return g_solo_param_repository_address;
    }

    const auto pattern_address = FindPattern(kSoloParamRepositoryPattern, kSoloParamRepositoryMask);
    if (pattern_address == 0) {
        return 0;
    }

    g_solo_param_repository_address = ResolveRipRelativeTarget(pattern_address, 3, 7);
    return g_solo_param_repository_address;
}

std::uintptr_t ResolveMsgRepositoryInstanceAddress()
{
    if (g_msg_repository_instance_address != 0) {
        return g_msg_repository_instance_address;
    }

    const auto pattern_address = FindPattern(kMsgRepositoryInstancePattern, kMsgRepositoryInstanceMask);
    if (pattern_address == 0) {
        return 0;
    }

    g_msg_repository_instance_address = ResolveRipRelativeTarget(pattern_address, 3, 7);
    return g_msg_repository_instance_address;
}

void* GetMsgRepositoryInstance()
{
    const auto repository_address = ResolveMsgRepositoryInstanceAddress();
    if (repository_address == 0) {
        return nullptr;
    }

    return *reinterpret_cast<void**>(repository_address);
}

bool EnsureMsgRepositoryHookInstalled()
{
    if (g_msg_repository_hook_attempted) {
        return g_msg_repository_lookup_target != nullptr;
    }

    g_msg_repository_hook_attempted = true;

    const auto lookup_stub = FindPattern(kMsgRepositoryLookupPattern, kMsgRepositoryLookupMask);
    if (lookup_stub == 0) {
        return false;
    }

    const auto lookup_target = ResolveRipRelativeTarget(lookup_stub + kMsgRepositoryLookupPattern.size(), 1, 5);
    if (lookup_target == 0) {
        return false;
    }

    g_msg_repository_lookup_target = reinterpret_cast<LPVOID>(lookup_target);
    g_direct_msg_repository_lookup = reinterpret_cast<MsgRepositoryLookupFn>(g_msg_repository_lookup_target);

    const MH_STATUS create_status = MH_CreateHook(
        g_msg_repository_lookup_target,
        reinterpret_cast<LPVOID>(&HookedMsgRepositoryLookup),
        reinterpret_cast<LPVOID*>(&g_original_msg_repository_lookup));
    if (create_status == MH_OK) {
        const MH_STATUS enable_status = MH_EnableHook(g_msg_repository_lookup_target);
        g_msg_repository_hook_installed = (enable_status == MH_OK);
    }

    return true;
}

MsgRepositoryLookupFn GetMsgRepositoryLookup()
{
    if (!EnsureMsgRepositoryHookInstalled()) {
        if (!g_logged_msg_repository_failure) {
            Log("Failed to resolve MsgRepository lookup; spell names will fall back to generic labels.");
            g_logged_msg_repository_failure = true;
        }
        return nullptr;
    }

    if (g_original_msg_repository_lookup != nullptr) {
        return g_original_msg_repository_lookup;
    }

    return g_direct_msg_repository_lookup;
}

std::wstring LookupMagicNameWide(std::uint32_t msg_id)
{
    const auto lookup = GetMsgRepositoryLookup();
    if (lookup == nullptr) {
        return {};
    }

    void* const repository = GetMsgRepositoryInstance();
    if (repository == nullptr) {
        return {};
    }

    const wchar_t* const result = lookup(repository, kMessageVersion, kMagicNameCategory, static_cast<int>(msg_id));
    if (result == nullptr || result[0] == L'\0') {
        return {};
    }

    return std::wstring(result);
}

std::string LookupMagicNameUtf8(std::uint32_t msg_id)
{
    const std::wstring wide_name = LookupMagicNameWide(msg_id);
    return Utf8FromWide(wide_name.c_str());
}

void LearnSpellNameRemap(std::uint32_t spell_id, std::uint32_t msg_id)
{
    if (spell_id == 0 || msg_id == 0 || spell_id == msg_id) {
        return;
    }

    g_spell_name_remaps[spell_id] = msg_id;
    g_runtime_name_cache.erase(spell_id);
}

void ObserveMagicNameLookup(std::uint32_t msg_id, const wchar_t* text)
{
    if (msg_id == 0 || text == nullptr || text[0] == L'\0') {
        return;
    }

    std::lock_guard<std::mutex> lock(g_name_state_mutex);
    if (!g_name_trace.armed) {
        return;
    }

    if (msg_id == g_name_trace.focused_spell_id) {
        g_name_trace.focused_raw_name.assign(text);
        g_name_trace.visible_raw_names[msg_id] = g_name_trace.focused_raw_name;
        return;
    }

    if (g_name_trace.visible_spell_ids.contains(msg_id)) {
        g_name_trace.visible_raw_names.emplace(msg_id, std::wstring(text));
        return;
    }

    if (!g_name_trace.focused_raw_name.empty() && g_name_trace.focused_raw_name == text) {
        LearnSpellNameRemap(g_name_trace.focused_spell_id, msg_id);
        g_name_trace.armed = false;
        return;
    }

    for (const auto& [spell_id, raw_name] : g_name_trace.visible_raw_names) {
        if (!raw_name.empty() && raw_name == text) {
            LearnSpellNameRemap(spell_id, msg_id);
            break;
        }
    }
}

const wchar_t* HookedMsgRepositoryLookup(void* repository, unsigned int version, unsigned int category, int msg_id)
{
    const wchar_t* const result = g_original_msg_repository_lookup != nullptr
        ? g_original_msg_repository_lookup(repository, version, category, msg_id)
        : g_direct_msg_repository_lookup(repository, version, category, msg_id);

    if (version == kMessageVersion && category == kMagicNameCategory && msg_id > 0) {
        ObserveMagicNameLookup(static_cast<std::uint32_t>(msg_id), result);
    }

    return result;
}

RuntimeMagicMetadata ReadRuntimeMagicMetadata(std::uint32_t spell_id)
{
    const auto repository_address = ResolveSoloParamRepositoryAddress();
    if (repository_address == 0) {
        return {};
    }

    const auto repository = *reinterpret_cast<const std::uintptr_t*>(repository_address);
    if (repository == 0) {
        return {};
    }

    const auto magic_holder = *reinterpret_cast<const std::uintptr_t*>(repository + kMagicParamOffset);
    if (magic_holder == 0) {
        return {};
    }

    const auto magic_container = *reinterpret_cast<const std::uintptr_t*>(magic_holder + kParamContainerStep1);
    if (magic_container == 0) {
        return {};
    }

    const auto param_base = *reinterpret_cast<const std::uintptr_t*>(magic_container + kParamContainerStep2);
    if (param_base == 0) {
        return {};
    }

    const auto row_count = *reinterpret_cast<const std::uint16_t*>(param_base + kParamRowCountOffset);
    for (std::uint16_t i = 0; i < row_count; ++i) {
        const auto row_descriptor = param_base + kParamRowTableStart + (kParamRowStride * i);
        const auto row_id = *reinterpret_cast<const std::int32_t*>(row_descriptor + kRowDescIdOffset);
        if (row_id != static_cast<std::int32_t>(spell_id)) {
            continue;
        }

        const auto row_data_offset = *reinterpret_cast<const std::int64_t*>(row_descriptor + kRowDescDataOffset);
        const auto row_data = param_base + static_cast<std::uintptr_t>(row_data_offset);

        RuntimeMagicMetadata metadata = {};
        // In Elden Ring's MagicParam, iconId is logically int32; reading as int16 can
        // truncate/mod-wrap IDs and cause wrong icon associations in UI overlays.
        const auto raw_icon_id_32 = *reinterpret_cast<const std::int32_t*>(row_data + kMagicIconIdOffset);
        if (raw_icon_id_32 > 0 && raw_icon_id_32 <= kLikelyMaxIconId) {
            metadata.icon_id = static_cast<std::uint32_t>(raw_icon_id_32);
        } else {
            // Keep a compatibility fallback for versions/layouts where iconId may still
            // be packed in a 16-bit field at this offset.
            const auto raw_icon_id_16 = *reinterpret_cast<const std::int16_t*>(row_data + kMagicIconIdOffset);
            metadata.icon_id = raw_icon_id_16 > 0 ? static_cast<std::uint32_t>(raw_icon_id_16) : 0;
        }

        const auto requirement_faith = *reinterpret_cast<const std::uint8_t*>(row_data + kMagicReqFaithOffset);
        const auto requirement_intellect = *reinterpret_cast<const std::uint8_t*>(row_data + kMagicReqIntOffset);
        if (requirement_faith > 0) {
            metadata.category = SpellCategory::incantation;
        } else if (requirement_intellect > 0) {
            metadata.category = SpellCategory::sorcery;
        }

        return metadata;
    }

    return {};
}

std::string ResolveRuntimeSpellName(std::uint32_t spell_id)
{
    {
        std::lock_guard<std::mutex> lock(g_name_state_mutex);
        const auto cached = g_runtime_name_cache.find(spell_id);
        if (cached != g_runtime_name_cache.end()) {
            return cached->second;
        }
    }

    std::uint32_t remapped_id = 0;
    {
        std::lock_guard<std::mutex> lock(g_name_state_mutex);
        const auto remap = g_spell_name_remaps.find(spell_id);
        if (remap != g_spell_name_remaps.end()) {
            remapped_id = remap->second;
        }
    }

    std::string name;
    if (remapped_id != 0) {
        name = LookupMagicNameUtf8(remapped_id);
    }
    if (name.empty()) {
        name = LookupMagicNameUtf8(spell_id);
    }

    if (!name.empty()) {
        std::lock_guard<std::mutex> lock(g_name_state_mutex);
        g_runtime_name_cache[spell_id] = name;
    }

    return name;
}

}  // namespace

bool InitializeSpellMetadata()
{
    EnsureMsgRepositoryHookInstalled();
    return true;
}

void BeginRuntimeMsgLookupTrace(const std::vector<std::uint32_t>& spell_ids, std::uint32_t focused_spell_id)
{
    EnsureMsgRepositoryHookInstalled();

    NameTraceState trace = {};
    trace.armed = focused_spell_id != 0 && !spell_ids.empty();
    trace.focused_spell_id = focused_spell_id;
    trace.visible_spell_ids.reserve(spell_ids.size());
    for (const std::uint32_t spell_id : spell_ids) {
        if (spell_id == 0) {
            continue;
        }

        trace.visible_spell_ids.insert(spell_id);
        const std::wstring raw_name = LookupMagicNameWide(spell_id);
        if (!raw_name.empty()) {
            trace.visible_raw_names.emplace(spell_id, raw_name);
            if (spell_id == focused_spell_id) {
                trace.focused_raw_name = raw_name;
            }
        }
    }

    std::lock_guard<std::mutex> lock(g_name_state_mutex);
    g_name_trace = std::move(trace);
}

ResolvedSpellMetadata ResolveSpellMetadata(std::uint32_t spell_id)
{
    ResolvedSpellMetadata resolved = {};
    const RuntimeMagicMetadata runtime = ReadRuntimeMagicMetadata(spell_id);
    resolved.icon_id = runtime.icon_id;
    resolved.category = runtime.category;

    resolved.name = ResolveRuntimeSpellName(spell_id);
    if (resolved.name.empty()) {
        char name[32] = {};
        std::snprintf(name, sizeof(name), "Spell %u", spell_id);
        resolved.name = name;
    }

    return resolved;
}

}  // namespace radial_spell_menu

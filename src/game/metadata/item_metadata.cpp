#include "game/metadata/item_metadata.h"

#include "core/common.h"
#include "game/messages/message_repository.h"
#include "game/metadata/seamless_coop_metadata.h"
#include "game/params/param_repository.h"

#include <windows.h>

#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace radial_menu_mod {
namespace {

constexpr std::uintptr_t kMagicParamOffset = 0x478;
constexpr std::uintptr_t kGoodsIconIdOffset = 0x30;

std::mutex g_cache_mutex;
std::unordered_map<std::uint32_t, ResolvedItemMetadata> g_item_metadata_cache;
std::uintptr_t g_goods_param_offset = 0;
ULONGLONG g_last_slow_item_metadata_log_ms = 0;

void LogSlowItemMetadata(std::uint32_t item_id, ULONGLONG start_ms, const ResolvedItemMetadata& metadata)
{
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG elapsed = now - start_ms;
    if (elapsed < 16) return;
    if (g_last_slow_item_metadata_log_ms != 0 && now - g_last_slow_item_metadata_log_ms < 2000) return;

    g_last_slow_item_metadata_log_ms = now;
    Log("Timing: ResolveItemMetadata(%u) took %llums (icon=%u).",
        item_id,
        static_cast<unsigned long long>(elapsed),
        metadata.icon_id);
}

std::uintptr_t LocateEquipParamGoodsOffset(std::uintptr_t repo)
{
    if (g_goods_param_offset) return g_goods_param_offset;

    g_goods_param_offset = param_repository::LocateParamOffsetByType(repo, "EQUIP_PARAM_GOODS_ST", kMagicParamOffset);
    return g_goods_param_offset;
}

std::uint32_t ReadAnyGoodsIconId(std::uintptr_t repo, std::uint32_t item_id)
{
    std::uintptr_t offset = LocateEquipParamGoodsOffset(repo);
    if (offset) {
        const std::uint8_t* data = param_repository::FindRowData(repo, offset, item_id);
        if (data) {
            const auto icon_id = *reinterpret_cast<const std::uint16_t*>(data + kGoodsIconIdOffset);
            if (icon_id != 0) return static_cast<std::uint32_t>(icon_id);
        }
    }

    for (std::uintptr_t candidate = 0; candidate < 0x1000; candidate += sizeof(void*)) {
        if (candidate == kMagicParamOffset || candidate == offset) continue;
        const std::uint8_t* row = param_repository::FindRowData(repo, candidate, item_id);
        if (!row) continue;

        const auto icon_id = *reinterpret_cast<const std::uint16_t*>(row + kGoodsIconIdOffset);
        if (icon_id == 0) continue;

        return static_cast<std::uint32_t>(icon_id);
    }

    return 0;
}

}  // namespace

bool InitializeItemMetadata()
{
    message_repository::Initialize();
    return true;
}

ResolvedItemMetadata ResolveItemMetadata(std::uint32_t item_id)
{
    {
        std::lock_guard lock(g_cache_mutex);
        if (const auto it = g_item_metadata_cache.find(item_id); it != g_item_metadata_cache.end()) {
            return it->second;
        }
    }

    const ULONGLONG start = GetTickCount64();
    std::string name = message_repository::LookupGoodsName(item_id);
    if (name.empty()) {
        char buf[32] = {};
        std::snprintf(buf, sizeof(buf), "Item %u", item_id);
        name = buf;
    }

    std::uint32_t icon_id = 0;
    const auto repo = param_repository::ResolveSoloParamRepository();
    if (repo) icon_id = ReadAnyGoodsIconId(repo, item_id);
    if (icon_id == 0) icon_id = seamless_coop_metadata::ResolveIconId(item_id);
    ResolvedItemMetadata metadata{
        .name = std::move(name),
        .icon_id = icon_id,
    };

    {
        std::lock_guard lock(g_cache_mutex);
        g_item_metadata_cache[item_id] = metadata;
    }
    LogSlowItemMetadata(item_id, start, metadata);
    return metadata;
}

}  // namespace radial_menu_mod

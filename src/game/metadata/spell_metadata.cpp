#include "game/metadata/spell_metadata.h"

#include "core/common.h"
#include "game/messages/message_repository.h"
#include "game/params/param_repository.h"

#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace radial_menu_mod {

namespace {

constexpr std::uintptr_t kMagicParamOffset      = 0x478;
constexpr std::uintptr_t kMagicReqIntOffset     = 0x22;
constexpr std::uintptr_t kMagicReqFaithOffset   = 0x23;
constexpr std::uintptr_t kGoodsIconIdOffset     = 0x30;
constexpr std::uintptr_t kGoodsTypeOffset       = 0x3E;
constexpr std::uint8_t   kGoodsTypeSorcery = 5;
constexpr std::uint8_t   kGoodsTypeIncantation = 16;
constexpr std::uint8_t   kGoodsTypeSpellTool = 17;
constexpr std::uint8_t   kGoodsTypeSpellBuff = 18;

std::mutex g_cache_mutex;
std::unordered_map<std::uint32_t, ResolvedSpellMetadata> g_metadata_cache;
std::uintptr_t g_goods_param_offset = 0;
bool g_logged_goods_fallback = false;

std::uintptr_t LocateEquipParamGoodsOffset(std::uintptr_t repo)
{
    if (g_goods_param_offset) return g_goods_param_offset;

    g_goods_param_offset = param_repository::LocateParamOffsetByType(repo, "EQUIP_PARAM_GOODS_ST", kMagicParamOffset);
    return g_goods_param_offset;
}

std::uint32_t ReadGoodsIconId(std::uintptr_t repo, std::uint32_t spell_id)
{
    std::uintptr_t offset = LocateEquipParamGoodsOffset(repo);
    if (!offset) {
        for (std::uintptr_t candidate = 0; candidate < 0x1000; candidate += sizeof(void*)) {
            if (candidate == kMagicParamOffset) continue;
            const std::uint8_t* row = param_repository::FindRowData(repo, candidate, spell_id);
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
            g_logged_goods_fallback = true;
            break;
        }
    }
    if (!offset) return 0;

    const std::uint8_t* data = param_repository::FindRowData(repo, offset, spell_id);
    if (!data) return 0;

    const auto icon_id = *reinterpret_cast<const std::uint16_t*>(data + kGoodsIconIdOffset);
    return icon_id != 0 ? static_cast<std::uint32_t>(icon_id) : 0;
}

struct RuntimeMagicMetadata { std::uint32_t icon_id = 0; SpellCategory category = SpellCategory::unknown; };

RuntimeMagicMetadata ReadRuntimeMagicMetadata(std::uint32_t spell_id)
{
    const auto repo = param_repository::ResolveSoloParamRepository();
    if (!repo) return {};
    const std::uint8_t* data = param_repository::FindRowData(repo, kMagicParamOffset, spell_id);
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
    message_repository::Initialize();
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
    std::string name = message_repository::LookupMagicName(spell_id);
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

}  // namespace radial_menu_mod

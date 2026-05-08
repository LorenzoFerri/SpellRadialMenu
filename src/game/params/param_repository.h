#pragma once

#include <cstdint>

namespace radial_spell_menu::param_repository {

std::uintptr_t ResolveSoloParamRepository();
std::uintptr_t LocateParamOffsetByType(std::uintptr_t repo, const char* type_name, std::uintptr_t skip_offset);
const std::uint8_t* FindRowData(std::uintptr_t repo, std::uintptr_t param_offset, std::uint32_t row_id);

}  // namespace radial_spell_menu::param_repository
